#!/usr/bin/env python3

"""
Configurable Mini Browser 3.0 / 4.1 / WHY2025 BadgeVMS full regression tester.

Edit only the CONFIG section for normal use.

Tests supported:
- Load arbitrary web sites through WHY+E
- Activate numbered links from the Mini Browser home page
- Run GET-form searches
- Exercise WHY-key badge/browser commands
- Verify action-number correction with Backspace
- Verify Mini Browser 2.5 Phase 1 HTML/entity handling
- Verify Mini Browser 2.5 Phase 2 parser torture/limits
- Verify Mini Browser 2.6 Phase 1B bounded foreground colors
- Verify Mini Browser 4.1 omnibox (search / URL / suggestions) and history
- Verify Mini Browser 4.1 tabs (new, switch, overview, close, limit)
- Print a PASS / NOT PASSED summary

Viewing speed:
    Every loaded page stays on screen for CONFIG["serial"]["view_delay"]
    seconds so you can watch the badge.  --view N changes it, --fast sets 0.

Serial keyboard protocol:
    E <scancode-hex> <down 0|1> <text-hex>

Example:
    E 28 1 00
    E 28 0 00
"""

import argparse
import re
import sys
import time
import tty
import termios
import threading

import serial
from serial.tools import list_ports


# ---------------------------------------------------------------------------
# CONFIG
# ---------------------------------------------------------------------------

CONFIG = {
    "serial": {
        "device": None,  # auto-detect; set a path here to override
        "baudrate": 115200,
        "startup_delay": 2.0,
        "default_timeout": 1000,
        # Seconds each loaded page stays visible before the next step.
        # 0 = as fast as possible (the old behaviour). --view / --fast override.
        "view_delay": 3.0,
    },

    # Mini Browser 4.1 omnibox + history tests.
    "v43": {
        # Slow URL for the stop test: the server waits this long before it
        # answers, so Esc arrives while the browser is still waiting.
        "slow_url": "httpbin.org/delay/10",
        # Compressed response for the gzip test (Content-Encoding: gzip).
        "gzip_url": "httpbin.org/gzip",
        # A host name that never resolves (.invalid is reserved, RFC 2606).
        "bad_host": "nonexistent.invalid",
        # A page that does not exist on minibrowser.macip.net.
        "missing_page": "this-page-does-not-exist-43.html",
        # 4.3-dev2: a binary file (application/octet-stream) to download.
        "download_url": "httpbin.org/bytes/20000",
        "download_name": "20000",
        "download_bytes": 20000,
        # A JSON page sent with "Content-Disposition: attachment".
        "attachment_url": "httpbin.org/response-headers?content-disposition=attachment",
        # A cookie with Max-Age (kept after a restart) for the restart test.
        "persistent_cookie_url": "httpbin.org/response-headers?set-cookie=mbkeep=43;max-age=3600",
        "cookie_check_url": "httpbin.org/cookies",
        # Quits and restarts Mini Browser, like CONFIG["v41"]["test_restart"].
        "test_cookie_restart": False,
    },

    "v41": {
        # Pages on minibrowser.macip.net used to fill the history.
        "history_pages": [
            "phase1b-colors.html",
            "phase5-info.php",
        ],
        # Page used for the suggestion test, and the text typed to find it.
        "suggest_page": "phase2a-styles.html",
        "suggest_typed": "phase2a",
        "search_query": "esp32 badge",
        # Destructive / app-restarting tests are opt-in.
        "test_clear_history": False,   # WHY+X wipes your browsing history
        "test_restart": False,         # quits Mini Browser and starts it again
    },

    # Mini Browser home page.
    "home": {
        "url_pattern": r"HTTP 200.*https://minibrowser\.macip\.net",
    },

    # Mini Browser 2.5 Phase 1 controlled entity test page.
    # Upload phase1-entities.html and phase1-entity-target.html to the
    # minibrowser.macip.net document root before running these tests.
    "phase1": {
        "url": "minibrowser.macip.net/phase1-entities.html",
        "url_pattern": r"HTTP 200.*https://minibrowser\.macip\.net/phase1-entities\.html",
        "target_pattern": r"HTTP 200.*https://minibrowser\.macip\.net/phase1-entity-target\.html\?a=one&b=two",
    },

    # Mini Browser 2.6 Phase 1B controlled foreground-color page.
    # Upload phase1b-colors.html to the minibrowser.macip.net document root.
    "phase1b": {
        "url": "minibrowser.macip.net/phase1b-colors.html",
        "url_pattern": r"HTTP 200.*https://minibrowser\.macip\.net/phase1b-colors\.html",
    },

    # Mini Browser 2.6 Phase 2A controlled inline text-style page.
    # Upload phase2a-styles.html to the minibrowser.macip.net document root.
    "phase2a": {
        "url": "minibrowser.macip.net/phase2a-styles.html",
        "url_pattern": r"HTTP 200.*https://minibrowser\.macip\.net/phase2a-styles\.html",
    },

    # Mini Browser 2.6 Phase 2B controlled background-color page.
    # Upload phase2b-backgrounds.html to the minibrowser.macip.net document root.
    "phase2b": {
        "url": "minibrowser.macip.net/phase2b-backgrounds.html",
        "url_pattern": r"HTTP 200.*https://minibrowser\.macip\.net/phase2b-backgrounds\.html",
    },

    # Mini Browser 3.0 Phase 3 final image/display-mode regression page.
    # Upload phase3-final-images.html to the minibrowser.macip.net document root.
    # It intentionally references the existing /img/mb.png using absolute,
    # root-relative and document-relative URLs.
    "phase3final": {
        "url": "minibrowser.macip.net/phase3-final-images.html",
        "url_pattern": r"HTTP 200.*https://minibrowser\.macip\.net/phase3-final-images\.html",
        "image_url_pattern": r"https://minibrowser\.macip\.net/img/mb\.png",
    },

    # Mini Browser 2.5 Phase 2 controlled parser-torture pages.
    # Upload all phase2-*.html files to the minibrowser.macip.net document root.
    "phase2": {
        "base": "minibrowser.macip.net/",
    },

    # Generic web-site tests.
    #
    # mode:
    #   "url"       -> WHY+E, then type URL
    #   "home_link" -> return home and activate numbered link
    #
    "sites": [
        {
            "name": "Example.com",
            "mode": "url",
            "url": "example.com",
            "expect": r"HTTP 200.*https://example\.com",
        },
       {
            "name": "UTF-8",
            "mode": "url",
            "url": "minibrowser.macip.net/u.html",
            "expect": r"HTTP 200.*https://minibrowser.macip\.net",
        },
       {
            "name": "UTF-8",
            "mode": "url",
            "url": "minibrowser.macip.net/e.html",
            "expect": r"HTTP 200.*https://minibrowser.macip\.net",
        },
       {
            "name": "UTF-8",
            "mode": "url",
            "url": "minibrowser.macip.net/em.html",
            "expect": r"HTTP 200.*https://minibrowser.macip\.net",
        },
       {
            "name": "404",
            "mode": "url",
            "url": "minibrowser.macip.net/nopage.html",
            "expect": r"HTTP 404.*https://minibrowser.macip\.net",
            "wait_for_content": False,
        },
        {
            "name": "MacIP.net",
            "mode": "url",
            "url": "macip.net",
            "expect": r"HTTP 200.*macip\.net",
        },
        {
            "name": "Wiby",
            "mode": "home_link",
            "action": 5,
            "expect": r"HTTP 200.*https://wiby\.me",
        },
        {
            "name": "Hacker News",
            "mode": "home_link",
            "action": 8,
            "expect": r"HTTP 200.*news\.ycombinator\.com",
        },
        {
            "name": "curl",
            "mode": "home_link",
            "action": 11,
            "expect": r"HTTP 200.*curl",
        },
        {
            "name": "ifconfig.co",
            "mode": "home_link",
            "action": 12,
            "expect": r"HTTP 200.*ifconfig.co",
        },
    ],

    # Search tests.
    #
    # source:
    #   "home"      -> search form is on Mini Browser home page
    #   "home_link" -> first open a numbered link from home, then use form
    #
    # submit_label:
    #   unique text contained in the submit control.
    #
    # numbered_fallback:
    #   useful for Google when result titles/URLs are stripped but numbered
    #   result actions are still clearly present.
    #

    "searches": [
            {
                "name": "Wiby search",
                "source": "home_link",
                "home_action": 5,
                "open_expect": r"HTTP 200.*https://wiby\.me",
                "query": "esp32",
                "submit_label": "[search]",
                "result_expect": r"HTTP 200.*wiby\.me",
                "numbered_fallback": None,
            },
            {
                "name": "Google search",
                "source": "home",
                "query": "esp32",
                "submit_label": "[search google]",
                "result_expect": r"HTTP 200.*google",
                "numbered_fallback": {
                    "start": 18,
                    "stop": 46,
                    "minimum": 3,
                },
            },
            {
                "name": "Google search Mini Browser",
                "source": "home",
                "query": "why2025 mini_browser",
                "submit_label": "[search google]",
                "result_expect": r"HTTP 200.*google",
                "numbered_fallback": {
                    "start": 18,
                    "stop": 46,
                    "minimum": 3,
                }
            },
	    {	
                "name": "Google search MacIPRpi",
                "source": "home",
                "query": "MacIPRpi",
                "submit_label": "[search google]",
                "result_expect": r"HTTP 200.*google",
                "numbered_fallback": {
                    "start": 18,
                    "stop": 46,
                    "minimum": 3,
                }
            },
        ],


    # Badge/browser command tests.
    #
    # command:
    #   One of the WHY shortcuts, e.g. H R B G F M Q C E
    #
    # expect:
    #   Regex that must appear after the command.
    #
    # setup:
    #   Optional built-in setup before sending the command.
    #
    # Available setup values:
    #   "home"
    #   "home_then_wiby"
    #   "home_wiby_back"
    #
    # Keep WHY+Q last if you enable it, because it exits Mini Browser.
    #
    "badge_commands": [
        {
            "name": "Home",
            "command": "H",
            "setup": None,
            "expect": r"HTTP 200.*https://minibrowser\.macip\.net",
        },
        {
            "name": "Reload",
            "command": "R",
            "setup": "home",
            "expect": r"HTTP 200.*https://minibrowser\.macip\.net",
        },
        {
            "name": "Back",
            "command": "B",
            "setup": "home_then_wiby",
            "expect": r"HTTP 200.*https://minibrowser\.macip\.net",
        },
        {
            "name": "Forward",
            "command": "G",
            "setup": "home_wiby_back",
            "expect": r"HTTP 200.*https://wiby\.me",
        },

        # Examples that are disabled by default because they need a more
        # specific assertion or alter application state:
        #
        # {
        #     "name": "Bookmarks",
        #     "command": "M",
        #     "setup": "home",
        #     "expect": r"...",
        # },
        #
        # {
        #     "name": "Quit",
        #     "command": "Q",
        #     "setup": "home",
        #     "expect": r"...",
        # },
    ],
}



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

# ---------------------------------------------------------------------------
# CONSTANTS
# ---------------------------------------------------------------------------

DEVICE = detect_badge_device(CONFIG["serial"]["device"])
BAUDRATE = CONFIG["serial"]["baudrate"]
DEFAULT_TIMEOUT = CONFIG["serial"]["default_timeout"]
VIEW_DELAY = CONFIG["serial"]["view_delay"]   # may be changed by --view/--fast


# ---------------------------------------------------------------------------
# SERIAL / KEYBOARD
# ---------------------------------------------------------------------------

class Badge:
    def __init__(self):
        self.serial = serial.Serial(
            DEVICE,
            BAUDRATE,
            timeout=0.1,
        )

        self.running = True
        self.lock = threading.Lock()
        self.lines = []

        self.reader_thread = threading.Thread(
            target=self._reader,
            daemon=True,
        )
        self.reader_thread.start()

    def _reader(self):
        buffer = bytearray()

        while self.running:
            try:
                data = self.serial.read(1024)

                if not data:
                    continue

                sys.stdout.buffer.write(data)
                sys.stdout.buffer.flush()

                buffer.extend(data)

                while b"\n" in buffer:
                    line, _, buffer = buffer.partition(b"\n")
                    text = line.decode(
                        "utf-8",
                        errors="replace",
                    ).rstrip("\r")

                    with self.lock:
                        self.lines.append(text)

                        if len(self.lines) > 5000:
                            self.lines = self.lines[-3500:]

            except Exception as exc:
                if self.running:
                    print(
                        f"\nSerial reader error: {exc}",
                        file=sys.stderr,
                    )
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
        command = (
            f"E {scancode:02X} "
            f"{1 if down else 0} "
            f"{text:02X}\n"
        )

        char_debug = ""
        if down and text:
            try:
                character = chr(text)
                if character.isprintable():
                    char_debug = f" [[{character}]]"
            except (ValueError, OverflowError):
                pass

        print(
            f"\n[TEST TX] {command.strip()}{char_debug}",
            file=sys.stderr,
        )

        self.serial.write(command.encode("ascii"))
        self.serial.flush()

    def press(self, scancode, text=0, delay=0.08):
        self.send_event(scancode, True, text)
        time.sleep(delay)
        self.send_event(scancode, False, 0)
        time.sleep(delay)

    def why(self, letter):
        letter = letter.upper()
        why_scancode = 0xE3
        key_scancode = 0x04 + ord(letter) - ord("A")

        self.send_event(why_scancode, True, 0)
        time.sleep(0.15)
        self.send_event(
            key_scancode,
            True,
            ord(letter.lower()),
        )
        time.sleep(0.15)
        self.send_event(key_scancode, False, 0)
        time.sleep(0.15)
        self.send_event(why_scancode, False, 0)
        time.sleep(0.50)

    def settle(self, seconds=0.5):
        self.serial.flush()
        time.sleep(seconds)

    def view(self, what="page"):
        """Leave the current page on screen so a human can watch the run."""
        if VIEW_DELAY > 0:
            print(f"\n[VIEW] {what} on screen for {VIEW_DELAY:g}s", file=sys.stderr)
            self.serial.flush()
            time.sleep(VIEW_DELAY)

    def why_key(self, scancode, text=0):
        """WHY + any key by scancode (Tab = 0x2B, digit 1 = 0x1E, ...)."""
        self.send_event(0xE3, True, 0)
        time.sleep(0.15)
        self.send_event(scancode, True, text)
        time.sleep(0.15)
        self.send_event(scancode, False, 0)
        time.sleep(0.15)
        self.send_event(0xE3, False, 0)
        time.sleep(0.50)

    def enter(self):
        self.press(0x28)

    def backspace(self):
        self.press(0x2A)

    def wait_for(self, pattern, timeout=None):
        if timeout is None:
            timeout = DEFAULT_TIMEOUT

        print(
            f"\n[WAIT] {pattern}",
            file=sys.stderr,
        )

        regex = re.compile(pattern)
        deadline = time.monotonic() + timeout
        checked = 0

        while time.monotonic() < deadline:
            with self.lock:
                current = list(self.lines)

            for line in current[checked:]:
                if regex.search(line):
                    print(
                        f"\n[MATCH] {line}",
                        file=sys.stderr,
                    )
                    return line

            checked = len(current)
            time.sleep(0.05)

        raise TimeoutError(
            f"Timeout waiting for: {pattern}"
        )

    def type_text(self, text, key_delay=0.15):
        """
        Type conservatively through the host-to-badge keyboard bridge.

        BadgeVMS/compositor can drop injected events while Mini Browser is
        busy rendering/decoding. Reliability is more important than speed.
        """
        for character in text:
            scancode = ascii_scancode(character)

            if scancode is None:
                raise ValueError(
                    f"Unsupported character: {character!r}"
                )

            self.press(
                scancode,
                ord(character),
                delay=0.10,
            )
            time.sleep(key_delay)


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


