#!/usr/bin/env python3

"""
Open a pasted URL in Mini Browser on the WHY2025 badge.

Requires the customized WHY2025 firmware serial keyboard bridge.

Usage:
    ./badge_open_url.py
    ./badge_open_url.py https://example.com/
    ./badge_open_url.py --clipboard
    ./badge_open_url.py --device /dev/cu.wchusbserial10 https://example.com/

The script sends WHY+E, types the URL through the normal BadgeVMS keyboard
path, and presses Enter.
"""

import argparse
import shutil
import subprocess
import sys
import time

try:
    import serial
    from serial.tools import list_ports
except ImportError:
    print(
        "ERROR: pyserial is required.\n"
        "Install it with:\n"
        "  python3 -m pip install pyserial",
        file=sys.stderr,
    )
    sys.exit(1)

DEFAULT_BAUDRATE = 115200
KEY_DELAY = 0.025
PRESS_DELAY = 0.035
WHY_DELAY = 0.08
AFTER_WHY_DELAY = 0.20

WHY_SCANCODE = 0xE3
ENTER_SCANCODE = 0x28
BACKSPACE_SCANCODE = 0x2A


def ascii_scancode(c):
    if "a" <= c <= "z":
        return 0x04 + ord(c) - ord("a")
    if "A" <= c <= "Z":
        return 0x04 + ord(c) - ord("A")
    if "1" <= c <= "9":
        return 0x1E + ord(c) - ord("1")
    if c == "0":
        return 0x27

    table = {
        " ": 0x2C,
        "-": 0x2D,
        "_": 0x2D,
        "=": 0x2E,
        "+": 0x2E,
        "[": 0x2F,
        "{": 0x2F,
        "]": 0x30,
        "}": 0x30,
        "\\": 0x31,
        "|": 0x31,
        ";": 0x33,
        ":": 0x33,
        "'": 0x34,
        '"': 0x34,
        "`": 0x35,
        "~": 0x35,
        ",": 0x36,
        "<": 0x36,
        ".": 0x37,
        ">": 0x37,
        "/": 0x38,
        "?": 0x38,
        "!": 0x1E,
        "@": 0x1F,
        "#": 0x20,
        "$": 0x21,
        "%": 0x22,
        "^": 0x23,
        "&": 0x24,
        "*": 0x25,
        "(": 0x26,
        ")": 0x27,
    }
    return table.get(c)


class Badge:
    def __init__(self, device, baudrate):
        self.serial = serial.Serial(
            device,
            baudrate,
            timeout=0.1,
            write_timeout=2.0,
        )
        time.sleep(0.25)

    def close(self):
        try:
            self.serial.close()
        except Exception:
            pass

    def send_event(self, scancode, down, text=0):
        command = (
            f"E {scancode:02X} "
            f"{1 if down else 0} "
            f"{text:02X}\n"
        )
        self.serial.write(command.encode("ascii"))
        self.serial.flush()

    def press(self, scancode, text=0, delay=PRESS_DELAY):
        self.send_event(scancode, True, text)
        time.sleep(delay)
        self.send_event(scancode, False, 0)
        time.sleep(delay)

    def why(self, letter):
        letter = letter.upper()
        key_scancode = 0x04 + ord(letter) - ord("A")

        self.send_event(WHY_SCANCODE, True, 0)
        time.sleep(WHY_DELAY)
        self.send_event(key_scancode, True, ord(letter.lower()))
        time.sleep(WHY_DELAY)
        self.send_event(key_scancode, False, 0)
        time.sleep(WHY_DELAY)
        self.send_event(WHY_SCANCODE, False, 0)
        time.sleep(AFTER_WHY_DELAY)

    def backspace(self, count=1):
        for _ in range(count):
            self.press(BACKSPACE_SCANCODE)

    def enter(self):
        self.press(ENTER_SCANCODE)

    def type_text(self, text):
        for character in text:
            codepoint = ord(character)
            if codepoint > 0x7F:
                raise ValueError(
                    f"Non-ASCII URL character is not supported: {character!r}"
                )

            scancode = ascii_scancode(character)
            if scancode is None:
                raise ValueError(f"Unsupported URL character: {character!r}")

            self.press(scancode, codepoint)
            time.sleep(KEY_DELAY)


