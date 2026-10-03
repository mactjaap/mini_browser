#!/usr/bin/env python3
"""
Mini Browser 3.0 image torture test v2.

This is deliberately verbose.  It tells you:
  * which display mode is selected
  * which test is running
  * which URL is being typed
  * what should appear on the badge
  * what serial message it is waiting for
  * when the image is being left visible for inspection
  * whether browser recovery succeeded

It uses the same important URL-entry strategy as the normal regression suite:
WHY+E seeds "https://", so ONLY host/path is typed.  URL entry is retried when
BadgeVMS reports a dropped injected key event.

Examples:
  python3 mini-browser-3.0-image-torture-v2.py --mode 3
  python3 mini-browser-3.0-image-torture-v2.py --mode 4
  python3 mini-browser-3.0-image-torture-v2.py --mode both
  python3 mini-browser-3.0-image-torture-v2.py --list
  python3 mini-browser-3.0-image-torture-v2.py --mode 3 --test 1
  python3 mini-browser-3.0-image-torture-v2.py --mode 4 --test 1-5

Requires:
  pip install pyserial
"""

import argparse
import re
import sys
import time
import threading

import serial
from serial.tools import list_ports


CONFIG = {
    "serial": {
        "device": None,
        "baudrate": 115200,
        "startup_delay": 2.0,
        "default_timeout": 90,
    },

    # Leave each successfully rendered image/page visible so you can SEE it.
    "show_seconds": 5.0,

    "home_pattern": r"HTTP 200.*https://minibrowser\.macip\.net",

    "modes": {
        3: {
            "name": "Colors + Image",
            "expect": r"\[mini_browser\] display mode: Colors \+ Image",
            "max_inline": 1,
        },
        4: {
            "name": "Colors + 5 Images (Experimental)",
            "expect": r"\[mini_browser\] display mode: Colors \+ 5 Images \(Experimental\)",
            "max_inline": 5,
        },
    },

    # kind:
    #   direct = image must decode successfully
    #   page   = HTML page must contain/render image(s)
    #   safe   = deliberately large image: decode OR bounded rejection is OK
    #
    # The known-good images are deliberately repeated after heavier tests.
    "tests": [
        ("PNG baseline", "direct", "httpbin.io/image/png",
         "A PNG should open in Mini Browser's Image Viewer."),
        ("JPEG baseline", "direct", "httpbin.io/image/jpeg",
         "A JPEG should open in Mini Browser's Image Viewer."),
        ("GIF baseline", "direct",
         "upload.wikimedia.org/wikipedia/commons/d/de/Ajax-loader.gif",
         "A GIF should open; Mini Browser renders its first frame."),
        ("Controlled Phase 3 image page", "page",
         "minibrowser.macip.net/phase3-final-images.html",
         "Mode 3 should show 1 inline image; Mode 4 should show up to 5."),
        ("tjaap.is real-world page", "page", "tjaap.is/",
         "The real page should render safely; its image may be bounded/rejected."),

        ("PNG checkpoint", "direct", "httpbin.io/image/png",
         "Known-good PNG after the first five tests."),
        ("JPEG checkpoint", "direct", "httpbin.io/image/jpeg",
         "Known-good JPEG after the first five tests."),
        ("Controlled image page repeat 1", "page",
         "minibrowser.macip.net/phase3-final-images.html",
         "Repeat the controlled multi-image page without rebooting."),
        ("Huge tjaap.is PNG", "safe", "tjaap.is/higres-tjaap.is.png",
         "This 3840x2750 PNG may be REJECTED. It must NOT crash Mini Browser."),
        ("PNG recovery after huge image", "direct", "httpbin.io/image/png",
         "Known-good PNG must still work after the huge image."),

        ("JPEG recovery after huge image", "direct", "httpbin.io/image/jpeg",
         "Known-good JPEG must still work after the huge image."),
        ("GIF recovery after huge image", "direct",
         "upload.wikimedia.org/wikipedia/commons/d/de/Ajax-loader.gif",
         "Known-good GIF must still work after the huge image."),
        ("Controlled image page repeat 2", "page",
         "minibrowser.macip.net/phase3-final-images.html",
         "Second multi-image-page repetition."),
        ("PNG stress 1", "direct", "httpbin.io/image/png",
         "Repeated PNG decode without reboot."),
        ("PNG stress 2", "direct", "httpbin.io/image/png",
         "Repeated PNG decode without reboot."),

        ("PNG stress 3", "direct", "httpbin.io/image/png",
         "Repeated PNG decode without reboot."),
        ("JPEG stress 1", "direct", "httpbin.io/image/jpeg",
         "Repeated JPEG decode without reboot."),
        ("JPEG stress 2", "direct", "httpbin.io/image/jpeg",
         "Repeated JPEG decode without reboot."),
        ("GIF stress 1", "direct",
         "upload.wikimedia.org/wikipedia/commons/d/de/Ajax-loader.gif",
         "Repeated GIF decode without reboot."),
        ("GIF stress 2", "direct",
         "upload.wikimedia.org/wikipedia/commons/d/de/Ajax-loader.gif",
         "Repeated GIF decode without reboot."),

        ("Controlled image page repeat 3", "page",
         "minibrowser.macip.net/phase3-final-images.html",
         "Third controlled multi-image-page repetition."),
        ("tjaap.is page repeat", "page", "tjaap.is/",
         "Repeat the real-world page late in the run."),
        ("Huge tjaap.is PNG second pass", "safe",
         "tjaap.is/higres-tjaap.is.png",
         "Second large-image safety test. Rejection is acceptable; crash is not."),
        ("Final JPEG recovery", "direct", "httpbin.io/image/jpeg",
         "Final known-good JPEG recovery check."),
        ("Final PNG recovery", "direct", "httpbin.io/image/png",
         "Final known-good PNG recovery check."),
    ],
}