# ---------------------------------------------------------------------------
# CONTENT PARSING
# ---------------------------------------------------------------------------

def latest_content_block(badge):
    lines = badge.get_lines()
    blocks = []
    current = None

    for line in lines:
        if line.strip() == "--- CONTENT START ---":
            current = []
            continue

        if line.strip() == "--- CONTENT END ---":
            if current is not None:
                blocks.append(current)
                current = None
            continue

        if current is not None:
            current.append(line)

    if not blocks:
        return []

    return blocks[-1]


def numbered_actions(content):
    actions = []

    for line in content:
        cleaned = line.strip()

        if not cleaned:
            continue

        matches = list(
            re.finditer(
                r"\[(\d+)\]",
                cleaned,
            )
        )

        for index, match in enumerate(matches):
            number = int(match.group(1))
            label_start = match.end()

            if index + 1 < len(matches):
                label_end = matches[index + 1].start()
            else:
                label_end = len(cleaned)

            label = cleaned[
                label_start:label_end
            ].strip()

            actions.append(
                {
                    "number": number,
                    "label": label,
                    "raw": cleaned,
                }
            )

    return actions


def normalize_control_text(text):
    text = text.strip().lower()
    text = text.strip("[]").strip()

    return re.sub(
        r"\s+",
        " ",
        text,
    )


def find_search_controls(content, submit_label):
    actions = numbered_actions(content)

    wanted = normalize_control_text(
        submit_label
    )

    submit_index = None

    for index, action in enumerate(actions):
        label = normalize_control_text(
            action["label"]
        )

        if label == wanted or wanted in label:
            submit_index = index
            break

    if submit_index is None:
        rendered = " | ".join(
            (
                f"[{action['number']}]"
                f"{action['label']}"
            )
            for action in actions[:30]
        )

        raise RuntimeError(
            (
                "Could not find submit control "
                f"containing {submit_label!r}. "
                f"Visible actions: {rendered}"
            )
        )

    submit_number = actions[
        submit_index
    ]["number"]

    field_number = None

    for index in range(
        submit_index - 1,
        -1,
        -1,
    ):
        action = actions[index]
        low = normalize_control_text(
            action["label"]
        )

        if (
            low.startswith("q:")
            or low.startswith("query:")
            or low.startswith("search:")
            or low == "q"
            or low == "query"
        ):
            field_number = action["number"]
            break

    if field_number is None:
        rendered = " | ".join(
            (
                f"[{action['number']}]"
                f"{action['label']}"
            )
            for action in actions[:30]
        )

        raise RuntimeError(
            (
                f"Found submit [{submit_number}] "
                f"{actions[submit_index]['label']!r}, "
                "but no preceding search field. "
                f"Visible actions: {rendered}"
            )
        )

    return field_number, submit_number



def looks_like_result_url(line):
    cleaned = line.strip()

    if not cleaned:
        return False

    low = cleaned.lower()

    if low.startswith("http://") or low.startswith("https://"):
        return True

    if low.startswith("www."):
        return True

    if " › " in cleaned:
        return True

    if re.match(
        r"^[a-z0-9][a-z0-9.-]*\.[a-z]{2,}(?:\s|$)",
        low,
    ):
        return True

    return False


def extract_search_results(badge):
    content = latest_content_block(badge)
    results = []
    index = 0

    ignored_labels = {
        "google",
        "wiby",
        "settings",
        "search",
        "submit",
        "afbeeldingen",
        "video's",
        "maps",
        "nieuws",
        "boeken",
        "zoektools",
        "alles bekijken",
        "volgende >",
        "meer informatie",
        "inloggen",
        "privacy",
        "voorwaarden",
        "donker thema: uit",
        "find more...",
    }

    while index < len(content):
        cleaned = content[index].strip()

        match = re.match(
            r"^\[(\d+)\]\s*(.*)$",
            cleaned,
        )

        if not match:
            index += 1
            continue

        number = int(match.group(1))
        title = match.group(2).strip()
        title_index = index

        if not title:
            probe = index + 1

            while probe < len(content):
                candidate = content[probe].strip()

                if not candidate:
                    probe += 1
                    continue

                if re.match(r"^\[\d+\]", candidate):
                    break

                title = candidate
                title_index = probe
                break

            if not title:
                index += 1
                continue

        low = title.lower()

        if (
            low in ignored_labels
            or "[submit]" in low
            or low.startswith("q:")
            or low.startswith("query:")
            or low.startswith("search:")
        ):
            index += 1
            continue

        result_url = None

        for probe in range(
            title_index + 1,
            min(len(content), title_index + 7),
        ):
            candidate = content[probe].strip()

            if not candidate:
                continue

            if re.match(r"^\[\d+\]", candidate):
                break

            if looks_like_result_url(candidate):
                result_url = candidate
                break

        if result_url is not None:
            results.append(
                {
                    "number": number,
                    "title": title,
                    "url": result_url,
                }
            )

        index += 1

    return results


def extract_numbered_result_actions(
    badge,
    start_number,
    stop_number,
):
    content = latest_content_block(badge)
    numbers = []

    for line in content:
        for match in re.finditer(r"\[(\d+)\]", line):
            number = int(match.group(1))

            if start_number <= number <= stop_number:
                if number not in numbers:
                    numbers.append(number)

    return numbers


# ---------------------------------------------------------------------------
# BROWSER OPERATIONS
# ---------------------------------------------------------------------------

def go_home(badge):
    badge.clear_log()
    badge.why("H")

    # HTTP 200 means the document arrived, but Phase 3 may still be
    # processing images. Synchronize on the completed display summary
    # without assuming which WHY+O display mode is currently active.
    badge.wait_for(
        CONFIG["home"]["url_pattern"],
        20,
    )
    badge.wait_for(
        r"\[mini_browser\] display: mode=.* "
        r"images_seen=\d+ images_retained=\d+ images_loaded=\d+",
        20,
    )
    badge.settle(1.0)


def activate_home_link(
    badge,
    action,
    expected_pattern,
):
    go_home(badge)
    badge.clear_log()

    badge.type_text(str(action))
    badge.settle(0.2)
    badge.enter()
    badge.settle(0.4)

    badge.wait_for(
        rf"activating link {action}",
        10,
    )

    badge.wait_for(
        expected_pattern,
        DEFAULT_TIMEOUT,
    )

    # HTTP 200 alone is too early for parser assertions. Wait until the
    # complete diagnostic CONTENT block has arrived over serial.
    badge.wait_for(
        r"^--- CONTENT END ---$",
        DEFAULT_TIMEOUT,
    )

    badge.settle(1.0)
    badge.view(f"home link [{action}]")


def test_action_number_backspace(badge):
    """
    Phase 0.5 regression:
    enter the wrong action number 55, remove the final 5 with
    Backspace, then press Enter. The remaining action 5 must open Wiby.
    """
    go_home(badge)
    badge.clear_log()

    badge.type_text("55")
    badge.settle(0.2)

    badge.backspace()
    badge.settle(0.2)

    badge.enter()
    badge.settle(0.4)

    badge.wait_for(
        r"activating link 5",
        10,
    )

    badge.wait_for(
        r"HTTP 200.*https://wiby\.me",
        DEFAULT_TIMEOUT,
    )

    badge.settle(1.0)

    return [
        "Typed action 55",
        "Backspace removed the last digit",
        "Remaining action 5 opened Wiby",
    ]


def open_direct_url(
    badge,
    url,
    expected_pattern,
    wait_for_content=True,
):
    """
    Open a URL through the host-to-badge keyboard bridge.

    BadgeVMS can occasionally reject injected key events while Mini Browser is
    busy. A rejected event can silently remove a hostname character. Treat
    that as a transport problem and retry the complete URL entry from Home.
    """
    typed_url = url[len("https://"):] if url.startswith("https://") else url
    max_attempts = 3

    for attempt in range(1, max_attempts + 1):
        if attempt > 1:
            print(
                f"\n[KEYBOARD RETRY] URL attempt {attempt}/{max_attempts}: {url}",
                file=sys.stderr,
            )

        go_home(badge)
        badge.clear_log()
        badge.why("E")
        badge.settle(0.8)

        # Observe only events belonging to the actual URL typing.
        badge.clear_log()
        badge.type_text(typed_url)
        badge.settle(0.5)

        if any(
            "Unable to send event to task" in line
            for line in badge.get_lines()
        ):
            print(
                "\n[KEYBOARD RETRY] compositor rejected an event while typing URL",
                file=sys.stderr,
            )
            badge.settle(1.0)
            continue

        badge.enter()
        badge.settle(0.6)

        regex = re.compile(expected_pattern)
        deadline = time.monotonic() + DEFAULT_TIMEOUT
        checked = 0
        success = False
        transport_failure = None

        while time.monotonic() < deadline:
            current = badge.get_lines()

            for line in current[checked:]:
                if regex.search(line):
                    print(f"\n[MATCH] {line}", file=sys.stderr)
                    success = True
                    break

                if "Unable to send event to task" in line:
                    transport_failure = "compositor rejected an injected event"
                    break

                # 4.3 firmware reports DNS failures as curl 5 (resolve
                # host); older firmware as 6 (connect).
                if re.search(r"\[mini_browser\] fetch error [56] URL=", line):
                    transport_failure = f"URL/DNS failure after keyboard entry: {line}"
                    break

            if success or transport_failure:
                break

            checked = len(current)
            time.sleep(0.05)

        if success:
            # Normal parser/page tests need the complete serial CONTENT block.
            # Status/error tests such as the intentional 404 may not emit one.
            if wait_for_content:
                badge.wait_for(r"^--- CONTENT END ---$", DEFAULT_TIMEOUT)
            badge.settle(1.0)
            badge.view(url)
            return

        if transport_failure:
            print(f"\n[KEYBOARD RETRY] {transport_failure}", file=sys.stderr)
        else:
            print(
                f"\n[KEYBOARD RETRY] expected URL result not seen on attempt {attempt}",
                file=sys.stderr,
            )

        badge.settle(1.0)

    raise RuntimeError(
        "Host-to-badge keyboard bridge failed to enter the requested URL "
        f"reliably after {max_attempts} attempts: {url}"
    )

def perform_search(badge, config):
    if config["source"] == "home":
        go_home(badge)

    elif config["source"] == "home_link":
        activate_home_link(
            badge,
            config["home_action"],
            config["open_expect"],
        )

    else:
        raise ValueError(
            f"Unknown search source: {config['source']}"
        )

    content = latest_content_block(badge)

    field_number, submit_number = find_search_controls(
        content,
        config["submit_label"],
    )

    print(
        (
            f"\n[SEARCH FORM] field=[{field_number}] "
            f"submit=[{submit_number}]"
        ),
        file=sys.stderr,
    )

    badge.clear_log()

    badge.type_text(str(field_number))
    badge.settle(0.2)
    badge.enter()
    badge.settle(0.4)

    badge.type_text(config["query"])
    badge.settle(0.3)
    badge.enter()
    badge.settle(0.4)

    badge.type_text(str(submit_number))
    badge.settle(0.2)
    badge.enter()
    badge.settle(0.5)

    badge.wait_for(
        config["result_expect"],
        30,
    )

    badge.settle(2.0)
    badge.view(f"search results for {config['query']!r}")

    query = config["query"]

    if not any(
        query.lower() in line.lower()
        for line in badge.get_lines()
    ):
        raise RuntimeError(
            f"HTTP succeeded, but query {query!r} "
            "was not visible in the result page"
        )

    results = extract_search_results(badge)

    details = [
        (
            f"Query: {query} "
            f"(field [{field_number}], submit [{submit_number}])"
        )
    ]

    if results:
        for result in results:
            details.append(
                (
                    f"[{result['number']}] "
                    f"{result['title']} -> "
                    f"{result['url']}"
                )
            )
        return details

    fallback = config.get("numbered_fallback")

    if fallback:
        numbers = extract_numbered_result_actions(
            badge,
            fallback["start"],
            fallback["stop"],
        )

        if len(numbers) >= fallback.get("minimum", 3):
            details.append(
                "Result actions: "
                + ", ".join(
                    f"[{number}]"
                    for number in numbers
                )
            )
            details.append(
                f"Detected {len(numbers)} numbered result actions"
            )
            return details

    raise RuntimeError(
        "Search HTTP request succeeded, but no usable "
        "search-result links/actions were detected"
    )


def phase1_open(badge, url=None, expected=None):
    if url is None:
        url = CONFIG["phase1"]["url"]
    if expected is None:
        expected = CONFIG["phase1"]["url_pattern"]

    open_direct_url(badge, url, expected)
    content = latest_content_block(badge)

    if not content:
        raise RuntimeError("Phase 1 page loaded but no CONTENT block was captured")

    return content


