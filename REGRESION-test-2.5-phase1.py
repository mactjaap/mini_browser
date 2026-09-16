#!/usr/bin/env python3

"""
Configurable Mini Browser / WHY2025 BadgeVMS regression tester.

Edit only the CONFIG section for normal use.

Tests supported:
- Load arbitrary web sites through WHY+E
- Activate numbered links from the Mini Browser home page
- Run GET-form searches
- Exercise WHY-key badge/browser commands
- Verify action-number correction with Backspace
- Verify Mini Browser 2.5 Phase 1 HTML/entity handling
- Print a PASS / NOT PASSED summary

Serial keyboard protocol:
    E <scancode-hex> <down 0|1> <text-hex>

Example:
    E 28 1 00
    E 28 0 00
"""

import re
import sys
import time
import tty
import termios
import threading

import serial


# ---------------------------------------------------------------------------
# CONFIG
# ---------------------------------------------------------------------------

CONFIG = {
    "serial": {
        "device": "/dev/cu.wchusbserial110",
        "baudrate": 115200,
        "startup_delay": 2.0,
        "default_timeout": 1000,
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


# ---------------------------------------------------------------------------
# CONSTANTS
# ---------------------------------------------------------------------------

DEVICE = CONFIG["serial"]["device"]
BAUDRATE = CONFIG["serial"]["baudrate"]
DEFAULT_TIMEOUT = CONFIG["serial"]["default_timeout"]


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

        print(
            f"\n[TEST TX] {command.strip()}",
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
        time.sleep(0.08)
        self.send_event(
            key_scancode,
            True,
            ord(letter.lower()),
        )
        time.sleep(0.08)
        self.send_event(key_scancode, False, 0)
        time.sleep(0.08)
        self.send_event(why_scancode, False, 0)
        time.sleep(0.30)

    def settle(self, seconds=0.5):
        self.serial.flush()
        time.sleep(seconds)

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

    def type_text(self, text, key_delay=0.06):
        for character in text:
            scancode = ascii_scancode(character)

            if scancode is None:
                raise ValueError(
                    f"Unsupported character: {character!r}"
                )

            self.press(
                scancode,
                ord(character),
                delay=0.04,
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
    badge.wait_for(
        CONFIG["home"]["url_pattern"],
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

    badge.settle(1.0)


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
):
    go_home(badge)
    badge.clear_log()

    badge.why("E")
    badge.settle(0.5)

    # WHY+E already seeds "https://".
    if url.startswith("https://"):
        url = url[len("https://"):]

    badge.type_text(url)
    badge.settle(0.2)
    badge.enter()
    badge.settle(0.4)

    badge.wait_for(
        expected_pattern,
        DEFAULT_TIMEOUT,
    )

    badge.settle(1.0)


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
    # Use a unique query string so this test does not collide with a bookmark
    # the user may already have for the ordinary Phase 1 test page.
    url = CONFIG["phase1"]["url"] + "?titlecheck=1"
    expected = (
        r"HTTP 200.*https://minibrowser\.macip\.net/"
        r"phase1-entities\.html\?titlecheck=1"
    )
    phase1_open(badge, url, expected)

    badge.clear_log()
    badge.why("F")
    badge.wait_for(r"bookmark added: .*phase1-entities\.html\?titlecheck=1", 10)

    badge.clear_log()
    badge.why("M")
    badge.wait_for(r"opened bookmarks:", 10)
    badge.settle(0.8)

    content = latest_content_block(badge)
    require_content(content, "Phase 1 & Entities 😀")

    # Return to the page and remove the temporary bookmark.
    badge.clear_log()
    badge.why("B")
    badge.wait_for(expected, 20)
    badge.settle(0.5)

    badge.clear_log()
    badge.why("F")
    badge.wait_for(r"bookmark removed: .*phase1-entities\.html\?titlecheck=1", 10)

    return [
        "Decoded <title> verified through the bookmark title",
        "Temporary regression-test bookmark removed",
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


# ---------------------------------------------------------------------------
# TEST FRAMEWORK
# ---------------------------------------------------------------------------

def run_test(
    results,
    number,
    description,
    test_func,
):
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
    print(
        f"Not passed: {total - passed}/{total}",
        file=sys.stderr,
    )
    print(
        "============================================================\n",
        file=sys.stderr,
    )


# ---------------------------------------------------------------------------
# MAIN
# ---------------------------------------------------------------------------

def main():
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

    try:
        time.sleep(
            CONFIG["serial"]["startup_delay"]
        )

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

        # Configured sites.
        for site in CONFIG["sites"]:
            def site_test(site=site):
                mode = site["mode"]

                if mode == "url":
                    open_direct_url(
                        badge,
                        site["url"],
                        site["expect"],
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

        print_summary(results)

        return (
            0
            if all(
                result["passed"]
                for result in results
            )
            else 1
        )

    finally:
        badge.close()


if __name__ == "__main__":
    sys.exit(main())