DEFAULT_TIMEOUT = CONFIG["serial"]["default_timeout"]


def banner(title, char="="):
    width = 72
    print("\n" + char * width, file=sys.stderr)
    print(title, file=sys.stderr)
    print(char * width, file=sys.stderr)


def step(text):
    print(f"\n>>> {text}", file=sys.stderr)


def detect_badge_device(explicit=None):
    if explicit:
        return explicit

    candidates = []
    for port in list_ports.comports():
        d = port.device
        if sys.platform == "darwin":
            if d.startswith(("/dev/cu.wchusbserial",
                             "/dev/cu.usbserial",
                             "/dev/cu.usbmodem")):
                candidates.append(d)
        else:
            if d.startswith(("/dev/ttyUSB", "/dev/ttyACM")):
                candidates.append(d)

    candidates = sorted(set(candidates))

    if len(candidates) == 1:
        return candidates[0]
    if not candidates:
        raise RuntimeError("WHY2025 badge serial device not found")
    raise RuntimeError(
        "Multiple possible badge devices found: "
        + ", ".join(candidates)
        + ". Set CONFIG['serial']['device'] explicitly."
    )


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
        " ": 0x2C, "-": 0x2D, "_": 0x2D, "=": 0x2E, "+": 0x2E,
        "[": 0x2F, "{": 0x2F, "]": 0x30, "}": 0x30, "\\": 0x31,
        "|": 0x31, ";": 0x33, ":": 0x33, "'": 0x34, '"': 0x34,
        "`": 0x35, "~": 0x35, ",": 0x36, "<": 0x36, ".": 0x37,
        ">": 0x37, "/": 0x38, "?": 0x38, "!": 0x1E, "@": 0x1F,
        "#": 0x20, "$": 0x21, "%": 0x22, "^": 0x23, "&": 0x24,
        "*": 0x25, "(": 0x26, ")": 0x27,
    }
    return table.get(c)


