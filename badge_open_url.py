#!/usr/bin/env python3

"""
Open a pasted URL in Mini Browser on the WHY2025 badge.

Requires the customized WHY2025 firmware serial keyboard bridge.

Usage:
    ./badge_open_url.py
    ./badge_open_url.py https://example.com/
    ./badge_open_url.py --clipboard
    ./badge_open_url.py -f urls.txt
    ./badge_open_url.py -f urls.txt -s 10
    ./badge_open_url.py -f urls.txt -z
    ./badge_open_url.py --device /dev/cu.wchusbserial10 https://example.com/

The script sends WHY+E, types the URL through the normal BadgeVMS keyboard
path, and presses Enter.

With -f/--file, URLs are loaded sequentially. Each page remains on screen for
the configured sleep period (30 seconds by default), making the script useful
as a presentation/demo tool.

With -z/--screenshot, a complete full-page screenshot is requested for every
URL in file mode by invoking badge_screenshot.py with --full-page --request.
"""

import argparse
import re
import shutil
import subprocess
import sys
import time
from pathlib import Path
from urllib.parse import urlsplit

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


def detect_device():
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
            "No likely badge serial device found. Use --device DEVICE."
        )

    raise RuntimeError(
        "Multiple serial devices found:\n  "
        + "\n  ".join(candidates)
        + "\nUse --device DEVICE."
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



def load_url_file(filename):
    path = Path(filename)

    if not path.is_file():
        raise ValueError(f"URL file not found: {filename}")

    urls = []

    for line_number, raw_line in enumerate(
        path.read_text(encoding="utf-8").splitlines(),
        start=1,
    ):
        line = raw_line.strip()

        if not line or line.startswith("#"):
            continue

        try:
            urls.append(normalize_url(line))
        except ValueError as exc:
            raise ValueError(
                f"{filename}:{line_number}: {exc}"
            ) from exc

    if not urls:
        raise ValueError(f"No URLs found in: {filename}")

    return urls


def screenshot_script_path(explicit_path=None):
    if explicit_path:
        path = Path(explicit_path).expanduser().resolve()
        if path.is_file():
            return path
        raise RuntimeError(
            f"badge_screenshot.py not found: {explicit_path}"
        )

    local_path = Path(__file__).resolve().with_name(
        "badge_screenshot.py"
    )
    if local_path.is_file():
        return local_path.resolve()

    cwd_path = (Path.cwd() / "badge_screenshot.py").resolve()
    if cwd_path.is_file():
        return cwd_path

    found = shutil.which("badge_screenshot.py")
    if found:
        return Path(found).resolve()

    raise RuntimeError(
        "Could not find badge_screenshot.py. "
        "Put it next to badge_open_url.py or use "
        "--screenshot-script PATH."
    )


def screenshot_filename(index, url):
    candidate = url

    if "://" not in candidate:
        candidate = "https://" + candidate

    parts = urlsplit(candidate)
    host = parts.netloc or "page"
    path = parts.path.strip("/")

    label = host
    if path:
        label += "_" + path

    label = re.sub(r"[^A-Za-z0-9._-]+", "_", label)
    label = label.strip("._-") or "page"

    return f"{index:03d}_{label}.png"


def capture_full_page(
    device,
    screenshot_script,
    output_dir,
    index,
    url,
):
    output_dir.mkdir(parents=True, exist_ok=True)
    output_path = output_dir / screenshot_filename(index, url)

    command = [
        sys.executable,
        str(screenshot_script),
        device,
        "--full-page",
        "--request",
    ]

    # Current badge_screenshot.py versions write their own PNG filename.
    # Run from the requested screenshot directory so all presentation
    # screenshots stay together.
    print(f"Requesting full-page screenshot for: {url}")
    print(f"Screenshot directory: {output_dir}")

    result = subprocess.run(
        command,
        cwd=output_dir,
        check=False,
    )

    if result.returncode != 0:
        raise RuntimeError(
            "badge_screenshot.py failed with exit status "
            f"{result.returncode}"
        )

    # The receiver chooses its own final filename. The generated label is
    # retained only for future compatibility/documentation.
    return output_path


def run_presentation(
    urls,
    device,
    baudrate,
    sleep_seconds,
    take_screenshots,
    screenshot_script,
    screenshot_dir,
):
    total = len(urls)

    print(f"Presentation URLs: {total}")
    print(f"Page sleep:        {sleep_seconds:g} seconds")
    if take_screenshots:
        print("Full screenshots:  enabled")
        print(f"Screenshot dir:    {screenshot_dir}")

    for index, url in enumerate(urls, start=1):
        print()
        print("=" * 60)
        print(f"PAGE {index}/{total}")
        print(url)
        print("=" * 60)

        badge = Badge(device, baudrate)
        try:
            open_url(badge, url)
        finally:
            badge.close()

        print(
            f"Showing page for {sleep_seconds:g} seconds "
            "(Ctrl-C to stop) ..."
        )
        time.sleep(sleep_seconds)

        if take_screenshots:
            capture_full_page(
                device=device,
                screenshot_script=screenshot_script,
                output_dir=screenshot_dir,
                index=index,
                url=url,
            )

    print()
    print("Presentation finished.")



def parse_args():
    parser = argparse.ArgumentParser(
        description=(
            "Paste/send URLs to Mini Browser on the WHY2025 badge, "
            "or run a URL-list presentation."
        )
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
        help="Read one URL directly from the system clipboard.",
    )
    parser.add_argument(
        "-f",
        "--file",
        help=(
            "Read URLs from a text file and load them sequentially. "
            "Blank lines and lines beginning with # are ignored."
        ),
    )
    parser.add_argument(
        "-s",
        "--sleep",
        type=float,
        default=30.0,
        metavar="SECONDS",
        help=(
            "Seconds to show each page in file/presentation mode "
            "(default: 30)."
        ),
    )
    parser.add_argument(
        "-z",
        "--screenshot",
        action="store_true",
        help=(
            "In file mode, request a complete full-page screenshot "
            "of every page after its sleep period."
        ),
    )
    parser.add_argument(
        "--screenshot-dir",
        default="screenshots",
        metavar="DIR",
        help=(
            "Directory used while receiving presentation screenshots "
            "(default: screenshots)."
        ),
    )
    parser.add_argument(
        "--screenshot-script",
        metavar="PATH",
        help=(
            "Path to badge_screenshot.py. By default it is searched "
            "next to this script, in the current directory, then PATH."
        ),
    )
    return parser.parse_args()

def main():
    args = parse_args()

    try:
        if args.sleep < 0:
            raise ValueError("--sleep must be zero or greater.")

        selected_modes = sum(
            bool(value)
            for value in (
                args.url is not None,
                args.clipboard,
                args.file,
            )
        )

        if selected_modes > 1:
            raise ValueError(
                "Use only one input mode: URL, --clipboard, or --file."
            )

        if args.screenshot and not args.file:
            raise ValueError(
                "-z/--screenshot is intended for -f/--file mode."
            )

        device = args.device or detect_device()
        print(f"Serial device: {device}")
        print(f"Baud rate:     {args.baudrate}")

        if args.file:
            urls = load_url_file(args.file)

            shot_script = None
            shot_dir = Path(args.screenshot_dir).expanduser()

            if args.screenshot:
                shot_script = screenshot_script_path(
                    args.screenshot_script
                )

            run_presentation(
                urls=urls,
                device=device,
                baudrate=args.baudrate,
                sleep_seconds=args.sleep,
                take_screenshots=args.screenshot,
                screenshot_script=shot_script,
                screenshot_dir=shot_dir,
            )
            return 0

        if args.clipboard:
            url = clipboard_text()
            print(f"Clipboard URL: {url}")
        elif args.url is not None:
            url = args.url
        else:
            url = input("Paste URL and press Enter: ")

        url = normalize_url(url)

        badge = Badge(device, args.baudrate)
        try:
            open_url(badge, url)
        finally:
            badge.close()

        print("URL sent to Mini Browser.")

    except KeyboardInterrupt:
        print("\nCancelled.", file=sys.stderr)
        return 130
    except (
        ValueError,
        RuntimeError,
        OSError,
        serial.SerialException,
    ) as exc:
        print(f"ERROR: {exc}", file=sys.stderr)
        return 1

    return 0


if __name__ == "__main__":
    sys.exit(main())