def require_content(content, *needles):
    joined = "\n".join(content)

    missing = [
        needle
        for needle in needles
        if needle not in joined
    ]

    if missing:
        raise RuntimeError(
            "Decoded page content is missing: "
            + " | ".join(repr(item) for item in missing)
        )

    return joined


def phase1_named_entities(badge):
    content = phase1_open(badge)
    require_content(
        content,
        "NAMED:",
        "©", "®", "™", "€", "£", "¥", "¢",
        "°", "±", "×", "÷", "•", "·", "…",
        "–", "—", "‘", "’", "“", "”", "«", "»",
        "END PHASE1 ENTITY TEST",
    )
    # These five ASCII entities are checked together so an undecoded
    # &amp;/&lt;/etc. cannot accidentally satisfy the Unicode checks above.
    joined = "\n".join(content)
    if "NAMED: & < > \" '" not in joined:
        raise RuntimeError("Basic named entities did not decode as expected")
    return ["Basic and extended named entities decoded"]


def phase1_decimal_entities(badge):
    content = phase1_open(badge)
    require_content(content, "DECIMAL: © € Ω 中")
    return ["Decimal numeric entities decoded to Unicode"]


def phase1_hex_entities(badge):
    content = phase1_open(badge)
    require_content(content, "HEX: © € Ω 中")
    return ["Hexadecimal numeric entities decoded to Unicode"]


def phase1_unicode_entities(badge):
    content = phase1_open(badge)
    require_content(
        content,
        "UNICODE:",
        "Greek=Ω",
        "Chinese=中文",
        "Emoji=😀",
        "ARABIC:",
    )
    # The Arabic text may be visually reordered/shaped by the renderer, but
    # the serial CONTENT block is the logical decoded UTF-8 text.
    require_content(content, "سلام")
    return [
        "Greek, CJK and supplementary-plane emoji decoded",
        "Arabic numeric entities decoded into logical UTF-8",
    ]


def phase1_invalid_entities(badge):
    content = phase1_open(badge)
    require_content(
        content,
        "INVALID:",
        "zero=�",
        "surrogate=�",
        "too-high=�",
        "END PHASE1 ENTITY TEST",
    )
    return ["Invalid Unicode scalar values safely became U+FFFD"]


def phase1_malformed_entities(badge):
    content = phase1_open(badge)
    require_content(
        content,
        "MALFORMED:",
        "&doesnotexist;",
        "&#xyz;",
        "&#xZZZZ;",
        "END PHASE1 ENTITY TEST",
    )
    return ["Unknown/malformed entities remained literal and parser continued"]


def phase1_entity_url(badge):
    content = phase1_open(badge)
    actions = numbered_actions(content)

    action = next(
        (
            item["number"]
            for item in actions
            if "Entity URL test" in item["label"]
        ),
        None,
    )

    if action is None:
        raise RuntimeError("Could not find Entity URL test action")

    badge.clear_log()
    badge.type_text(str(action))
    badge.settle(0.2)
    badge.enter()
    badge.settle(0.4)

    badge.wait_for(rf"activating link {action}", 10)
    badge.wait_for(CONFIG["phase1"]["target_pattern"], DEFAULT_TIMEOUT)
    badge.settle(0.8)

    return [
        f"Activated Entity URL test as action [{action}]",
        "href &amp; decoded to literal & in requested URL",
    ]


def phase1_title_entity(badge):
    # Use a unique query string so this fetch has an unambiguous log sequence.
    url = CONFIG["phase1"]["url"] + "?titlecheck=1"
    expected = (
        r"HTTP 200.*https://minibrowser\.macip\.net/"
        r"phase1-entities\.html\?titlecheck=1"
    )

    badge.clear_log()
    open_direct_url(badge, url, expected)

    # This line is emitted directly from page->title immediately after
    # html_to_page(), so it tests the actual decoded title rather than
    # inferring it through the synthetic Bookmarks page.
    badge.wait_for(
        r"\[mini_browser\] page title: Phase 1 & Entities 😀",
        10,
    )

    return [
        "Decoded <title> verified directly from page->title diagnostic",
        "Expected title: Phase 1 & Entities 😀",
    ]


def phase1_button_entity(badge):
    content = phase1_open(badge)
    actions = numbered_actions(content)

    matching = [
        item
        for item in actions
        if normalize_control_text(item["label"]) == "search & go"
    ]

    if not matching:
        rendered = " | ".join(
            f"[{item['number']}] {item['label']}"
            for item in actions
        )
        raise RuntimeError(
            "Decoded button label 'Search & Go' not found. "
            f"Visible actions: {rendered}"
        )

    return [
        f"Button label decoded: [{matching[0]['number']}] Search & Go"
    ]


def phase2_url(filename):
    return CONFIG["phase2"]["base"] + filename


def phase1b_foreground_colors(badge):
    open_direct_url(badge, CONFIG["phase1b"]["url"], CONFIG["phase1b"]["url_pattern"])
    badge.wait_for(r"\[mini_browser\] visual: explicit_colors=13", 13)
    content = latest_content_block(badge)
    if not content:
        raise RuntimeError("Phase 1B color page loaded but no CONTENT block was captured")
    require_content(content, "PHASE1B COLORS", "named red", "hex green", "short blue",
                    "inline orange", "nested purple", "nested cyan", "restored purple",
                    "link magenta", "heading yellow", "invalid ignored", "END PHASE1B COLORS")
    return ["13 valid foreground colors parsed", "Nested color content preserved", "Invalid color ignored"]


def phase2a_inline_text_styles(badge):
    open_direct_url(badge, CONFIG["phase2a"]["url"], CONFIG["phase2a"]["url_pattern"])
    badge.wait_for(r"\[mini_browser\] visual: explicit_styles=15", 13)
    content = latest_content_block(badge)
    if not content:
        raise RuntimeError("Phase 2A style page loaded but no CONTENT block was captured")
    require_content(content, "PHASE2A STYLES", "bold", "weight 700", "italic",
                    "underline", "left aligned", "center aligned", "right aligned",
                    "yellow bold", "parent bold", "bold italic", "restored bold",
                    "normal", "bold again", "not underlined", "underlined again",
                    "invalid ignored", "END PHASE2A STYLES")
    return ["15 valid inline style declarations parsed",
            "Nested style content preserved",
            "Invalid style values ignored"]


def phase2b_background_colors(badge):
    open_direct_url(badge, CONFIG["phase2b"]["url"], CONFIG["phase2b"]["url_pattern"])
    badge.wait_for(r"\[mini_browser\] visual: explicit_backgrounds=10", 13)
    content = latest_content_block(badge)
    if not content:
        raise RuntimeError("Phase 2B background page loaded but no CONTENT block was captured")
    require_content(content, "PHASE2B BACKGROUNDS", "yellow background", "short hex background",
                    "dark blue paragraph text run", "purple parent", "orange child", "restored purple",
                    "gray parent", "transparent child", "restored gray", "white bold on blue",
                    "styled on red", "short abc background", "invalid background ignored",
                    "END PHASE2B BACKGROUNDS")
    return ["10 valid background-color declarations parsed",
            "Nested and transparent background content preserved",
            "Invalid background color ignored"]



def phase3_set_display_mode(badge, number, name):
    """Select a WHY+O display mode and wait until Mini Browser records it."""
    badge.clear_log()
    badge.why("O")
    badge.settle(0.3)
    badge.type_text(str(number))
    badge.wait_for(
        r"\[mini_browser\] display mode: " + re.escape(name),
        10,
    )
    badge.settle(0.8)


def phase3_open_final_page(badge):
    open_direct_url(
        badge,
        CONFIG["phase3final"]["url"],
        CONFIG["phase3final"]["url_pattern"],
    )


def phase3_require_final_content(badge):
    content = latest_content_block(badge)
    if not content:
        raise RuntimeError(
            "Phase 3 final image page loaded but no CONTENT block was captured"
        )
    require_content(
        content,
        "PHASE3 FINAL IMAGE TEST",
        "ABSOLUTE IMAGE",
        "ROOT RELATIVE IMAGE",
        "DOCUMENT RELATIVE IMAGE",
        "INLINE IMAGE FOUR",
        "INLINE IMAGE FIVE",
        "ACTION IMAGE SIX",
        "END PHASE3 FINAL IMAGE TEST",
    )
    return content


def phase3_colors_images(badge):
    # Fix 15 mode 4: the original five-inline-image path is now explicit
    # experimental mode rather than the default.
    phase3_set_display_mode(badge, 4, "Colors + 5 Images (Experimental)")
    phase3_open_final_page(badge)

    badge.wait_for(
        r"\[mini_browser\] display: mode=Colors \+ 5 Images \(Experimental\) "
        r"images_seen=6 images_retained=6 images_loaded=5",
        30,
    )
    badge.wait_for(
        r"\[mini_browser\] image: fetching "
        + CONFIG["phase3final"]["image_url_pattern"],
        10,
    )
    badge.wait_for(
        r"\[mini_browser\] image: decoded source=\d+x\d+ "
        r"retained=\d+x\d+ RGB565=\d+ bytes",
        10,
    )

    # Mode 4: images 1..5 are inline markers; image 6 is an action.
    content = latest_content_block(badge)
    if not content:
        raise RuntimeError("Phase 3 final image page loaded but no CONTENT block was captured")
    joined = "\n".join(content)
    require_content(
        content,
        "PHASE3 FINAL IMAGE TEST",
        "[1] Image: ACTION IMAGE SIX",
        "END PHASE3 FINAL IMAGE TEST",
    )
    inline_markers = re.findall(r"\[\[MBIMG\d+\]\]", joined)
    if len(inline_markers) != 5:
        raise RuntimeError(
            f"Expected exactly five inline image markers in experimental mode, found {len(inline_markers)}: {inline_markers}"
        )

    lines = "\n".join(badge.get_lines())
    if "https://img/mb.png" in lines:
        raise RuntimeError(
            "Document-relative image URL regressed to https://img/mb.png"
        )

    return [
        "Experimental Colors + 5 Images mode selected through WHY+O",
        "Six <img> elements seen and retained",
        "Exactly first five images loaded inline; sixth remains an action",
        "Bounded JPEG/PNG RGB565 decode path active",
        "Absolute/root-relative/document-relative image URLs resolve to the origin",
    ]


def phase3_black_white(badge):
    phase3_set_display_mode(badge, 1, "Black & White")
    phase3_open_final_page(badge)

    badge.wait_for(
        r"\[mini_browser\] display: mode=Black & White "
        r"images_seen=6 images_retained=0 images_loaded=0",
        20,
    )
    content = phase3_require_final_content(badge)
    joined = "\n".join(content)

    for alt in (
        "ABSOLUTE IMAGE",
        "ROOT RELATIVE IMAGE",
        "DOCUMENT RELATIVE IMAGE",
        "INLINE IMAGE FOUR",
        "INLINE IMAGE FIVE",
        "ACTION IMAGE SIX",
    ):
        if f"[Image: {alt}]" not in joined:
            raise RuntimeError(
                f"Black & White mode missing image placeholder for {alt}"
            )

    return [
        "Black & White mode selected through WHY+O",
        "All six images represented as alt-text placeholders",
        "No image retained or loaded",
    ]


def phase3_colors_no_images(badge):
    phase3_set_display_mode(badge, 2, "Colors")
    phase3_open_final_page(badge)

    badge.wait_for(
        r"\[mini_browser\] display: mode=Colors "
        r"images_seen=6 images_retained=0 images_loaded=0",
        20,
    )
    content = phase3_require_final_content(badge)
    joined = "\n".join(content)

    for alt in (
        "ABSOLUTE IMAGE",
        "ROOT RELATIVE IMAGE",
        "DOCUMENT RELATIVE IMAGE",
        "INLINE IMAGE FOUR",
        "INLINE IMAGE FIVE",
        "ACTION IMAGE SIX",
    ):
        if f"[Image: {alt}]" not in joined:
            raise RuntimeError(
                f"Colors mode missing image placeholder for {alt}"
            )

    return [
        "Colors mode selected through WHY+O",
        "All six images represented as alt-text placeholders",
        "No image retained or loaded",
    ]


def phase3_restore_colors_images(badge):
    # Fix 15 mode 3: this is the normal/default visual mode.
    # Only the first image is inline; images 2..6 are ACTION_IMAGE entries.
    phase3_set_display_mode(badge, 3, "Colors + Image")
    phase3_open_final_page(badge)

    badge.wait_for(
        r"\[mini_browser\] display: mode=Colors \+ Image "
        r"images_seen=6 images_retained=6 images_loaded=1",
        30,
    )

    badge.wait_for(
        r"\[mini_browser\] image: decoded source=\d+x\d+ "
        r"retained=\d+x\d+ RGB565=\d+ bytes",
        10,
    )

    links, actions_count, forms = phase2_parser_counts(badge)
    if links != 0 or actions_count != 5 or forms != 0:
        raise RuntimeError(
            "Expected final image page parser counts 0/5/0 "
            f"(first image inline, images 2..6 are actions), got "
            f"{links}/{actions_count}/{forms}"
        )

    # Mode 3: image 1 is inline; images 2..6 are numbered actions.
    content = latest_content_block(badge)
    if not content:
        raise RuntimeError("Phase 3 final image page loaded but no CONTENT block was captured")
    joined = "\n".join(content)
    require_content(
        content,
        "PHASE3 FINAL IMAGE TEST",
        "[1] Image: ROOT RELATIVE IMAGE",
        "[2] Image: DOCUMENT RELATIVE IMAGE",
        "[3] Image: INLINE IMAGE FOUR",
        "[4] Image: INLINE IMAGE FIVE",
        "[5] Image: ACTION IMAGE SIX",
        "END PHASE3 FINAL IMAGE TEST",
    )
    inline_markers = re.findall(r"\[\[MBIMG\d+\]\]", joined)
    if len(inline_markers) != 1:
        raise RuntimeError(
            f"Expected exactly one inline image marker in default mode, found {len(inline_markers)}: {inline_markers}"
        )

    return [
        "Default Colors + Image mode selected through WHY+O",
        "Exactly one image loaded inline",
        "Images 2..6 remain numbered ACTION_IMAGE entries",
        "Parser counts confirm exactly five image actions",
    ]