class Badge:
    def __init__(self, device):
        self.serial = serial.Serial(
            device,
            CONFIG["serial"]["baudrate"],
            timeout=0.1,
        )
        self.running = True
        self.lock = threading.Lock()
        self.lines = []
        self.reader = threading.Thread(target=self._reader, daemon=True)
        self.reader.start()

    def _reader(self):
        buf = bytearray()
        while self.running:
            try:
                data = self.serial.read(1024)
                if not data:
                    continue

                # Keep the badge's real serial output visible.
                sys.stdout.buffer.write(data)
                sys.stdout.buffer.flush()

                buf.extend(data)
                while b"\n" in buf:
                    raw, _, buf = buf.partition(b"\n")
                    line = raw.decode("utf-8", errors="replace").rstrip("\r")
                    with self.lock:
                        self.lines.append(line)
                        if len(self.lines) > 8000:
                            self.lines = self.lines[-5000:]
            except Exception as exc:
                if self.running:
                    print(f"\n[SERIAL ERROR] {exc}", file=sys.stderr)
                return

    def close(self):
        self.running = False
        try:
            self.serial.close()
        except Exception:
            pass

    def clear_log(self):
        with self.lock:
            self.lines.clear()

    def get_lines(self):
        with self.lock:
            return list(self.lines)

    def send_event(self, scancode, down, text=0):
        cmd = f"E {scancode:02X} {1 if down else 0} {text:02X}\n"
        self.serial.write(cmd.encode("ascii"))
        self.serial.flush()

    def press(self, scancode, text=0, delay=0.10):
        self.send_event(scancode, True, text)
        time.sleep(delay)
        self.send_event(scancode, False, 0)
        time.sleep(delay)

    def enter(self):
        self.press(0x28)

    def why(self, letter):
        letter = letter.upper()
        why_scancode = 0xE3
        key_scancode = 0x04 + ord(letter) - ord("A")

        self.send_event(why_scancode, True, 0)
        time.sleep(0.15)
        self.send_event(key_scancode, True, ord(letter.lower()))
        time.sleep(0.15)
        self.send_event(key_scancode, False, 0)
        time.sleep(0.15)
        self.send_event(why_scancode, False, 0)
        time.sleep(0.50)

    def settle(self, seconds=0.5):
        self.serial.flush()
        time.sleep(seconds)

    def type_text(self, text, key_delay=0.15):
        # Same conservative timing used by the normal regression tester.
        for character in text:
            scancode = ascii_scancode(character)
            if scancode is None:
                raise ValueError(f"Unsupported character: {character!r}")
            self.press(scancode, ord(character), delay=0.10)
            time.sleep(key_delay)

    def wait_for(self, pattern, timeout=None, label=None):
        timeout = timeout or DEFAULT_TIMEOUT
        regex = re.compile(pattern)
        deadline = time.monotonic() + timeout
        checked = 0

        if label:
            step(f"WAITING: {label}")
        else:
            step(f"WAITING FOR: {pattern}")

        while time.monotonic() < deadline:
            current = self.get_lines()
            for line in current[checked:]:
                if regex.search(line):
                    print(f"    MATCH: {line}", file=sys.stderr)
                    return line
            checked = len(current)
            time.sleep(0.05)

        raise TimeoutError(label or f"Timeout waiting for: {pattern}")

    def wait_for_any(self, patterns, timeout=45, label="image result"):
        compiled = [(name, re.compile(p)) for name, p in patterns]
        deadline = time.monotonic() + timeout
        checked = 0
        step(f"WAITING: {label}")

        while time.monotonic() < deadline:
            current = self.get_lines()
            for line in current[checked:]:
                for name, regex in compiled:
                    if regex.search(line):
                        print(f"    MATCH [{name}]: {line}", file=sys.stderr)
                        return name, line
            checked = len(current)
            time.sleep(0.05)

        raise TimeoutError(f"Timeout waiting for {label}")


def go_home(badge):
    step("Returning to Mini Browser Home with WHY+H")
    badge.clear_log()
    badge.why("H")
    badge.wait_for(
        CONFIG["home_pattern"],
        30,
        "HTTP 200 from Mini Browser home page",
    )
    badge.settle(0.8)


def set_display_mode(badge, mode):
    cfg = CONFIG["modes"][mode]

    banner(f"SELECT DISPLAY MODE {mode}: {cfg['name']}", "-")
    go_home(badge)

    step("Opening Display Mode menu with WHY+O")
    badge.clear_log()
    badge.why("O")
    badge.settle(0.3)

    step(f"Selecting option {mode}")
    badge.type_text(str(mode))

    # Unlike v1, DO NOT continue blindly.  Prove the mode really changed.
    badge.wait_for(
        cfg["expect"],
        10,
        f"Mini Browser confirms display mode '{cfg['name']}'",
    )
    badge.settle(0.8)