def clipboard_text():
    commands = []
    if sys.platform == "darwin":
        commands.append(["pbpaste"])
    else:
        commands.extend(
            [
                ["wl-paste", "--no-newline"],
                ["xclip", "-selection", "clipboard", "-o"],
                ["xsel", "--clipboard", "--output"],
            ]
        )

    for command in commands:
        if shutil.which(command[0]) is None:
            continue
        try:
            result = subprocess.run(
                command,
                check=True,
                stdout=subprocess.PIPE,
                stderr=subprocess.DEVNULL,
                text=True,
            )
            text = result.stdout.strip()
            if text:
                return text
        except (OSError, subprocess.CalledProcessError):
            pass

    raise RuntimeError(
        "Could not read the clipboard. Paste the URL at the prompt instead."
    )



def detect_badge_device(explicit=None):
    """Return the WHY2025 badge serial device, or fail clearly if ambiguous."""
    if explicit:
        return explicit

    candidates = []
    for port in list_ports.comports():
        device = port.device
        if sys.platform == "darwin":
            if (
                device.startswith("/dev/cu.wchusbserial")
                or device.startswith("/dev/cu.usbserial")
                or device.startswith("/dev/cu.usbmodem")
            ):
                candidates.append(device)
        else:
            if (
                device.startswith("/dev/ttyUSB")
                or device.startswith("/dev/ttyACM")
            ):
                candidates.append(device)

    candidates = sorted(set(candidates))

    if len(candidates) == 1:
        return candidates[0]
    if not candidates:
        raise RuntimeError(
            "WHY2025 badge serial device not found. "
            "Connect the badge or specify the device explicitly."
        )
    raise RuntimeError(
        "Multiple possible badge serial devices found:\n  "
        + "\n  ".join(candidates)
        + "\nSpecify the device explicitly."
    )

def normalize_url(value):
    url = value.strip()

    if not url:
        raise ValueError("URL is empty.")
    if "\n" in url or "\r" in url:
        raise ValueError("URL must be a single line.")
    if len(url.encode("utf-8")) > 255:
        raise ValueError("URL is longer than Mini Browser allows.")

    lower = url.lower()
    if "://" in url and not (
        lower.startswith("https://") or lower.startswith("http://")
    ):
        raise ValueError("Mini Browser only supports HTTP and HTTPS URLs.")

    return url


def open_url(badge, url):
    print("Sending WHY+E ...")
    badge.why("E")

    lower = url.lower()

    if lower.startswith("https://"):
        # WHY+E already seeds the editor with "https://".
        text_to_type = url[8:]
    elif lower.startswith("http://"):
        # WHY+E seeds "https://". Remove it and type the full HTTP URL.
        badge.backspace(8)
        text_to_type = url
    else:
        # No scheme: keep the seeded "https://".
        text_to_type = url

    print(f"Typing: {url}")
    badge.type_text(text_to_type)

    print("Pressing Enter ...")
    badge.enter()


def parse_args():
    parser = argparse.ArgumentParser(
        description="Paste/send a URL to Mini Browser on the WHY2025 badge."
    )
    parser.add_argument(
        "url",
        nargs="?",
        help="URL to open. If omitted, paste it at the prompt.",
    )
    parser.add_argument(
        "-d",
        "--device",
        help=(
            "Badge serial device. If omitted, a likely macOS/Linux "
            "serial device is auto-detected."
        ),
    )
    parser.add_argument(
        "-b",
        "--baudrate",
        type=int,
        default=DEFAULT_BAUDRATE,
        help=f"Serial baud rate (default: {DEFAULT_BAUDRATE}).",
    )
    parser.add_argument(
        "-c",
        "--clipboard",
        action="store_true",
        help="Read the URL directly from the system clipboard.",
    )
    return parser.parse_args()


def main():
    args = parse_args()

    try:
        if args.clipboard:
            if args.url is not None:
                raise ValueError(
                    "Use either a URL argument or --clipboard, not both."
                )
            url = clipboard_text()
            print(f"Clipboard URL: {url}")
        elif args.url is not None:
            url = args.url
        else:
            url = input("Paste URL and press Enter: ")

        url = normalize_url(url)

        device = detect_badge_device(args.device)
        print(f"Serial device: {device}")
        print(f"Baud rate:     {args.baudrate}")

        badge = Badge(device, args.baudrate)
        try:
            open_url(badge, url)
        finally:
            badge.close()

        print("URL sent to Mini Browser.")

    except KeyboardInterrupt:
        print("\nCancelled.", file=sys.stderr)
        return 130
    except (ValueError, RuntimeError, serial.SerialException) as exc:
        print(f"ERROR: {exc}", file=sys.stderr)
        return 1

    return 0


if __name__ == "__main__":
    sys.exit(main())