def phase2_expect(filename):
    return (
        r"HTTP 200.*https://minibrowser\.macip\.net/"
        + re.escape(filename)
    )


def phase2_open(badge, filename):
    badge.clear_log()
    open_direct_url(
        badge,
        phase2_url(filename),
        phase2_expect(filename),
    )
    badge.wait_for(r"\[mini_browser\] parser: links=\d+ actions=\d+ forms=\d+", 10)
    badge.settle(0.5)

    content = latest_content_block(badge)
    if not content:
        raise RuntimeError(
            f"{filename} loaded but no CONTENT block was captured"
        )

    return content


def phase2_parser_counts(badge):
    lines = badge.get_lines()
    pattern = re.compile(
        r"\[mini_browser\] parser: links=(\d+) actions=(\d+) forms=(\d+)"
    )
    found = None
    for line in lines:
        match = pattern.search(line)
        if match:
            found = tuple(int(x) for x in match.groups())

    if found is None:
        raise RuntimeError("No parser-count diagnostic found")

    return found


def phase2_malformed(badge):
    content = phase2_open(badge, "phase2-malformed.html")
    joined = require_content(
        content,
        "START MALFORMED",
        "quoted greater-than survived",
        "END MALFORMED",
        "RECOVERY LINK",
    )
    if "unknown" in joined.lower() and "</unknown>" in joined.lower():
        raise RuntimeError("Unknown closing tag leaked into rendered text")
    return [
        "Mismatched/unknown tags did not crash or stop parsing",
        "Quoted > inside an attribute did not terminate the tag early",
        "Parser reached END MALFORMED and recovery link",
    ]


def phase2_links(badge):
    content = phase2_open(badge, "phase2-links.html")
    require_content(content, "START LINKS", "END LINKS")
    actions = numbered_actions(content)
    labels = [a["label"] for a in actions]

    for wanted in ("Alpha link", "Root link", "Absolute link"):
        if not any(wanted in label for label in labels):
            raise RuntimeError(f"Supported link missing: {wanted}")

    for forbidden in (
        "Unsupported mail link",
        "Unsupported javascript link",
        "Fragment only",
    ):
        if any(forbidden in label for label in labels):
            raise RuntimeError(f"Unsupported link became an action: {forbidden}")

    links, action_count, forms = phase2_parser_counts(badge)
    if (links, action_count, forms) != (3, 3, 0):
        raise RuntimeError(
            f"Expected parser counts 3/3/0, got {links}/{action_count}/{forms}"
        )

    return [
        "Relative, root-relative and absolute HTTP(S) links became actions",
        "mailto:, javascript: and fragment-only links were not actionable",
        "Parser counts: links=3 actions=3 forms=0",
    ]


def phase2_forms(badge):
    content = phase2_open(badge, "phase2-forms.html")
    require_content(content, "START FORMS", "END FORMS")
    actions = numbered_actions(content)
    labels = [normalize_control_text(a["label"]) for a in actions]

    for wanted in ("q: hello", "s: world", "submit torture form"):
        if not any(wanted in label for label in labels):
            raise RuntimeError(f"Expected form action missing: {wanted}")

    if any("ignored-no-name" in label for label in labels):
        raise RuntimeError("Nameless editable input became an action")
    if any("ignored" in label and "disabled" in label for label in labels):
        raise RuntimeError("Disabled editable input became an action")

    links, action_count, forms = phase2_parser_counts(badge)
    if forms != 1:
        raise RuntimeError(f"Expected one parsed form, got {forms}")
    if action_count != 3:
        raise RuntimeError(f"Expected three actionable controls, got {action_count}")

    return [
        "Hidden/editable/disabled/nameless fields parsed without corruption",
        "Two editable controls plus submit produced three actions",
    ]


def phase2_unicode(badge):
    content = phase2_open(badge, "phase2-unicode.html")
    require_content(
        content,
        "café naïve façade",
        "Ελληνικά Ω",
        "Привет мир",
        "שלום עולם",
        "السلام عليكم",
        "中文 日本語 한국어",
        "😀 🚀 ★",
        "END UNICODE",
    )
    return ["Multiscript UTF-8 survived parser and wrapping pipeline"]


def phase2_rtl(badge):
    content = phase2_open(badge, "phase2-rtl.html")
    require_content(
        content,
        "שלום 123 ABC",
        "السلام 456 ESP32",
        "LEFT שלום 789 RIGHT",
        "END RTL",
    )
    return [
        "Logical RTL/mixed-direction UTF-8 remained intact in CONTENT",
        "Renderer can apply existing Phase 2.4 Bidi/shaping independently",
    ]


def phase2_huge_words(badge):
    # This page intentionally produces a very large CONTENT block. Do not use
    # latest_content_block() here: the regression harness keeps a bounded serial
    # history, so CONTENT START may fall out of history before CONTENT END.
    # Verify completion directly from trailing serial sentinels instead.
    filename = "phase2-huge-words.html"

    badge.clear_log()
    open_direct_url(
        badge,
        phase2_url(filename),
        phase2_expect(filename),
    )

    badge.wait_for(
        r"\[mini_browser\] parser: links=\d+ actions=\d+ forms=\d+",
        10,
    )
    badge.wait_for(r"AFTER HUGE ASCII", 20)
    badge.wait_for(r"AFTER HUGE CJK", 20)
    badge.wait_for(r"END HUGE WORDS", 20)
    badge.wait_for(r"--- CONTENT END ---", 20)

    links, actions, forms = phase2_parser_counts(badge)
    if (links, actions, forms) != (0, 0, 0):
        raise RuntimeError(
            f"Expected parser counts 0/0/0, got {links}/{actions}/{forms}"
        )

    return [
        "12 KiB unbroken ASCII word completed without hang/crash",
        "2048-glyph CJK run completed without hang/crash",
        "Trailing sentinels and CONTENT END were emitted",
        "Parser remained bounded: links=0 actions=0 forms=0",
    ]


def phase2_tables(badge):
    content = phase2_open(badge, "phase2-tables.html")
    joined = require_content(
        content,
        "Name",
        "Value",
        "Unicode",
        "Alpha",
        "123",
        "Ω",
        "Beta",
        "456",
        "中",
        "Gamma",
        "789",
        "😀",
        "END TABLES",
    )
    if "|" not in joined:
        raise RuntimeError("Table cell separators were not emitted")
    return ["Table rows/cells survived with text separators and Unicode"]


def phase2_nested_formatting(badge):
    content = phase2_open(badge, "phase2-nested-formatting.html")
    require_content(
        content,
        "plain",
        "bold",
        "bold italic",
        "code inside strong",
        "quote",
        "one",
        "two",
        "first",
        "second",
        "END NESTED FORMATTING",
    )
    return [
        "Nested/mixed formatting did not corrupt or stop parser",
        "Lists, blockquote and code content remained present",
    ]


def phase2_link_limits(badge):
    content = phase2_open(badge, "phase2-limits.html")
    require_content(content, "START LIMITS", "END LIMITS")

    links, actions, forms = phase2_parser_counts(badge)
    if links != 128:
        raise RuntimeError(f"MAX_LINKS expected 128, parser reported {links}")
    if actions != 128:
        raise RuntimeError(f"Expected 128 link actions, parser reported {actions}")
    if forms != 0:
        raise RuntimeError(f"Expected no forms, parser reported {forms}")

    # The parser diagnostic above is the authoritative boundary check.
    # Do not re-parse the asynchronously interleaved console transcript here.

    return [
        "140 input links safely capped at MAX_LINKS=128",
        "Visible deterministic action range is exactly 1..128",
        "Parser reached END LIMITS after refusing additional links",
    ]


def phase2_form_limits(badge):
    content = phase2_open(badge, "phase2-form-limits.html")
    require_content(content, "START FORM LIMITS", "END FORM LIMITS")

    links, actions, forms = phase2_parser_counts(badge)
    if forms != 4:
        raise RuntimeError(f"MAX_FORMS expected 4, parser reported {forms}")

    # Each accepted form contains 10 editable inputs followed by a submit.
    # MAX_FORM_FIELDS=8 means only the first eight fields are retained; the
    # later submit is intentionally outside the accepted field budget.
    if actions != 32:
        raise RuntimeError(
            f"Expected 4 forms x 8 editable actions = 32, got {actions}"
        )

    numbered = numbered_actions(content)
    if len(numbered) != 32:
        raise RuntimeError(
            f"Expected 32 visible form-field actions, found {len(numbered)}"
        )

    return [
        "Six input forms safely capped at MAX_FORMS=4",
        "Ten fields/form safely capped at MAX_FORM_FIELDS=8",
        "Resulting action count remained bounded and deterministic at 32",
    ]


def command_setup(badge, setup):
    if setup is None:
        return

    if setup == "home":
        go_home(badge)
        return

    if setup == "home_then_wiby":
        activate_home_link(
            badge,
            5,
            r"HTTP 200.*https://wiby\.me",
        )
        return

    if setup == "home_wiby_back":
        activate_home_link(
            badge,
            5,
            r"HTTP 200.*https://wiby\.me",
        )

        badge.clear_log()
        badge.why("B")
        badge.wait_for(
            CONFIG["home"]["url_pattern"],
            20,
        )
        badge.settle(1.0)
        return

    raise ValueError(
        f"Unknown command setup: {setup}"
    )



def phase3_open_form(badge):
    return phase2_open(badge, "phase3-post-form.html")


def phase3_post_parser(badge):
    content = phase3_open_form(badge)
    joined = require_content(
        content,
        "POST FORM START",
        "WHY2025",
        "Hello Mini Browser!",
        "Send POST",
        "POST FORM END",
    )

    # Verify the rendered controls directly. numbered_actions() is intended
    # for compact single-line controls and is unnecessarily brittle when a
    # form control is wrapped by the pixel-aware renderer.
    for wanted in ("user: WHY2025", "message: Hello Mini Browser!", "[Send POST]"):
        if wanted not in joined:
            raise RuntimeError(f"Expected POST form control missing: {wanted}")

    links, actions_count, forms = phase2_parser_counts(badge)
    if (links, actions_count, forms) != (0, 3, 1):
        raise RuntimeError(
            f"Expected parser counts 0/3/1, got "
            f"{links}/{actions_count}/{forms}"
        )

    return [
        "method=post form parsed successfully",
        "Two editable fields plus submit produced three actions",
        "Hidden field retained without becoming an action",
    ]


def phase3_submit_default_post(badge):
    phase3_open_form(badge)
    badge.clear_log()

    # [1] user, [2] message, [3] Send POST
    badge.type_text("3")
    badge.enter()

    expected_body = (
        "user=WHY2025"
        "&message=Hello+Mini+Browser%21"
        "&token=A%26B%3D100%25"
        "&submit=Send"
    )

    badge.wait_for(
        r"\[mini_browser\] POST .*phase3-post\.php body="
        + re.escape(expected_body),
        15,
    )
    badge.wait_for(
        r"HTTP 200.*phase3-post\.php",
        DEFAULT_TIMEOUT,
    )
    badge.wait_for(r"POST RESULT END", 15)
    badge.settle(0.5)

    content = latest_content_block(badge)
    joined = require_content(
        content,
        "POST RESULT START",
        "METHOD: POST",
        "CONTENT-TYPE: application/x-www-form-urlencoded",
        "user: WHY2025",
        "message: Hello Mini Browser!",
        "token: A&B=100%",
        "submit: Send",
        "POST RESULT END",
    )

    # The raw body is a long unbroken token and Mini Browser deliberately
    # glyph-wraps overlong tokens. Rejoin rendered lines before exact compare.
    compact = "".join(line.strip() for line in content)
    if "RAW:" + expected_body not in compact:
        raise RuntimeError("Server did not receive the exact expected POST body")

    return [
        "Server received HTTP POST",
        "Content-Type was application/x-www-form-urlencoded",
        "Spaces/special characters were form-urlencoded correctly",
        "Hidden field and activated submit button were included",
        "Exact raw POST body verified by server response",
    ]


def phase3_submit_edited_post(badge):
    phase3_open_form(badge)
    badge.clear_log()

    # Edit action [2]: message.
    badge.type_text("2")
    badge.enter()
    badge.settle(0.3)

    # Existing value is exactly: Hello Mini Browser! (19 chars).
    for _ in range(len("Hello Mini Browser!")):
        badge.backspace()
    badge.type_text("POST works & yes")
    badge.enter()
    badge.settle(0.3)

    # Submit action [3].
    badge.type_text("3")
    badge.enter()

    expected_body = (
        "user=WHY2025"
        "&message=POST+works+%26+yes"
        "&token=A%26B%3D100%25"
        "&submit=Send"
    )

    badge.wait_for(
        r"\[mini_browser\] POST .*phase3-post\.php body="
        + re.escape(expected_body),
        15,
    )
    badge.wait_for(r"POST RESULT END", DEFAULT_TIMEOUT)
    badge.settle(0.5)

    content = latest_content_block(badge)
    joined = require_content(
        content,
        "METHOD: POST",
        "message: POST works & yes",
        "token: A&B=100%",
        "submit: Send",
        "POST RESULT END",
    )

    # Same wrapping rule as the default POST test above.
    compact = "".join(line.strip() for line in content)
    if "RAW:" + expected_body not in compact:
        raise RuntimeError("Edited value was not encoded in exact POST body")

    return [
        "Editable POST field changed before submission",
        "Edited value reached server intact",
        "Ampersand encoded as %26 in raw request body",
    ]




def phase4_open(badge, path, end_marker):
    """
    Phase 4 navigation deliberately does NOT go through WHY+H/home first.
    WHY+E works from any Mini Browser page and avoids an unrelated home-page
    request becoming a synchronization dependency for cookie tests.
    """
    badge.clear_log()
    badge.why("E")
    badge.settle(0.5)

    # WHY+E seeds https://, so type only host/path.
    badge.type_text("minibrowser.macip.net/" + path)
    badge.settle(0.2)
    badge.enter()
    badge.settle(0.4)

    badge.wait_for(
        r"HTTP 200.*https://minibrowser\.macip\.net/" + re.escape(path),
        DEFAULT_TIMEOUT,
    )
    badge.wait_for(re.escape(end_marker), 15)
    badge.settle(0.5)
    badge.view(path)
    return latest_content_block(badge)