def open_url_robust(badge, url, expected_patterns, max_attempts=3):
    """
    This mirrors the normal regression suite's important URL-entry behavior.

    WHY+E already seeds "https://".  We therefore type only host/path.
    If BadgeVMS rejects an injected event, retry the complete entry.
    """
    typed_url = url[8:] if url.startswith("https://") else url

    for attempt in range(1, max_attempts + 1):
        banner(f"OPEN URL - ATTEMPT {attempt}/{max_attempts}", ".")

        go_home(badge)

        step("Opening URL entry with WHY+E")
        badge.clear_log()
        badge.why("E")
        badge.settle(0.8)

        step("WHY+E has already inserted: https://")
        step(f"Typing ONLY host/path: {typed_url}")

        # Exactly as the regression tester: clear previous WHY+E chatter so
        # dropped events during URL typing are easy to detect.
        badge.clear_log()
        badge.type_text(typed_url)
        badge.settle(0.5)

        dropped = [
            line for line in badge.get_lines()
            if "Unable to send event to task" in line
        ]

        if dropped:
            print("    Keyboard event was rejected by BadgeVMS.", file=sys.stderr)
            print("    Retrying the ENTIRE URL entry.", file=sys.stderr)
            badge.settle(1.0)
            continue

        step("Pressing ENTER to load the URL")
        badge.enter()
        badge.settle(0.6)

        compiled = [(name, re.compile(p)) for name, p in expected_patterns]
        deadline = time.monotonic() + DEFAULT_TIMEOUT
        checked = 0
        transport_failure = None

        step("Watching serial output for the requested URL/result")

        while time.monotonic() < deadline:
            current = badge.get_lines()

            for line in current[checked:]:
                if "Unable to send event to task" in line:
                    transport_failure = (
                        "BadgeVMS rejected an injected keyboard event"
                    )
                    break

                # This is the exact failure seen in the previous script.
                if "[mini_browser] fetch error 2 URL='https://'" in line:
                    transport_failure = (
                        "URL field contained only https://; hostname was not "
                        "entered successfully"
                    )
                    break

                if "[mini_browser] fetch error 6 URL=" in line:
                    transport_failure = (
                        "URL/DNS failure after keyboard entry: " + line
                    )
                    break

                for name, rx in compiled:
                    if rx.search(line):
                        print(f"    MATCH [{name}]: {line}", file=sys.stderr)
                        return name, line

            if transport_failure:
                break

            checked = len(current)
            time.sleep(0.05)

        if transport_failure:
            print(f"    URL ENTRY FAILURE: {transport_failure}", file=sys.stderr)
        else:
            print("    Expected URL result was not seen.", file=sys.stderr)

        if attempt < max_attempts:
            print("    Retrying from Home...", file=sys.stderr)
            badge.settle(1.0)

    raise RuntimeError(
        f"Could not reliably open {url} after {max_attempts} attempts"
    )


def show_picture_pause(description):
    seconds = CONFIG["show_seconds"]
    banner("LOOK AT THE BADGE NOW", "*")
    print(f"{description}", file=sys.stderr)
    print(
        f"The result will stay on screen for {seconds:.0f} seconds.",
        file=sys.stderr,
    )
    time.sleep(seconds)


def recovery_probe(badge):
    banner("RECOVERY CHECK", "-")
    step("Sending WHY+B (returns from Image Viewer when applicable)")
    badge.clear_log()
    badge.why("B")
    badge.settle(0.5)

    step("Now loading Home with WHY+H to prove Mini Browser is still alive")
    go_home(badge)
    print("    RECOVERY OK: Mini Browser is still responding.", file=sys.stderr)