def phase4_reset(badge):
    """Return the two Phase 4 cookie names to a known absent state."""
    content = phase4_open(badge, "phase4-cookie-reset.php", "END COOKIE RESET")
    require_content(content, "COOKIE RESET PAGE", "END COOKIE RESET")
    return content


def phase4_store_send(badge):
    # Independent test: never rely on cookie state left by another test.
    phase4_reset(badge)

    content = phase4_open(badge, "phase4-cookie-set.php", "END COOKIE SET")
    require_content(content, "COOKIE SET PAGE", "Set mb_session=alpha123")
    badge.wait_for(
        r"\[mini_browser\] cookie store: mb_session=alpha123 ",
        10,
    )

    content = phase4_open(badge, "phase4-cookie-check.php", "END COOKIE CHECK")
    require_content(content, "mb_session: alpha123")
    badge.wait_for(
        r"\[mini_browser\] cookie send: .*mb_session=alpha123",
        10,
    )
    return [
        "Started from a known empty Phase 4 cookie state",
        "Set-Cookie stored mb_session=alpha123",
        "Server received mb_session=alpha123 on the next HTTPS request",
    ]


def phase4_replace(badge):
    # Independent test: create the value that this test intends to replace.
    phase4_reset(badge)

    content = phase4_open(badge, "phase4-cookie-set.php", "END COOKIE SET")
    require_content(content, "Set mb_session=alpha123")
    badge.wait_for(
        r"\[mini_browser\] cookie store: mb_session=alpha123 ",
        10,
    )

    content = phase4_open(badge, "phase4-cookie-replace.php", "END COOKIE REPLACE")
    require_content(content, "Replaced mb_session with beta456")
    badge.wait_for(
        r"\[mini_browser\] cookie store: mb_session=beta456 ",
        10,
    )

    content = phase4_open(badge, "phase4-cookie-check.php", "END COOKIE CHECK")
    require_content(content, "mb_session: beta456")
    return [
        "Started from a known empty Phase 4 cookie state",
        "Created alpha123 and then replaced it with beta456",
        "Server received the replacement value beta456",
    ]


def phase4_path_scope(badge):
    # Independent test: no dependency on the session cookie tests.
    phase4_reset(badge)

    content = phase4_open(badge, "phase4-cookie-path-set.php", "END PATH COOKIE SET")
    require_content(content, "PATH COOKIE SET", "Set mb_path=private789")
    badge.wait_for(
        r"\[mini_browser\] cookie store: mb_path=private789 .*path=/phase4-private ",
        10,
    )

    # This URL is outside the cookie path and must not receive mb_path.
    content = phase4_open(badge, "phase4-cookie-check.php", "END COOKIE CHECK")
    require_content(content, "mb_path: (missing)")

    # This URL is inside /phase4-private and must receive it.
    content = phase4_open(badge, "phase4-private/check.php", "END PRIVATE COOKIE CHECK")
    require_content(content, "mb_path: private789")
    badge.wait_for(
        r"\[mini_browser\] cookie send: .*mb_path=private789",
        10,
    )
    return [
        "Started from a known empty Phase 4 cookie state",
        "Path cookie withheld outside /phase4-private",
        "Path cookie sent and received inside /phase4-private",
    ]


def phase4_delete(badge):
    # Independent test: create the cookie here before deleting it.
    phase4_reset(badge)

    content = phase4_open(badge, "phase4-cookie-set.php", "END COOKIE SET")
    require_content(content, "Set mb_session=alpha123")
    badge.wait_for(
        r"\[mini_browser\] cookie store: mb_session=alpha123 ",
        10,
    )

    content = phase4_open(badge, "phase4-cookie-delete.php", "END COOKIE DELETE")
    require_content(content, "COOKIE DELETE PAGE", "Deleted mb_session")
    badge.wait_for(
        r"\[mini_browser\] cookie delete: mb_session ",
        10,
    )

    content = phase4_open(badge, "phase4-cookie-check.php", "END COOKIE CHECK")
    require_content(content, "mb_session: (missing)")
    return [
        "Started from a known empty Phase 4 cookie state",
        "Created mb_session and deleted it with Max-Age=0",
        "Server confirmed the deleted cookie was not sent again",
    ]




def phase5_open(badge, path, end_marker):
    badge.clear_log()
    badge.why("E")
    badge.settle(0.5)
    badge.type_text("minibrowser.macip.net/" + path)
    badge.settle(0.2)
    badge.enter()
    badge.settle(0.4)
    badge.wait_for(
        r"HTTP 200.*https://minibrowser\.macip\.net/" + re.escape(path),
        DEFAULT_TIMEOUT,
    )
    badge.wait_for(re.escape(end_marker), 15)
    badge.settle(0.5)
    badge.view(path)
    return latest_content_block(badge)


def phase5_basic_page_info(badge):
    content = phase5_open(badge, "phase5-info.php", "END PHASE5 INFO")
    require_content(content, "PHASE 5 PAGE INFO", "END PHASE5 INFO")

    badge.clear_log()
    badge.why("I")
    badge.settle(0.5)

    line = badge.wait_for(
        r"\[mini_browser\] page info: status=200 bytes=[0-9]+ redirects=na "
        r"effective=na "
        r"content_type=(?:text/html(?:; charset=UTF-8)?|\(not reported\)) cookies=[0-9]+ "
        r"links=[0-9]+ forms=[0-9]+ actions=[0-9]+ "
        r"requested=https://minibrowser\.macip\.net/phase5-info\.php "
        r"final=\(not reported\)",
        15,
    )
    badge.wait_for(
        r"\[mini_browser\] inspector: method=GET body_bytes=0 cookies_sent=[0-9]+ "
        r"transport=HTTPS http_version=na remote_ip=na remote_port=na "
        r"tls=yes certinfo=na certverify=na",
        15,
    )
    return [
        "WHY+I opened HTTP / TLS Inspector",
        "HTTP status, size, Content-Type and object counts were retained",
        "Request method, transport and cookies-sent count were retained",
        "Unsupported connection/TLS CURLINFO fields are reported as unavailable",
        "Requested URL retained; effective/final URL is reported unavailable",
    ]


def phase5_redirect_info(badge):
    # This endpoint redirects once to phase5-info.php.
    phase5_open(badge, "phase5-redirect.php", "END PHASE5 INFO")

    badge.clear_log()
    badge.why("I")
    badge.settle(0.5)

    badge.wait_for(
        r"\[mini_browser\] page info: status=200 bytes=[0-9]+ redirects=na "
        r"effective=na "
        r"content_type=(?:text/html(?:; charset=UTF-8)?|\(not reported\)) cookies=[0-9]+ "
        r"links=[0-9]+ forms=[0-9]+ actions=[0-9]+ "
        r"requested=https://minibrowser\.macip\.net/phase5-redirect\.php "
        r"final=\(not reported\)",
        15,
    )
    return [
        "Redirect followed successfully (target page content was received)",
        "Requested URL retained",
        "Effective URL and redirect metadata are reported honestly as unavailable",
    ]


def phase5_return_from_info(badge):
    phase5_open(badge, "phase5-info.php", "END PHASE5 INFO")

    badge.clear_log()
    badge.why("I")
    badge.wait_for(r"\[mini_browser\] page info: status=200 ", 15)

    # WHY+B from the synthetic info page returns to the source URL.
    badge.clear_log()
    badge.why("B")
    badge.wait_for(
        r"HTTP 200.*https://minibrowser\.macip\.net/phase5-info\.php",
        DEFAULT_TIMEOUT,
    )
    badge.wait_for("END PHASE5 INFO", 15)
    return [
        "WHY+B left Page Information",
        "Original page URL was restored",
        "Original page loaded successfully after return",
    ]



# ---------------------------------------------------------------------------
# MINI BROWSER 4.1: OMNIBOX + HISTORY
# ---------------------------------------------------------------------------

V41_SITE = "https://minibrowser.macip.net/"


def v41_type_in_omnibox(badge, text, why_key="L"):
    """WHY+L opens an empty omnibox (WHY+E seeds https://), then type text."""
    badge.clear_log()
    badge.why(why_key)
    badge.settle(0.5)
    badge.type_text(text)
    badge.settle(0.4)


def v41_open_page(badge, path, wait_content=True):
    """Open a minibrowser.macip.net page through WHY+E (as the 3.0 tests do)."""
    badge.clear_log()
    badge.why("E")
    badge.settle(0.5)
    badge.type_text("minibrowser.macip.net/" + path)
    badge.settle(0.2)
    badge.enter()
    badge.wait_for(
        r"HTTP 200.*https://minibrowser\.macip\.net/" + re.escape(path),
        DEFAULT_TIMEOUT,
    )
    if wait_content:
        badge.wait_for(r"^--- CONTENT END ---$", 30)
    badge.settle(0.8)
    badge.view(path)


def v41_open_history(badge):
    """WHY+Y; returns (entry count, CONTENT lines of the history page)."""
    badge.clear_log()
    badge.why("Y")
    line = badge.wait_for(r"\[mini_browser\] opened history: (\d+) entries", 15)
    count = int(re.search(r"opened history: (\d+) entries", line).group(1))
    badge.settle(0.5)
    badge.view("history page")
    return count, latest_content_block(badge)


def v41_history_entries(content):
    """[(number, title, url)] from the rendered history page, in order."""
    entries = []
    lines = [line.strip() for line in content]
    for i, line in enumerate(lines):
        m = re.match(r"^\[(\d+)\]\s*(.*)$", line)
        if not m:
            continue
        url = ""
        for probe in lines[i + 1:i + 4]:
            if probe.startswith("http://") or probe.startswith("https://"):
                url = probe
                break
        entries.append((int(m.group(1)), m.group(2), url))
    return entries


def v41_search_empty_bar(badge):
    query = CONFIG["v41"]["search_query"]
    encoded = re.escape(query.replace(" ", "+"))
    v41_type_in_omnibox(badge, query, "L")
    badge.enter()
    badge.wait_for(
        r"\[mini_browser\] omnibox: search -> http://www\.google\.com/search\?q=" + encoded,
        15,
    )
    badge.wait_for(r"HTTP \d+.*google\.", DEFAULT_TIMEOUT)
    badge.settle(1.5)
    badge.view(f"search results for {query!r}")
    return [
        f"WHY+L opened an empty omnibox; typed {query!r}",
        "Text without a host name became a Google search",
        "Search request reached Google",
    ]


def v41_search_after_why_e(badge):
    # WHY+E seeds "https://"; a single word without a dot must still search.
    v41_type_in_omnibox(badge, "minibrowser", "E")
    badge.enter()
    badge.wait_for(
        r"\[mini_browser\] omnibox: search -> http://www\.google\.com/search\?q=minibrowser$",
        15,
    )
    badge.wait_for(r"HTTP \d+.*google\.", DEFAULT_TIMEOUT)
    badge.settle(1.5)
    badge.view("search results for 'minibrowser'")
    return [
        "WHY+E + 'minibrowser' (no dot) became a search",
        "3.0 would have tried to load https://minibrowser and failed",
    ]


def v41_host_opens_https(badge):
    path = CONFIG["v41"]["history_pages"][-1]
    v41_type_in_omnibox(badge, "minibrowser.macip.net/" + path, "L")
    badge.enter()
    badge.wait_for(
        r"HTTP 200.*https://minibrowser\.macip\.net/" + re.escape(path),
        DEFAULT_TIMEOUT,
    )
    badge.settle(1.0)
    if any("omnibox: search" in line for line in badge.get_lines()):
        raise RuntimeError("A host name was treated as a search")
    badge.view(path)
    return [
        "WHY+L + host/path without scheme opened over https://",
        "No search was triggered for a host name",
    ]


def v41_history_recorded(badge):
    pages = CONFIG["v41"]["history_pages"]
    for path in pages:
        v41_open_page(badge, path)

    count, content = v41_open_history(badge)
    require_content(content, "= HISTORY =")
    if count < len(pages):
        raise RuntimeError(f"History has {count} entries, expected at least {len(pages)}")

    entries = v41_history_entries(content)
    if not entries:
        raise RuntimeError("History page shows no numbered entries")

    # Most recent first: the last page opened must be entry [1].
    newest = entries[0]
    if not newest[2].endswith(pages[-1]):
        raise RuntimeError(f"Entry [1] is {newest[2]!r}, expected {V41_SITE + pages[-1]}")
    previous = entries[1] if len(entries) > 1 else None
    if not previous or not previous[2].endswith(pages[-2]):
        raise RuntimeError(f"Entry [2] is {previous!r}, expected {V41_SITE + pages[-2]}")

    badge.clear_log()
    badge.why("Y")   # back to the page
    badge.wait_for(r"HTTP 200.*https://minibrowser\.macip\.net/" + re.escape(pages[-1]), DEFAULT_TIMEOUT)
    badge.settle(0.8)
    return [
        f"Opened {len(pages)} pages, history shows {count} entries",
        f"[1] {newest[1]} -> {newest[2]}",
        f"[2] {previous[1]} -> {previous[2]}",
        "WHY+Y again returned to the page",
    ]


def v41_history_open_entry(badge):
    pages = CONFIG["v41"]["history_pages"]
    for path in pages:
        v41_open_page(badge, path)

    count, content = v41_open_history(badge)
    entries = v41_history_entries(content)
    target = next((e for e in entries if e[2].endswith(pages[-2])), None)
    if target is None:
        raise RuntimeError(f"{pages[-2]} not found on the history page")

    badge.clear_log()
    badge.type_text(str(target[0]))
    badge.settle(0.2)
    badge.enter()
    badge.wait_for(rf"activating link {target[0]}", 10)
    badge.wait_for(
        r"HTTP 200.*https://minibrowser\.macip\.net/" + re.escape(pages[-2]),
        DEFAULT_TIMEOUT,
    )
    badge.settle(1.0)
    badge.view(pages[-2])
    return [
        f"History entry [{target[0]}] {target[1]}",
        f"Number + Enter reopened {pages[-2]}",
    ]


def v41_suggestion(badge):
    page = CONFIG["v41"]["suggest_page"]
    typed = CONFIG["v41"]["suggest_typed"]
    v41_open_page(badge, page)

    v41_type_in_omnibox(badge, typed, "L")
    badge.view("omnibox suggestions")
    badge.press(0x51)        # Down: first suggestion
    badge.settle(0.6)
    badge.view("first suggestion selected")
    badge.enter()
    badge.wait_for(
        r"\[mini_browser\] omnibox: suggestion -> .*" + re.escape(page),
        15,
    )
    badge.wait_for(
        r"HTTP 200.*https://minibrowser\.macip\.net/" + re.escape(page),
        DEFAULT_TIMEOUT,
    )
    badge.settle(1.0)
    badge.view(page)
    return [
        f"Typed {typed!r}: suggestion list shown under the omnibox",
        f"Down + Enter opened {page} from the suggestion",
    ]


def v41_omnibox_escape(badge):
    page = CONFIG["v41"]["history_pages"][0]
    v41_open_page(badge, page)
    v41_type_in_omnibox(badge, "this text is never sent", "L")
    badge.press(0x29)        # Esc
    badge.settle(1.0)
    lines = badge.get_lines()
    if any("omnibox:" in line or "HTTP " in line for line in lines):
        raise RuntimeError("Esc in the omnibox still loaded something")
    # The page is still there: a reload must reload the same page.
    badge.clear_log()
    badge.why("R")
    badge.wait_for(r"HTTP 200.*https://minibrowser\.macip\.net/" + re.escape(page), DEFAULT_TIMEOUT)
    badge.settle(0.8)
    badge.view(page)
    return [
        "Esc closed the omnibox without loading anything",
        f"WHY+R reloaded the original page {page}",
    ]


def v41_clear_history(badge):
    page = CONFIG["v41"]["history_pages"][0]
    v41_open_page(badge, page)
    count, _ = v41_open_history(badge)
    badge.clear_log()
    badge.why("X")
    badge.wait_for(r"\[mini_browser\] history: cleared", 10)
    badge.settle(0.5)
    badge.view("cleared history page")
    # WHY+Y leaves the history page; open it again to read the new count.
    badge.clear_log()
    badge.why("Y")
    badge.wait_for(r"HTTP 200.*https://minibrowser\.macip\.net/" + re.escape(page), DEFAULT_TIMEOUT)
    badge.settle(0.8)
    count_after, content = v41_open_history(badge)
    entries = v41_history_entries(content)
    # Returning reloads the page, so it is the one and only new entry.
    if count_after != 1 or not entries or not entries[0][2].endswith(page):
        raise RuntimeError(f"Expected only {page} after WHY+X and one reload, found {count_after}: {entries}")
    badge.why("Y")
    return [f"WHY+X removed {count} entries", "History restarted with only the reloaded page"]


def v41_restart_keeps_history(badge):
    page = CONFIG["v41"]["history_pages"][0]
    v41_open_page(badge, page)
    badge.clear_log()
    badge.why("Q")
    badge.wait_for(r"\[mini_browser\] exit main", 20)
    badge.settle(2.0)
    badge.clear_log()
    badge.enter()            # launcher: start Mini Browser again
    line = badge.wait_for(r"\[mini_browser\] history: loaded (\d+) entries", 30)
    loaded = int(re.search(r"loaded (\d+) entries", line).group(1))
    badge.wait_for(CONFIG["home"]["url_pattern"], 60)
    badge.settle(1.0)
    count, content = v41_open_history(badge)
    if not any(e[2].endswith(page) for e in v41_history_entries(content)):
        raise RuntimeError(f"{page} missing from history after restart")
    badge.why("Y")
    return [f"History file loaded {loaded} entries at start", f"{page} survived the restart"]



# ---------------------------------------------------------------------------
# MINI BROWSER 4.1 PART 2: TABS
# ---------------------------------------------------------------------------

def tabs_reset_to_one(badge):
    """Close extra tabs left by an earlier (failed) test: WHY+W until one is left."""
    for _ in range(5):
        badge.clear_log()
        badge.why("W")
        badge.settle(0.6)
        lines = badge.get_lines()
        if not any("tab: close" in line for line in lines):
            break   # "LAST TAB": only one tab open


def tabs_new_and_open(badge, path):
    """WHY+T, then type a minibrowser.macip.net page in the new tab's omnibox."""
    badge.clear_log()
    badge.why("T")
    line = badge.wait_for(r"\[mini_browser\] tab: new (\d+)/(\d+)", 10)
    badge.wait_for(r"^--- CONTENT END ---$", 10)     # the new-tab page
    content = latest_content_block(badge)
    require_content(content, "= NEW TAB =")
    badge.view("new tab page")
    badge.type_text("minibrowser.macip.net/" + path)
    badge.settle(0.3)
    badge.enter()
    badge.wait_for(r"HTTP 200.*https://minibrowser\.macip\.net/" + re.escape(path), DEFAULT_TIMEOUT)
    badge.wait_for(r"^--- CONTENT END ---$", 30)
    badge.settle(0.8)
    badge.view(path)
    m = re.search(r"tab: new (\d+)/(\d+)", line)
    return int(m.group(1)), int(m.group(2))


def tabs_new_tab(badge):
    tabs_reset_to_one(badge)
    v41_open_page(badge, CONFIG["v41"]["history_pages"][0])
    pos, count = tabs_new_and_open(badge, CONFIG["v41"]["history_pages"][1])
    if (pos, count) != (2, 2):
        raise RuntimeError(f"Expected new tab 2/2, got {pos}/{count}")
    return [
        "WHY+T opened tab 2/2 with the new-tab page",
        f"Omnibox in the new tab opened {CONFIG['v41']['history_pages'][1]}",
    ]


def tabs_switch_keeps_page(badge):
    first, second = CONFIG["v41"]["history_pages"][0], CONFIG["v41"]["history_pages"][1]
    tabs_reset_to_one(badge)
    v41_open_page(badge, first)
    tabs_new_and_open(badge, second)

    # WHY+1: back to tab 1 without reloading it.
    badge.clear_log()
    badge.why_key(0x1E, ord("1"))
    badge.wait_for(r"\[mini_browser\] tab: switch 1/2 url=https://minibrowser\.macip\.net/" + re.escape(first), 10)
    badge.settle(1.5)
    if any(re.search(r"HTTP 200.*" + re.escape(first), line) for line in badge.get_lines()):
        raise RuntimeError("Switching tabs reloaded the page instead of keeping it")
    badge.view(f"tab 1: {first}")

    # WHY+Tab: next tab.
    badge.clear_log()
    badge.why_key(0x2B)
    badge.wait_for(r"\[mini_browser\] tab: switch 2/2 url=https://minibrowser\.macip\.net/" + re.escape(second), 10)
    badge.settle(0.8)
    badge.view(f"tab 2: {second}")
    tabs_reset_to_one(badge)
    return [
        "WHY+1 switched to tab 1 without a new page request",
        "WHY+Tab switched to the next tab",
    ]


def tabs_overview_and_close(badge):
    first, second = CONFIG["v41"]["history_pages"][0], CONFIG["v41"]["history_pages"][1]
    tabs_reset_to_one(badge)
    v41_open_page(badge, first)
    tabs_new_and_open(badge, second)

    badge.clear_log()
    badge.why("A")
    badge.wait_for(r"\[mini_browser\] tab: overview, 2 tabs, current 2", 10)
    badge.settle(0.5)
    badge.view("tab overview")
    badge.press(0x52)        # Up: tab 1
    badge.settle(0.3)
    badge.enter()
    badge.wait_for(r"\[mini_browser\] tab: switch 1/2", 10)
    badge.settle(0.8)

    badge.clear_log()
    badge.why("W")           # close tab 1, tab 2 moves to the front
    badge.wait_for(r"\[mini_browser\] tab: close 1/2 url=https://minibrowser\.macip\.net/" + re.escape(first), 10)
    badge.settle(1.0)
    badge.view(f"remaining tab: {second}")

    badge.clear_log()
    badge.why("W")           # last tab: must stay open
    badge.settle(1.0)
    if any("tab: close" in line for line in badge.get_lines()):
        raise RuntimeError("WHY+W closed the last tab")
    return [
        "WHY+A showed the overview with 2 tabs",
        "Up + Enter in the overview switched to tab 1",
        "WHY+W closed tab 1; the other tab came to the front",
        "WHY+W on the last tab was refused",
    ]


def tabs_limit(badge):
    tabs_reset_to_one(badge)
    for expected in range(2, 6):
        badge.clear_log()
        badge.why("T")
        badge.wait_for(rf"\[mini_browser\] tab: new {expected}/{expected}", 10)
        badge.settle(0.5)
        badge.press(0x29)    # Esc: leave the omnibox of the new tab
        badge.settle(0.3)
    badge.clear_log()
    badge.why("T")
    badge.wait_for(r"\[mini_browser\] tab: new refused, 5 tabs open", 10)
    badge.settle(0.5)
    badge.why("A")
    badge.settle(0.5)
    badge.view("tab overview with 5 tabs")
    badge.press(0x29)        # Esc: close the overview
    tabs_reset_to_one(badge)
    return ["Tabs 2..5 opened", "A sixth tab was refused", "Closed back to one tab"]


def tabs_cases(badge):
    return [
        ("4.1 Tabs: WHY+T opens a new tab", lambda: tabs_new_tab(badge)),
        ("4.1 Tabs: switching keeps the page (WHY+1, WHY+Tab)", lambda: tabs_switch_keeps_page(badge)),
        ("4.1 Tabs: overview (WHY+A) and close (WHY+W)", lambda: tabs_overview_and_close(badge)),
        ("4.1 Tabs: at most 5 tabs", lambda: tabs_limit(badge)),
    ]


# ---------------------------------------------------------------------------
# MINI BROWSER 4.3 PART 1: LOADING LINE, STOP, ERROR PAGES, BACK/FORWARD CACHE
# (needs BadgeVMS 4.3 firmware for stop, gzip and the precise error codes)
# ---------------------------------------------------------------------------

def v43_bfcache(badge):
    first, second = CONFIG["v41"]["history_pages"][:2]
    v41_open_page(badge, first)
    v41_open_page(badge, second)

    badge.clear_log()
    badge.why("B")
    badge.wait_for(r"\[mini_browser\] bfcache: hit https://minibrowser\.macip\.net/" + re.escape(first), 15)
    line = badge.wait_for(r"HTTP 200.*https://minibrowser\.macip\.net/" + re.escape(first), 10)
    badge.wait_for(r"^--- CONTENT END ---$", 10)
    if "(bfcache)" not in line:
        raise RuntimeError("Back loaded the page from the network instead of the cache")
    badge.settle(0.8)
    badge.view(first + " (from the cache)")

    badge.clear_log()
    badge.why("G")
    badge.wait_for(r"\[mini_browser\] bfcache: hit https://minibrowser\.macip\.net/" + re.escape(second), 15)
    badge.wait_for(r"^--- CONTENT END ---$", 10)
    badge.settle(0.8)
    badge.view(second + " (from the cache)")

    # Reload always uses the network.
    badge.clear_log()
    badge.why("R")
    line = badge.wait_for(r"HTTP 200.*https://minibrowser\.macip\.net/" + re.escape(second), DEFAULT_TIMEOUT)
    if "(bfcache)" in line:
        raise RuntimeError("WHY+R showed the cached page instead of reloading it")
    badge.wait_for(r"^--- CONTENT END ---$", 30)
    badge.settle(0.8)
    return [f"WHY+B showed {first} from the cache", f"WHY+G showed {second} from the cache",
            "WHY+R reloaded from the network"]


def v43_stop(badge):
    page = CONFIG["v41"]["history_pages"][0]
    v41_open_page(badge, page)
    v41_type_in_omnibox(badge, CONFIG["v43"]["slow_url"], "L")
    badge.enter()
    badge.settle(3.0)                 # connected, waiting for the slow answer
    badge.view("loading line")
    badge.press(0x29)                 # Esc
    badge.wait_for(r"\[mini_browser\] stop: Esc pressed", 10)
    badge.wait_for(r"\[mini_browser\] stop: nothing received, staying on https://minibrowser\.macip\.net/"
                   + re.escape(page), 10)
    badge.settle(1.0)
    lines = badge.get_lines()
    if any(re.search(r"HTTP \d+.*httpbin", line) for line in lines):
        raise RuntimeError("The slow page was shown although the load was stopped")
    badge.view("page kept after stop")
    # The address went back to the page that is still shown.
    badge.clear_log()
    badge.why("R")
    badge.wait_for(r"HTTP 200.*https://minibrowser\.macip\.net/" + re.escape(page), DEFAULT_TIMEOUT)
    badge.settle(0.8)
    return ["Esc stopped the slow load", f"{page} stayed on screen", "WHY+R reloaded that page"]


def v43_error_dns(badge):
    host = CONFIG["v43"]["bad_host"]
    v41_type_in_omnibox(badge, host, "L")
    badge.enter()
    line = badge.wait_for(r"\[mini_browser\] fetch error (\d+) URL='https://" + re.escape(host), DEFAULT_TIMEOUT)
    badge.wait_for(r"^--- CONTENT END ---$", 10)
    content = latest_content_block(badge)
    require_content(content, "This site can't be reached", "Press R or Enter to reload")
    code = re.search(r"fetch error (\d+)", line).group(1)
    joined = "\n".join(content)
    name = "ERR_NAME_NOT_RESOLVED" if "ERR_NAME_NOT_RESOLVED" in joined else "other error code"
    badge.settle(0.8)
    badge.view("DNS error page")

    # R (no WHY) on an error page loads the address again.
    badge.clear_log()
    badge.press(0x15, ord("r"))
    badge.wait_for(r"\[mini_browser\] error page: reload https://" + re.escape(host), 10)
    badge.wait_for(r"\[mini_browser\] fetch error \d+ URL='https://" + re.escape(host), DEFAULT_TIMEOUT)
    badge.settle(0.8)
    result = [f"curl {code}: {name}", "R reloaded the failed address"]
    if code != "5":
        result.append("(curl 5 needs BadgeVMS 4.3 firmware)")
    return result