def run_direct(badge, url, safe, picture_description):
    expected = [
        ("direct-image",
         r"\[mini_browser\] direct image: .*URL="),
        ("decoded",
         r"\[mini_browser\] image: decoded source=\d+x\d+ "
         r"retained=\d+x\d+ RGB565=\d+ bytes"),
        ("budget",
         r"\[mini_browser\] image: rejected \d+x\d+: "
         r"RGB decode would need \d+ bytes"),
        ("dimensions",
         r"\[mini_browser\] image: rejected dimensions \d+x\d+"),
        ("unsupported",
         r"\[mini_browser\] image: unsupported, invalid, "
         r"or outside memory budget"),
    ]

    # First prove URL entry reached the image path.  A decode/rejection can
    # race ahead, so all useful image diagnostics are accepted here.
    first, first_line = open_url_robust(badge, url, expected)

    if first == "decoded":
        outcome, line = first, first_line
    elif first in ("budget", "dimensions", "unsupported"):
        outcome, line = first, first_line
    else:
        outcome, line = badge.wait_for_any(
            [
                ("decoded",
                 r"\[mini_browser\] image: decoded source=\d+x\d+ "
                 r"retained=\d+x\d+ RGB565=\d+ bytes"),
                ("budget",
                 r"\[mini_browser\] image: rejected \d+x\d+: "
                 r"RGB decode would need \d+ bytes"),
                ("dimensions",
                 r"\[mini_browser\] image: rejected dimensions \d+x\d+"),
                ("unsupported",
                 r"\[mini_browser\] image: unsupported, invalid, "
                 r"or outside memory budget"),
                ("fetch_failed",
                 r"\[mini_browser\] image: fetch failed"),
            ],
            45,
            "image decode or safe rejection",
        )

    if outcome == "fetch_failed":
        raise RuntimeError(line)

    if not safe and outcome != "decoded":
        raise RuntimeError(
            f"Known-good image did not decode; outcome={outcome}: {line}"
        )

    if outcome == "decoded":
        show_picture_pause(picture_description)
    else:
        banner("SAFE IMAGE REJECTION", "*")
        print(f"Outcome: {outcome}", file=sys.stderr)
        print(f"Serial:  {line}", file=sys.stderr)
        print(
            "This test is allowed to reject the image. "
            "The browser must remain alive.",
            file=sys.stderr,
        )
        time.sleep(2)

    recovery_probe(badge)
    return f"outcome={outcome}; recovery=OK"


def run_page(badge, url, mode, picture_description):
    cfg = CONFIG["modes"][mode]

    # For HTML pages, wait for the final 200 URL.
    open_url_robust(
        badge,
        url,
        [("HTTP-200", r"HTTP 200.*https://")],
    )

    line = badge.wait_for(
        r"\[mini_browser\] display: mode=.* "
        r"images_seen=[1-9]\d* images_retained=\d+ images_loaded=\d+",
        45,
        "page renderer image summary",
    )

    m = re.search(
        r"images_seen=(\d+) images_retained=(\d+) images_loaded=(\d+)",
        line,
    )
    if not m:
        raise RuntimeError("Could not parse image counters: " + line)

    seen, retained, loaded = map(int, m.groups())

    print("", file=sys.stderr)
    print(f"    images_seen     = {seen}", file=sys.stderr)
    print(f"    images_retained = {retained}", file=sys.stderr)
    print(f"    images_loaded   = {loaded}", file=sys.stderr)
    print(f"    mode inline max = {cfg['max_inline']}", file=sys.stderr)

    if loaded > cfg["max_inline"]:
        raise RuntimeError(
            f"Mode {mode} loaded {loaded} inline images; "
            f"expected at most {cfg['max_inline']}"
        )

    show_picture_pause(picture_description)
    recovery_probe(badge)

    return (
        f"seen={seen}, retained={retained}, loaded={loaded}; recovery=OK"
    )


def parse_selection(spec, maximum):
    selected = set()
    for item in spec.split(","):
        item = item.strip()
        if not item:
            continue
        if "-" in item:
            a, b = item.split("-", 1)
            a, b = int(a), int(b)
            if a < 1 or b < a or b > maximum:
                raise ValueError(f"Invalid test range: {item}")
            selected.update(range(a, b + 1))
        else:
            n = int(item)
            if n < 1 or n > maximum:
                raise ValueError(f"Invalid test number: {n}")
            selected.add(n)
    if not selected:
        raise ValueError("No tests selected")
    return selected


def parse_args():
    p = argparse.ArgumentParser(
        description="Verbose Mini Browser 3.0 image torture tester",
        formatter_class=argparse.RawDescriptionHelpFormatter,
    )
    p.add_argument(
        "--mode",
        choices=("3", "4", "both"),
        default="both",
        help="display mode to test (default: both)",
    )
    p.add_argument(
        "--list",
        action="store_true",
        help="list all 25 tests and exit",
    )
    p.add_argument(
        "-t", "--test",
        metavar="N[,N|N-M...]",
        help="run selected tests, e.g. 1 or 1,3,5 or 20-25",
    )
    p.add_argument(
        "--show-seconds",
        type=float,
        default=CONFIG["show_seconds"],
        help="seconds to leave each rendered picture/page visible (default: 5)",
    )
    return p.parse_args()