def v43_error_404(badge):
    path = CONFIG["v43"]["missing_page"]
    badge.clear_log()
    badge.why("E")
    badge.settle(0.5)
    badge.type_text("minibrowser.macip.net/" + path)
    badge.settle(0.2)
    badge.enter()
    badge.wait_for(r"\[mini_browser\] HTTP 404 URL='https://minibrowser\.macip\.net/" + re.escape(path), DEFAULT_TIMEOUT)
    badge.wait_for(r"^--- CONTENT END ---$", 10)
    content = latest_content_block(badge)
    require_content(content, "This page can't be found", "HTTP ERROR 404")
    badge.settle(0.8)
    badge.view("404 page")
    return ["HTTP 404 shown as 'This page can't be found'"]


def v43_gzip(badge):
    url = CONFIG["v43"]["gzip_url"]
    v41_type_in_omnibox(badge, url, "L")
    badge.enter()
    line = badge.wait_for(r"\[mini_browser\] net: (\d+) bytes received, (\d+) bytes after decoding", DEFAULT_TIMEOUT)
    badge.wait_for(r"HTTP 200.*https://" + re.escape(url), DEFAULT_TIMEOUT)
    badge.wait_for(r"^--- CONTENT END ---$", 15)
    content = latest_content_block(badge)
    require_content(content, "gzipped")
    m = re.search(r"net: (\d+) bytes received, (\d+) bytes after decoding", line)
    badge.settle(0.8)
    badge.view("decoded gzip response")
    return [f"{m.group(1)} compressed bytes decoded to {m.group(2)} bytes"]


def v43_cases(badge):
    return [
        ("4.3 Back/Forward from the page cache, reload from the network", lambda: v43_bfcache(badge)),
        ("4.3 Esc stops a slow page, the old page stays", lambda: v43_stop(badge)),
        ("4.3 Error page: name not resolved, R reloads", lambda: v43_error_dns(badge)),
        ("4.3 Error page: HTTP 404", lambda: v43_error_404(badge)),
        ("4.3 gzip: compressed response decoded", lambda: v43_gzip(badge)),
    ]


# ---------------------------------------------------------------------------
# MINI BROWSER 4.3 PART 2: DISK CACHE, DOWNLOADS, SAVED COOKIES
# ---------------------------------------------------------------------------

def v43_cache_page(badge):
    page = CONFIG["v41"]["history_pages"][0]           # a static .html page
    v41_open_page(badge, page)
    badge.clear_log()
    badge.why("R")                                      # reload: asks the server
    line = badge.wait_for(
        r"\[mini_browser\] cache: (304 not modified|stored) https://minibrowser\.macip\.net/" + re.escape(page),
        DEFAULT_TIMEOUT)
    badge.wait_for(r"HTTP 200.*https://minibrowser\.macip\.net/" + re.escape(page), DEFAULT_TIMEOUT)
    badge.settle(0.8)
    if "stored" in line:
        # First time on this badge: the reload stored it; a second reload must hit.
        badge.clear_log()
        badge.why("R")
        badge.wait_for(r"\[mini_browser\] cache: 304 not modified https://minibrowser\.macip\.net/"
                       + re.escape(page), DEFAULT_TIMEOUT)
        badge.settle(0.8)
    return [f"{page}: reload answered with 304, page shown from the disk cache"]


def v43_cache_image(badge):
    url = CONFIG["phase3final"]["url"]
    image = CONFIG["phase3final"]["image_url_pattern"]
    badge.clear_log()
    badge.why("E")
    badge.settle(0.5)
    badge.type_text(url)
    badge.settle(0.2)
    badge.enter()
    badge.wait_for(CONFIG["phase3final"]["url_pattern"], DEFAULT_TIMEOUT)
    badge.wait_for(r"^--- CONTENT END ---$", 30)
    badge.settle(0.8)
    badge.clear_log()
    badge.why("R")
    badge.wait_for(r"\[mini_browser\] cache: (304 not modified|fresh) " + image, DEFAULT_TIMEOUT)
    badge.wait_for(CONFIG["phase3final"]["url_pattern"], DEFAULT_TIMEOUT)
    badge.settle(0.8)
    badge.view("images from the disk cache")
    return ["mb.png came from the disk cache on reload"]


def v43_download(badge):
    cfg = CONFIG["v43"]
    page = CONFIG["v41"]["history_pages"][0]
    v41_open_page(badge, page)
    v41_type_in_omnibox(badge, cfg["download_url"], "L")
    badge.enter()
    badge.wait_for(r"\[mini_browser\] download: offered " + re.escape(cfg["download_name"]), DEFAULT_TIMEOUT)
    badge.settle(1.0)
    badge.view("download question")
    badge.clear_log()
    badge.enter()
    line = badge.wait_for(r"\[mini_browser\] download: (saved|.*file removed)", 60)
    m = re.search(r"saved (\S+) \((\d+) bytes\)", line)
    if not m:
        raise RuntimeError(f"Download failed: {line}")
    if int(m.group(2)) != cfg["download_bytes"]:
        raise RuntimeError(f"Saved {m.group(2)} bytes, expected {cfg['download_bytes']}")
    badge.settle(1.0)
    badge.clear_log()
    badge.why("D")
    badge.wait_for(r"\[mini_browser\] opened downloads: (\d+) files", 15)
    badge.wait_for(r"^--- CONTENT END ---$", 10)
    content = latest_content_block(badge)
    require_content(content, cfg["download_name"])
    badge.view("downloads page")
    badge.why("D")                                      # back to the page
    badge.settle(1.0)
    return [f"Saved {m.group(1)} ({m.group(2)} bytes)", "Listed on the Downloads page (WHY+D)"]


def v43_download_cancel(badge):
    cfg = CONFIG["v43"]
    v41_type_in_omnibox(badge, cfg["attachment_url"], "L")
    badge.enter()
    line = badge.wait_for(r"\[mini_browser\] download: offered (\S+)", DEFAULT_TIMEOUT)
    badge.settle(1.0)
    badge.clear_log()
    badge.press(0x29)                                   # Esc
    badge.wait_for(r"\[mini_browser\] download: cancelled", 10)
    badge.settle(0.5)
    if any("download: saved" in l or "download: saving" in l for l in badge.get_lines()):
        raise RuntimeError("Esc did not cancel the download")
    name = re.search(r"offered (\S+)", line).group(1)
    return ["Content-Disposition: attachment offered a download", name + ": Esc cancelled it"]


def v43_clear_cookies_cache(badge):
    page = CONFIG["v41"]["history_pages"][0]
    v41_open_page(badge, page)
    badge.clear_log()
    badge.why("I")
    badge.wait_for(r"\[mini_browser\] page info: status=", 15)
    badge.settle(0.8)
    badge.view("page information")
    badge.clear_log()
    badge.why("X")
    badge.wait_for(r"\[mini_browser\] cookies: cleared \d+", 10)
    line = badge.wait_for(r"\[mini_browser\] cache: cleared (\d+) entries", 10)
    badge.why("B")
    badge.wait_for(r"HTTP 200.*https://minibrowser\.macip\.net/" + re.escape(page), DEFAULT_TIMEOUT)
    badge.settle(0.8)
    entries = re.search(r"(\d+) entries", line).group(1)
    return ["WHY+X on Page Information cleared cookies and " + entries + " cache files"]


def v43_cookie_restart(badge):
    cfg = CONFIG["v43"]
    v41_type_in_omnibox(badge, cfg["persistent_cookie_url"], "L")
    badge.enter()
    badge.wait_for(r"\[mini_browser\] cookie store: mbkeep=", DEFAULT_TIMEOUT)
    badge.wait_for(r"HTTP 200", DEFAULT_TIMEOUT)
    badge.settle(1.0)
    badge.clear_log()
    badge.why("Q")
    badge.wait_for(r"\[mini_browser\] cookies: saved [1-9]\d* of", 20)
    badge.wait_for(r"\[mini_browser\] exit main", 20)
    badge.settle(2.0)
    badge.clear_log()
    badge.enter()                                       # launcher: start again
    line = badge.wait_for(r"\[mini_browser\] cookies: loaded (\d+) saved", 30)
    badge.wait_for(CONFIG["home"]["url_pattern"], 60)
    badge.settle(1.0)
    v41_type_in_omnibox(badge, cfg["cookie_check_url"], "L")
    badge.enter()
    badge.wait_for(r"HTTP 200.*https://" + re.escape(cfg["cookie_check_url"]), DEFAULT_TIMEOUT)
    badge.wait_for(r"^--- CONTENT END ---$", 15)
    require_content(latest_content_block(badge), "mbkeep")
    loaded = re.search(r"loaded (\d+)", line).group(1)
    return [loaded + " saved cookie(s) loaded at start", "mbkeep sent again after the restart"]


def v43_dev2_cases(badge):
    cases = [
        ("4.3 Disk cache: reload of a page answered with 304", lambda: v43_cache_page(badge)),
        ("4.3 Disk cache: images from the cache on reload", lambda: v43_cache_image(badge)),
        ("4.3 Download: save a file, Downloads page (WHY+D)", lambda: v43_download(badge)),
        ("4.3 Download: attachment offered, Esc cancels", lambda: v43_download_cancel(badge)),
        ("4.3 Page Information: WHY+X clears cookies and cache", lambda: v43_clear_cookies_cache(badge)),
    ]
    if CONFIG["v43"]["test_cookie_restart"]:
        cases.append(("4.3 Cookies: Max-Age cookie survives a restart", lambda: v43_cookie_restart(badge)))
    return cases

def v41_cases(badge):
    cases = [
        ("4.1 Omnibox: search from empty bar (WHY+L)", lambda: v41_search_empty_bar(badge)),
        ("4.1 Omnibox: word without dot after WHY+E searches", lambda: v41_search_after_why_e(badge)),
        ("4.1 Omnibox: host without scheme opens https", lambda: v41_host_opens_https(badge)),
        ("4.1 Omnibox: Esc cancels without loading", lambda: v41_omnibox_escape(badge)),
        ("4.1 History: pages recorded, newest first (WHY+Y)", lambda: v41_history_recorded(badge)),
        ("4.1 History: open an entry by number", lambda: v41_history_open_entry(badge)),
        ("4.1 Omnibox: suggestion from history (Down + Enter)", lambda: v41_suggestion(badge)),
    ]
    if CONFIG["v41"]["test_restart"]:
        cases.append(("4.1 History: survives an app restart", lambda: v41_restart_keeps_history(badge)))
    if CONFIG["v41"]["test_clear_history"]:
        cases.append(("4.1 History: WHY+X clears the history", lambda: v41_clear_history(badge)))
    return cases


def show_final_result_page(badge, passed):
    """Leave Mini Browser displaying a clear PASS / NOT PASS result page."""
    filename = (
        "phase2-test-pass.html"
        if passed
        else "phase2-test-not-pass.html"
    )

    expected = (
        r"HTTP 200.*https://minibrowser\.macip\.net/"
        + re.escape(filename)
    )

    print(
        (
            "\n[FINAL PAGE] "
            + ("PASS" if passed else "NOT PASS")
            + f" -> {filename}"
        ),
        file=sys.stderr,
    )

    try:
        badge.clear_log()
        open_direct_url(
            badge,
            phase2_url(filename),
            expected,
        )
        badge.settle(1.0)
    except Exception as exc:
        print(
            f"\n[FINAL PAGE] Could not display result page: {exc}",
            file=sys.stderr,
        )


# ---------------------------------------------------------------------------
# TEST SELECTION / CLI
# ---------------------------------------------------------------------------

SELECTED_TESTS = None

def parse_test_selection(spec):
    selected = set()
    for part in spec.split(","):
        part = part.strip()
        if not part:
            continue
        if "-" in part:
            first, last = part.split("-", 1)
            first = int(first)
            last = int(last)
            if first < 1 or last < first:
                raise ValueError(f"Invalid test range: {part}")
            selected.update(range(first, last + 1))
        else:
            number = int(part)
            if number < 1:
                raise ValueError(f"Invalid test number: {part}")
            selected.add(number)
    if not selected:
        raise ValueError("No test numbers selected")
    return selected

def test_catalog():
    names = [
        "Load Mini Browser home page",
        "Action number: correct wrong digit with Backspace",
        "Phase 1: named HTML entities",
        "Phase 1: decimal numeric entities",
        "Phase 1: hexadecimal numeric entities",
        "Phase 1: Unicode entity pipeline",
        "Phase 1: invalid numeric entities",
        "Phase 1: malformed/unknown entities",
        "Phase 1: &amp; inside link URL",
        "Phase 1: entities in page title",
        "Phase 1: entities in button label",
        "2.6 Phase 1B: foreground colors and nesting",
        "2.6 Phase 2A: inline text styles and nesting",
        "2.6 Phase 2B: background colors and nesting",
        "3.0 Phase 3: experimental 5-image mode, five inline + sixth action, URL resolution",
        "3.0 Phase 3: Black & White placeholders, no image loading",
        "3.0 Phase 3: Colors placeholders, no image loading",
        "3.0 Phase 3: default 1-image mode, one inline + five image actions",
        "Phase 2: malformed HTML recovery",
        "Phase 2: link parsing/filtering",
        "Phase 2: form parser robustness",
        "Phase 2: raw Unicode parser path",
        "Phase 2: RTL/mixed-direction parser path",
        "Phase 2: huge unbroken words",
        "Phase 2: table parsing",
        "Phase 2: nested formatting",
        "Phase 2: MAX_LINKS/MAX_ACTIONS boundary",
        "Phase 2: MAX_FORMS/MAX_FORM_FIELDS boundary",
        "Phase 3: parse POST form",
        "Phase 3: submit default POST form",
        "Phase 3: submit edited POST form",
        "Phase 4: store and send cookie",
        "Phase 4: replace cookie",
        "Phase 4: path-scoped cookie",
        "Phase 4: delete cookie",
        "Phase 5B: HTTP/TLS Inspector",
        "Phase 5: redirect metadata",
        "Phase 5: return from Page Information",
    ]
    names.extend(f"Website: {site['name']}" for site in CONFIG["sites"])
    names.extend(f"Search: {item['name']}" for item in CONFIG["searches"])
    names.extend(f"Badge command: {item['name']}" for item in CONFIG["badge_commands"])
    names.extend(description for description, _ in v41_cases(None))
    names.extend(description for description, _ in tabs_cases(None))
    names.extend(description for description, _ in v43_cases(None))
    names.extend(description for description, _ in v43_dev2_cases(None))
    return names

def parse_args():
    parser = argparse.ArgumentParser(
        description="Mini Browser 3.0 / 4.1 / 4.3 / WHY2025 BadgeVMS regression tester",
        epilog=(
            "Examples:\n"
            "  %(prog)s                 Run all tests\n"
            "  %(prog)s --list          List tests and numbers\n"
            "  %(prog)s --test 30       Run only test 30\n"
            "  %(prog)s --test 30,31    Run tests 30 and 31\n"
            "  %(prog)s --test 29-31    Run tests 29 through 31\n"
            "  %(prog)s --view 6        Keep every page 6 s on screen\n"
            "  %(prog)s --fast          No viewing pauses"
        ),
        formatter_class=argparse.RawDescriptionHelpFormatter,
    )
    parser.add_argument("--list", action="store_true", help="list all tests and exit")
    parser.add_argument("-t", "--test", metavar="N[,N|N-M...]", help="run only selected test number(s)")
    parser.add_argument("--view", type=float, metavar="SECONDS",
                        help=f"seconds each page stays on screen (default {CONFIG['serial']['view_delay']:g})")
    parser.add_argument("--fast", action="store_true", help="no viewing pauses (same as --view 0)")
    return parser.parse_args()

# ---------------------------------------------------------------------------
# TEST FRAMEWORK
# ---------------------------------------------------------------------------

def run_test(
    results,
    number,
    description,
    test_func,
):
    if SELECTED_TESTS is not None and number not in SELECTED_TESTS:
        return

    print(
        f"\n\n========== TEST {number}: {description} ==========",
        file=sys.stderr,
    )

    try:
        details = test_func()

        if details is None:
            details = []

        elif isinstance(details, str):
            details = [details]

        results.append(
            {
                "number": number,
                "description": description,
                "passed": True,
                "error": "",
                "details": list(details),
            }
        )

        print(
            f"\nPASS: {description}",
            file=sys.stderr,
        )

    except Exception as exc:
        results.append(
            {
                "number": number,
                "description": description,
                "passed": False,
                "error": str(exc),
                "details": [],
            }
        )

        print(
            f"\nNOT PASSED: {description}",
            file=sys.stderr,
        )
        print(
            f"Reason: {exc}",
            file=sys.stderr,
        )


def print_summary(results):
    print(
        "\n\n============================================================",
        file=sys.stderr,
    )
    print(
        "CONFIGURABLE MINI BROWSER TEST SUMMARY",
        file=sys.stderr,
    )
    print(
        "============================================================",
        file=sys.stderr,
    )

    for result in results:
        status = (
            "PASS"
            if result["passed"]
            else "NOT PASSED"
        )

        print(
            (
                f"{result['number']:>2}. "
                f"{status:<10} "
                f"{result['description']}"
            ),
            file=sys.stderr,
        )

        if result["error"]:
            print(
                f"    {result['error']}",
                file=sys.stderr,
            )

        for detail in result["details"]:
            print(
                f"    > {detail}",
                file=sys.stderr,
            )

    passed = sum(
        1
        for result in results
        if result["passed"]
    )

    total = len(results)

    print(
        "------------------------------------------------------------",
        file=sys.stderr,
    )
    print(
        f"Passed:     {passed}/{total}",
        file=sys.stderr,
    )
    failed = [
        result
        for result in results
        if not result["passed"]
    ]

    print(
        f"Not passed: {len(failed)}/{total}",
        file=sys.stderr,
    )
    print(
        "============================================================",
        file=sys.stderr,
    )

    if failed:
        print(
            "\nNOT PASSED TESTS — COPY/PASTE SUMMARY",
            file=sys.stderr,
        )
        print(
            "------------------------------------------------------------",
            file=sys.stderr,
        )
        for result in failed:
            print(
                f"{result['number']}. NOT PASSED {result['description']}",
                file=sys.stderr,
            )
            if result["error"]:
                print(
                    f"    {result['error']}",
                    file=sys.stderr,
                )
        print(
            "------------------------------------------------------------",
            file=sys.stderr,
        )
    else:
        print(
            "\nAll tests passed.",
            file=sys.stderr,
        )

    print("", file=sys.stderr)


# ---------------------------------------------------------------------------
# MAIN
# ---------------------------------------------------------------------------

def main():
    global SELECTED_TESTS, VIEW_DELAY

    args = parse_args()
    if args.fast:
        VIEW_DELAY = 0
    elif args.view is not None:
        VIEW_DELAY = max(0.0, args.view)
    catalog = test_catalog()

    if args.list:
        for number, description in enumerate(catalog, 1):
            print(f"{number:2d}. {description}")
        return 0

    if args.test:
        try:
            SELECTED_TESTS = parse_test_selection(args.test)
        except ValueError as exc:
            print(f"Error: {exc}", file=sys.stderr)
            return 2

        invalid = sorted(number for number in SELECTED_TESTS if number > len(catalog))
        if invalid:
            print(
                f"Error: unknown test number(s): {', '.join(map(str, invalid))}. "
                f"Valid range is 1-{len(catalog)}.",
                file=sys.stderr,
            )
            return 2

    badge = Badge()
    results = []
    number = 1

    print(
        "\nConfigurable Mini Browser regression test",
        file=sys.stderr,
    )
    print(
        f"Serial device: {DEVICE}",
        file=sys.stderr,
    )
    print(
        f"Viewing pause per page: {VIEW_DELAY:g}s",
        file=sys.stderr,
    )

    try:
        time.sleep(
            CONFIG["serial"]["startup_delay"]
        )

        # Start Mini Browser when the BadgeVMS menu is still shown.
        # If Mini Browser is already running, this is only a harmless Enter.
        print(
            "\n[STARTUP] Pressing ENTER once to start/confirm Mini Browser",
            file=sys.stderr,
        )
        badge.enter()
        badge.settle(1.0)

        # First verify that Mini Browser itself is alive.
        run_test(
            results,
            number,
            "Load Mini Browser home page",
            lambda: go_home(badge),
        )
        number += 1

        # Phase 0.5: action-number input must be editable with Backspace.
        run_test(
            results,
            number,
            "Action number: correct wrong digit with Backspace",
            lambda: test_action_number_backspace(badge),
        )
        number += 1

        # Mini Browser 2.5 Phase 1: controlled HTML/entity regressions.
        phase1_cases = [
            ("Phase 1: named HTML entities", lambda: phase1_named_entities(badge)),
            ("Phase 1: decimal numeric entities", lambda: phase1_decimal_entities(badge)),
            ("Phase 1: hexadecimal numeric entities", lambda: phase1_hex_entities(badge)),
            ("Phase 1: Unicode entity pipeline", lambda: phase1_unicode_entities(badge)),
            ("Phase 1: invalid numeric entities", lambda: phase1_invalid_entities(badge)),
            ("Phase 1: malformed/unknown entities", lambda: phase1_malformed_entities(badge)),
            ("Phase 1: &amp; inside link URL", lambda: phase1_entity_url(badge)),
            ("Phase 1: entities in page title", lambda: phase1_title_entity(badge)),
            ("Phase 1: entities in button label", lambda: phase1_button_entity(badge)),
        ]

        for description, test_func in phase1_cases:
            run_test(
                results,
                number,
                description,
                test_func,
            )
            number += 1

        # Mini Browser 2.6 Phase 1B: bounded HTML/inline-CSS foreground colors.
        run_test(
            results,
            number,
            "2.6 Phase 1B: foreground colors and nesting",
            lambda: phase1b_foreground_colors(badge),
        )
        number += 1

        # Mini Browser 2.6 Phase 2A: bounded inline text styling.
        run_test(
            results,
            number,
            "2.6 Phase 2A: inline text styles and nesting",
            lambda: phase2a_inline_text_styles(badge),
        )
        number += 1

        # Mini Browser 2.6 Phase 2B: bounded inline background colors.
        run_test(
            results,
            number,
            "2.6 Phase 2B: background colors and nesting",
            lambda: phase2b_background_colors(badge),
        )
        number += 1

        # Mini Browser 3.0 Phase 3: final display-mode + image regressions.
        phase3_visual_cases = [
            (
                "3.0 Phase 3: experimental 5-image mode, five inline + sixth action, URL resolution",
                lambda: phase3_colors_images(badge),
            ),
            (
                "3.0 Phase 3: Black & White placeholders, no image loading",
                lambda: phase3_black_white(badge),
            ),
            (
                "3.0 Phase 3: Colors placeholders, no image loading",
                lambda: phase3_colors_no_images(badge),
            ),
            (
                "3.0 Phase 3: default 1-image mode, one inline + five image actions",
                lambda: phase3_restore_colors_images(badge),
            ),
        ]

        for description, test_func in phase3_visual_cases:
            run_test(
                results,
                number,
                description,
                test_func,
            )
            number += 1

        # Mini Browser 2.5 Phase 2: parser torture and hard-limit regressions.
        phase2_cases = [
            ("Phase 2: malformed HTML recovery", lambda: phase2_malformed(badge)),
            ("Phase 2: link parsing/filtering", lambda: phase2_links(badge)),
            ("Phase 2: form parser robustness", lambda: phase2_forms(badge)),
            ("Phase 2: raw Unicode parser path", lambda: phase2_unicode(badge)),
            ("Phase 2: RTL/mixed-direction parser path", lambda: phase2_rtl(badge)),
            ("Phase 2: huge unbroken words", lambda: phase2_huge_words(badge)),
            ("Phase 2: table parsing", lambda: phase2_tables(badge)),
            ("Phase 2: nested formatting", lambda: phase2_nested_formatting(badge)),
            ("Phase 2: MAX_LINKS/MAX_ACTIONS boundary", lambda: phase2_link_limits(badge)),
            ("Phase 2: MAX_FORMS/MAX_FORM_FIELDS boundary", lambda: phase2_form_limits(badge)),
        ]

        for description, test_func in phase2_cases:
            run_test(
                results,
                number,
                description,
                test_func,
            )
            number += 1

        # Mini Browser 2.5 Phase 3: bounded application/x-www-form-urlencoded POST.
        phase3_cases = [
            ("Phase 3: parse POST form", lambda: phase3_post_parser(badge)),
            ("Phase 3: submit default POST form", lambda: phase3_submit_default_post(badge)),
            ("Phase 3: submit edited POST form", lambda: phase3_submit_edited_post(badge)),
        ]

        for description, test_func in phase3_cases:
            run_test(
                results,
                number,
                description,
                test_func,
            )
            number += 1

        # Mini Browser 2.5 Phase 4: bounded in-memory session cookies.
        phase4_cases = [
            ("Phase 4: store and send cookie", lambda: phase4_store_send(badge)),
            ("Phase 4: replace cookie", lambda: phase4_replace(badge)),
            ("Phase 4: path-scoped cookie", lambda: phase4_path_scope(badge)),
            ("Phase 4: delete cookie", lambda: phase4_delete(badge)),
        ]

        for description, test_func in phase4_cases:
            run_test(
                results,
                number,
                description,
                test_func,
            )
            number += 1

        # Mini Browser 2.5 Phase 5: Page Information.
        phase5_cases = [
            ("Phase 5B: HTTP/TLS Inspector", lambda: phase5_basic_page_info(badge)),
            ("Phase 5: redirect metadata", lambda: phase5_redirect_info(badge)),
            ("Phase 5: return from Page Information", lambda: phase5_return_from_info(badge)),
        ]

        for description, test_func in phase5_cases:
            run_test(
                results,
                number,
                description,
                test_func,
            )
            number += 1

        # Configured sites.
        for site in CONFIG["sites"]:
            def site_test(site=site):
                mode = site["mode"]

                if mode == "url":
                    open_direct_url(
                        badge,
                        site["url"],
                        site["expect"],
                        wait_for_content=site.get("wait_for_content", True),
                    )
                    return [
                        f"URL: {site['url']}"
                    ]

                if mode == "home_link":
                    activate_home_link(
                        badge,
                        site["action"],
                        site["expect"],
                    )
                    return [
                        f"Home action: [{site['action']}]"
                    ]

                raise ValueError(
                    f"Unknown site mode: {mode}"
                )

            run_test(
                results,
                number,
                f"Website: {site['name']}",
                site_test,
            )
            number += 1

        # Configured searches.
        for search_config in CONFIG["searches"]:
            run_test(
                results,
                number,
                f"Search: {search_config['name']}",
                lambda search_config=search_config: perform_search(
                    badge,
                    search_config,
                ),
            )
            number += 1

        # Configured badge/browser commands.
        for command_config in CONFIG["badge_commands"]:
            def command_test(
                command_config=command_config
            ):
                command_setup(
                    badge,
                    command_config.get("setup"),
                )

                badge.clear_log()
                badge.why(
                    command_config["command"]
                )

                badge.wait_for(
                    command_config["expect"],
                    DEFAULT_TIMEOUT,
                )

                badge.settle(0.8)
                badge.view(f"after WHY+{command_config['command'].upper()}")

                return [
                    (
                        f"WHY+"
                        f"{command_config['command'].upper()}"
                    )
                ]

            run_test(
                results,
                number,
                (
                    "Badge command: "
                    f"{command_config['name']}"
                ),
                command_test,
            )
            number += 1

        # Mini Browser 4.1: omnibox and history.
        for description, test_func in v41_cases(badge):
            run_test(
                results,
                number,
                description,
                test_func,
            )
            number += 1

        # Mini Browser 4.1 part 2: tabs.
        for description, test_func in tabs_cases(badge):
            run_test(
                results,
                number,
                description,
                test_func,
            )
            number += 1

        # Mini Browser 4.3 part 1: stop, error pages, back/forward cache, gzip.
        for description, test_func in v43_cases(badge):
            run_test(
                results,
                number,
                description,
                test_func,
            )
            number += 1

        # Mini Browser 4.3 part 2: disk cache, downloads, saved cookies.
        for description, test_func in v43_dev2_cases(badge):
            run_test(
                results,
                number,
                description,
                test_func,
            )
            number += 1

        all_passed = all(
            result["passed"]
            for result in results
        )

        # Leave the badge on its visual PASS / NOT PASS result page first.
        show_final_result_page(
            badge,
            all_passed,
        )

        # Keep the full human-readable console result as the final output.
        # Nothing from the badge is printed after this summary.
        print_summary(results)

        return 0 if all_passed else 1

    finally:
        badge.close()


if __name__ == "__main__":
    sys.exit(main())