def print_summary(results):
    banner("MINI BROWSER 3.0 IMAGE TORTURE TEST SUMMARY")

    for r in results:
        status = "PASS" if r["passed"] else "NOT PASSED"
        print(
            f"{r['case']:2d}. {status:<10} Mode {r['mode']} - {r['name']}",
            file=sys.stderr,
        )
        print(f"    {r['detail']}", file=sys.stderr)

    passed = sum(1 for r in results if r["passed"])
    total = len(results)

    print("-" * 72, file=sys.stderr)
    print(f"Passed:     {passed}/{total}", file=sys.stderr)
    print(f"Not passed: {total - passed}/{total}", file=sys.stderr)
    print("=" * 72, file=sys.stderr)


def main():
    args = parse_args()
    CONFIG["show_seconds"] = max(0.0, args.show_seconds)

    if args.list:
        for n, (name, kind, url, picture) in enumerate(CONFIG["tests"], 1):
            print(f"{n:2d}. {name}")
            print(f"    kind: {kind}")
            print(f"    URL : https://{url}")
            print(f"    see : {picture}")
        return 0

    selected = None
    if args.test:
        try:
            selected = parse_selection(args.test, len(CONFIG["tests"]))
        except ValueError as exc:
            print(f"Error: {exc}", file=sys.stderr)
            return 2

    modes = [3, 4] if args.mode == "both" else [int(args.mode)]

    try:
        device = detect_badge_device(CONFIG["serial"]["device"])
    except Exception as exc:
        print(f"Error: {exc}", file=sys.stderr)
        return 2

    banner("MINI BROWSER 3.0 IMAGE TORTURE TEST v2")
    print(f"Serial device : {device}", file=sys.stderr)
    print(f"Baud rate     : {CONFIG['serial']['baudrate']}", file=sys.stderr)
    print(
        "Modes         : "
        + ", ".join(str(m) for m in modes),
        file=sys.stderr,
    )
    print(
        f"Picture time  : {CONFIG['show_seconds']:.1f} seconds",
        file=sys.stderr,
    )
    print(
        "IMPORTANT: the test intentionally does NOT reboot between cases.",
        file=sys.stderr,
    )

    badge = Badge(device)
    results = []

    try:
        time.sleep(CONFIG["serial"]["startup_delay"])

        step("Initial browser check: pressing ENTER once, then WHY+H")
        badge.enter()
        badge.settle(1.0)
        go_home(badge)

        for mode in modes:
            banner(
                f"BEGIN MODE {mode}: {CONFIG['modes'][mode]['name']}",
                "#",
            )

            for case_no, (name, kind, url, picture) in enumerate(
                CONFIG["tests"], 1
            ):
                if selected is not None and case_no not in selected:
                    continue

                banner(
                    f"TEST {case_no}/25 - MODE {mode} - {name}",
                    "=",
                )
                print(f"URL:      https://{url}", file=sys.stderr)
                print(f"TYPE:     {kind}", file=sys.stderr)
                print(f"EXPECTED: {picture}", file=sys.stderr)

                try:
                    set_display_mode(badge, mode)

                    if kind == "page":
                        detail = run_page(
                            badge, url, mode, picture
                        )
                    else:
                        detail = run_direct(
                            badge,
                            url,
                            safe=(kind == "safe"),
                            picture_description=picture,
                        )

                    results.append({
                        "case": case_no,
                        "mode": mode,
                        "name": name,
                        "passed": True,
                        "detail": detail,
                    })
                    banner(f"PASS - TEST {case_no}: {name}", "+")

                except Exception as exc:
                    results.append({
                        "case": case_no,
                        "mode": mode,
                        "name": name,
                        "passed": False,
                        "detail": str(exc),
                    })
                    banner(f"NOT PASSED - TEST {case_no}: {name}", "!")
                    print(f"Reason: {exc}", file=sys.stderr)
                    print(
                        "The script will continue so later recovery tests "
                        "can show whether the browser is still usable.",
                        file=sys.stderr,
                    )

        print_summary(results)
        return 0 if all(r["passed"] for r in results) else 1

    finally:
        badge.close()


if __name__ == "__main__":
    sys.exit(main())
