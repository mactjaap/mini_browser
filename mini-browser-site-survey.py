#!/usr/bin/env python3
"""
Mini Browser site survey (4.5 development).

Loads a fixed set of 20 news and tech sites on the badge, one after the
other, and records per site what arrived and what the reader gets to see:

  status      HTTP status, or the curl error
  bytes       page bytes the browser kept (4.4: at most 65536)
  cut         the page was cut off at the size limit
  links       links the parser found
  text        characters of readable text on the page
  flags       cookie = probably a cookie / consent wall
              js     = probably needs JavaScript (little text, or says so)

Run it once on 4.4 for a baseline, and again after each 4.5 step:

    ./mini-browser-site-survey.py --label 4.4
    ./mini-browser-site-survey.py --label 4.5-dev1
    ./mini-browser-site-survey.py --compare site-survey-4.4.csv site-survey-4.5-dev1.csv

Watching on the badge: --view 10 keeps every site on screen for 10 seconds.

Screenshots: --screenshots takes a WHY+S screenshot of every site (or a
whole-page WHY+Z one with --full-page) into site-survey-<label>/, with an
index.html that shows all of them next to their numbers.  Screenshots use
badge_screenshot.py (same folder) to check and repair the transfer.

    ./mini-browser-site-survey.py --label 4.5-dev1-look --view 10
    ./mini-browser-site-survey.py --label 4.5-dev1-shots --screenshots
    ./mini-browser-site-survey.py --sheet 4.5-dev1-shots     (only rebuild index.html)
    ./mini-browser-site-survey.py --label 4.5-dev2-web --screenshots --look web

It needs the custom firmware with the serial keyboard bridge (as the
regression suite) and reuses the regression suite's serial code.  All
serial output goes to site-survey-<label>.log, the results to
site-survey-<label>.csv and a table on the screen.
"""

import argparse
import csv
import subprocess
import importlib.util
import os
import re
import sys
import time
from pathlib import Path

HERE = os.path.dirname(os.path.abspath(__file__))

SITES = [
    # (group, name, address as typed after WHY+L)
    ("news", "NOS", "nos.nl"),
    ("news", "NU.nl", "nu.nl"),
    ("news", "de Volkskrant", "volkskrant.nl"),
    ("news", "BBC News", "bbc.com/news"),
    ("news", "The Guardian", "theguardian.com"),
    ("news", "Reuters", "reuters.com"),
    ("news", "CNN lite", "lite.cnn.com"),
    ("news", "NPR text", "text.npr.org"),
    ("news", "DW News", "dw.com/en"),
    ("news", "AP News", "apnews.com"),
    ("tech", "Tweakers", "tweakers.net"),
    ("tech", "Hacker News", "news.ycombinator.com"),
    ("tech", "Lobsters", "lobste.rs"),
    ("tech", "LWN.net", "lwn.net"),
    ("tech", "Ars Technica", "arstechnica.com"),
    ("tech", "The Register", "theregister.com"),
    ("tech", "Heise", "heise.de"),
    ("tech", "GitHub", "github.com/mactjaap/mini_browser"),
    ("tech", "Wikipedia", "en.wikipedia.org/wiki/ESP32"),
    ("tech", "Stack Overflow", "stackoverflow.com/questions/tagged/esp32"),
]

COOKIE_WORDS = ("cookie", "consent", "toestemming", "akkoord", "accept all",
                "accepteer", "privacy settings", "manage choices")
JS_WORDS = ("enable javascript", "javascript is disabled", "javascript is required",
            "javascript uitgeschakeld", "turn on javascript", "please enable js")

FIELDS = ["group", "name", "address", "status", "final_url", "bytes", "received",
          "cut", "links", "text", "flags", "seconds", "screenshot"]


def load_regression():
    path = os.path.join(HERE, "mini-browser-3.0-regression-selectable.py")
    spec = importlib.util.spec_from_file_location("mb_regression", path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def load_screenshot_module():
    path = os.path.join(HERE, "badge_screenshot.py")
    spec = importlib.util.spec_from_file_location("mb_screenshot", path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def take_lines(badge):
    """All serial lines received so far, removed from the badge's log."""
    with badge.lock:
        lines = badge.lines
        badge.lines = []
    return lines


class ShotProgress:
    """How much of the picture has arrived, for the progress bar.

    The picture is sent as RLE records of 5 bytes (2-byte count + RGB) cut
    into chunks of chunk_size bytes, chunk n starting at byte n*chunk_size.
    Adding up the counts of the records received so far gives the pixels
    received; the picture has width*height pixels.  A record cut in two by
    a chunk border is counted when both halves are there; a lost chunk
    (repaired from parity at the end) is simply missing from the sum."""

    def __init__(self, header):
        self.size = header.chunk_size
        self.total = max(1, header.width * header.height)
        self.pixels = 0
        self.done = set()
        self.first_byte = {}       # chunk -> its first byte
        self.waiting_low = {}      # chunk -> low count byte of a record cut at its end

    def add(self, seq, payload):
        if seq in self.done or not payload:
            return
        self.done.add(seq)
        start = seq * self.size
        end = start + len(payload)
        self.first_byte[seq] = payload[0]
        r = start + (-start) % 5               # first record that starts in this chunk
        while r < end:
            if r + 1 < end:
                self.pixels += payload[r - start] | (payload[r - start + 1] << 8)
            elif seq + 1 in self.first_byte:   # next chunk already here
                self.pixels += payload[r - start] | (self.first_byte[seq + 1] << 8)
            else:
                self.waiting_low[seq] = payload[r - start]
            r += 5
        if seq - 1 in self.waiting_low:        # finish the previous chunk's record
            self.pixels += self.waiting_low.pop(seq - 1) | (payload[0] << 8)

    def fraction(self):
        return min(1.0, self.pixels / self.total)


def fmt_time(seconds):
    seconds = int(max(0, seconds))
    h, rest = divmod(seconds, 3600)
    m, s = divmod(rest, 60)
    return f"{h}:{m:02d}:{s:02d}" if h else f"{m:02d}:{s:02d}"


def progress_bar(fraction, width=30):
    full = int(fraction * width)
    return "[" + "=" * full + (">" if full < width else "") + " " * max(0, width - full - 1) + "]"


def capture_screenshot(badge, shot, path, full_page=False, timeout=180.0):
    """WHY+S (or WHY+Z) and receive the IMG transfer from the serial log.
    Returns a short status text."""
    if any("caused an unhandled exception" in l for l in take_lines(badge)):
        return "CRASH (before the screenshot)"
    badge.why("Z" if full_page else "S")
    state = None
    tried = []
    records = 0
    shown = 0.0
    asked = time.monotonic()
    progress = None
    begun = None
    last_activity = time.monotonic()
    end_wait = None
    error = None
    while True:
        lines = take_lines(badge)
        if lines:
            last_activity = time.monotonic()
        for raw in lines:
            line = raw.strip()
            if "caused an unhandled exception" in line:
                return "CRASH"
            kind, value = shot.parse_line(line, state)
            if kind == "data":
                records += 1
                if progress is not None:
                    m = shot.DATA_RE.search(line)
                    if m:
                        seq = int(m.group(1))
                        cands = state.data_chunks.get(seq)
                        if cands:
                            progress.add(seq, cands[-1])
            if kind == "fail":
                return f"failed: {value}"
            if kind == "begin":
                state = shot.TransferState(value)
                tried, end_wait = [], None
                progress, begun, records = ShotProgress(value), time.monotonic(), 0
            elif kind == "end" and state is not None:
                end = shot.parse_end(value, state.header)
                if end is not None and end not in tried:
                    tried.append(end)
                    try:
                        compressed, repaired = shot.assemble(state, end)
                    except ValueError as exc:
                        error = exc
                        end_wait = end_wait or time.monotonic() + 2.0
                        continue
                    h = state.header
                    rgb = shot.decode_rle5(compressed, h.width, h.height)
                    shot.write_png_rgb(Path(path), h.width, h.height, rgb)
                    took = fmt_time(time.monotonic() - asked)
                    return f"ok ({h.width}x{h.height}, {repaired} repaired, {took})"
                end_wait = end_wait or time.monotonic() + 2.0
        now = time.monotonic()
        if now - shown >= 1.0:                            # progress on the screen
            if state is None:
                line = f"  {'rendering the whole page' if full_page else 'screenshot'} ... {fmt_time(now - asked)}"
            else:
                h = state.header
                frac = progress.fraction()
                spent = now - begun
                eta = f"ETA {fmt_time(spent / frac - spent)}" if frac > 0.01 else "ETA --:--"
                kbps = records * h.chunk_size / 1024 / spent if spent > 0 else 0.0
                line = (f"  {progress_bar(frac)} {frac * 100:5.1f}%  {h.width}x{h.height}  "
                        f"{kbps:4.1f} KB/s  {fmt_time(spent)}  {eta}")
            sys.__stdout__.write("\r" + line.ljust(98)[:98])
            sys.__stdout__.flush()
            shown = now
        if end_wait and now > end_wait:
            return f"damaged: {error}" if error else "damaged"
        # WHY+Z first renders the whole page: allow more time before BEGIN.
        if now - last_activity > ((60.0 if full_page else 20.0) if state is None else timeout):
            return "no screenshot received"
        time.sleep(0.05)


def site_link(row):
    """The address to open in a normal browser: where the badge ended up, else the start address."""
    url = (row.get("final_url") or "").strip()
    if not url.startswith(("http://", "https://")):
        addr = row["address"].strip()
        url = addr if addr.startswith(("http://", "https://")) else "https://" + addr
    return url


def write_contact_sheet(rows, folder, label):
    """index.html with every screenshot, its numbers and a link to the real site.

    Click a screenshot (or "full size") to open the PNG on its own in a new
    tab; click the site address to open the live page in a normal browser.
    """
    import html as htmlmod
    esc = htmlmod.escape
    cards = []
    for i, r in enumerate(rows, 1):
        img = r.get("screenshot", "") or ""
        if not img:                               # older CSV: look for NN-name.png
            guess = [f for f in sorted(os.listdir(folder))
                     if f.endswith(".png") and f[3:-4] == re.sub(r"[^A-Za-z0-9]+", "-", r["name"]).strip("-").lower()]
            img = guess[0] if guess else ""
        have = img and os.path.exists(os.path.join(folder, img))
        url = site_link(r)
        if have:
            pic = (f'<a class="shot" href="{esc(img)}" target="_blank" rel="noopener" title="Open the screenshot on its own">'
                   f'<img src="{esc(img)}" alt="{esc(r["name"])} on the badge" loading="lazy"></a>')
            full = f'<a href="{esc(img)}" target="_blank" rel="noopener">screenshot full size</a>'
        else:
            pic, full = '<div class="none">no screenshot</div>', '<span class="dim">no screenshot</span>'
        moved = ""
        if r.get("final_url") and r["final_url"].rstrip("/") != ("https://" + r["address"]).rstrip("/"):
            moved = f'<div class="dim small">ended at {esc(r["final_url"])}</div>'
        facts = (f'status {esc(r["status"] or "?")} &middot; {esc(r["text"] or "0")} characters &middot; '
                 f'{esc(r["links"] or "0")} links{" &middot; cut off" if r["cut"] == "yes" else ""}'
                 f'{(" &middot; <b class=flag>" + esc(r["flags"]) + "</b>") if r["flags"] else ""}')
        cards.append(
            f'<figure id="s{i}"><figcaption class="top"><b>{i}. {esc(r["name"])}</b>'
            f'<a class="site" href="{esc(url)}" target="_blank" rel="noopener">{esc(r["address"])} &#8599;</a></figcaption>'
            f'{pic}<figcaption><div>{facts}</div>{moved}'
            f'<div class="links">{full} &middot; <a href="{esc(url)}" target="_blank" rel="noopener">open the site</a></div>'
            f'</figcaption></figure>')
    toc = " ".join(f'<a href="#s{i}">{i}. {esc(r["name"])}</a>' for i, r in enumerate(rows, 1))
    page = f"""<!doctype html><html><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>Mini Browser {esc(label)}</title>
<style>
body{{font-family:system-ui,sans-serif;background:#111;color:#ddd;margin:16px}}
h1{{font-size:20px;margin:0 0 6px}} p{{color:#999;font-size:13px;margin:0 0 10px}}
a{{color:#7cb7ff}} a:visited{{color:#b59cff}}
nav{{font-size:13px;line-height:1.9;margin-bottom:14px}} nav a{{margin-right:10px;white-space:nowrap}}
.grid{{display:grid;grid-template-columns:repeat(auto-fill,minmax(320px,1fr));gap:16px}}
figure{{margin:0;background:#1c1c1c;padding:8px;border-radius:6px;display:flex;flex-direction:column}}
figcaption{{font-size:13px;margin-top:6px}} figcaption.top{{margin:0 0 6px;display:flex;justify-content:space-between;gap:8px}}
.site{{text-decoration:none;overflow:hidden;text-overflow:ellipsis;white-space:nowrap}}
a.shot{{display:block;max-height:480px;overflow:hidden;border:1px solid #333;cursor:zoom-in}}
a.shot:hover{{border-color:#7cb7ff}}
img{{width:100%;display:block;image-rendering:pixelated}}
.links{{margin-top:4px}} .dim{{color:#777}} .small{{font-size:12px;word-break:break-all}} .flag{{color:#e8a33c}}
.none{{height:300px;display:flex;align-items:center;justify-content:center;color:#666;border:1px dashed #444}}
</style></head><body>
<h1>Mini Browser site survey &ndash; {esc(label)}</h1>
<p>Click a screenshot to open it full size in a new tab. Click the address (&#8599;) to open the real site in this browser and compare.</p>
<nav>{toc}</nav>
<div class="grid">{''.join(cards)}</div></body></html>"""
    with open(os.path.join(folder, "index.html"), "w", encoding="utf-8") as f:
        f.write(page)


def text_of(content):
    """Readable characters in a CONTENT block (no spaces, no [n] numbers)."""
    joined = " ".join(content)
    joined = re.sub(r"\[\d+\]", "", joined)
    return sum(1 for ch in joined if not ch.isspace())


def restart_browser(badge, note="(Mini Browser crashed and was started again) "):
    """After a crash: Enter in the BadgeVMS menu starts Mini Browser again."""
    badge.settle(3.0)
    for attempt in range(3):
        badge.clear_log()
        badge.enter()
        try:
            badge.wait_for(r"\[mini_browser\] enter main", 20)
            badge.wait_for(r"^--- CONTENT END ---$", 60)   # its home page
            badge.settle(2.0)
            sys.__stdout__.write(note)
            return True
        except TimeoutError:
            continue
    sys.__stdout__.write("(Mini Browser crashed; start it again on the badge) ")
    return False


def ensure_browser(badge):
    """Make sure Mini Browser is running.  A badge that was just booted sits
    in the BadgeVMS menu: Enter starts the selected app (Mini Browser).

    1. Listen for 4 s: the menu prints "... running processes 1", a
       running app "... running processes 2" or Mini Browser's own lines.
    2. Still unknown: WHY+O (options) - Mini Browser answers "options
       opened" and Esc closes it again.
    3. No answer: press Enter and wait for Mini Browser's home page."""
    badge.clear_log()
    state = None
    deadline = time.monotonic() + 4.0
    while state is None and time.monotonic() < deadline:
        with badge.lock:
            lines = list(badge.lines)
        for line in lines:
            m = re.search(r"running processes (\d+)", line)
            if m:
                state = "menu" if m.group(1) == "1" else "app"
            if "[mini_browser]" in line:
                state = "app"
        time.sleep(0.2)
    if state is None:
        badge.clear_log()
        badge.why("O")
        try:
            badge.wait_for(r"\[mini_browser\] options opened", 5)
            state = "app"
            badge.press(0x29)                  # Esc: close the options again
            badge.settle(1.0)
        except TimeoutError:
            state = "menu"
    if state == "app":
        return True
    sys.__stdout__.write("Badge is in the BadgeVMS menu: starting Mini Browser (Enter) ... ")
    sys.__stdout__.flush()
    ok = restart_browser(badge, note="started\n")
    if not ok:
        sys.__stdout__.write("\n")
    return ok


def set_look(badge, want):
    """4.5-dev2: WHY+N switches the page look (badge <-> web).  Press it until
    Mini Browser reports "page look: switched to <want>".  Mini Browser must
    be running (not still starting) - the browser's own start line
    "page look: badge" is not taken as an answer."""
    for attempt in range(4):
        badge.clear_log()
        badge.why("N")
        try:
            line = badge.wait_for(r"\[mini_browser\] page look: switched to (web|badge)", 8)
        except TimeoutError:
            continue                    # key not seen (browser busy or starting): again
        if line.rstrip().endswith(want):
            badge.settle(1.0)
            return True
    return False


def survey_site(reg, badge, group, name, address):
    row = {"group": group, "name": name, "address": address, "status": "", "final_url": "",
           "bytes": "", "received": "", "cut": "", "links": "", "text": "", "flags": "",
           "seconds": "", "screenshot": ""}
    started = time.monotonic()
    reg.v41_type_in_omnibox(badge, address, "L")
    badge.clear_log()
    badge.enter()
    try:
        line = badge.wait_for(
            r"\[mini_browser\] (HTTP \d+, \d+ bytes|HTTP \d+ URL=|fetch error \d+|download: offered)"
            r"|caused an unhandled exception",
            120)
    except TimeoutError:
        row["status"] = "timeout"
        row["seconds"] = f"{time.monotonic() - started:.0f}"
        badge.press(0x29)                      # Esc: stop the load
        badge.settle(2.0)
        return row

    if "unhandled exception" in line:
        # Mini Browser crashed: BadgeVMS is back in its menu.  Note it, start
        # Mini Browser again (Enter in the menu) and go on with the next site.
        row["status"] = "CRASH"
        row["flags"] = "crash"
        row["seconds"] = f"{time.monotonic() - started:.0f}"
        restart_browser(badge)
        return row

    m = re.search(r"HTTP (\d+), (\d+) bytes, (\d+) links from (\S+)", line)
    if m:
        row["status"] = m.group(1)
        row["bytes"] = m.group(2)
        row["links"] = m.group(3)
        row["final_url"] = m.group(4)
    else:
        m = re.search(r"fetch error (\d+)|HTTP (\d+) URL=", line)
        row["status"] = (f"curl {m.group(1)}" if m and m.group(1) else
                         m.group(2) if m else "download")

    content = []
    try:
        line = badge.wait_for(r"^--- CONTENT END ---$|caused an unhandled exception", 60)
        if "caused an unhandled exception" in line:
            row["status"] = "CRASH"
            row["flags"] = "crash"
            row["seconds"] = f"{time.monotonic() - started:.0f}"
            restart_browser(badge)
            return row
        content = reg.latest_content_block(badge)
    except TimeoutError:
        pass
    badge.settle(2.0)                          # 4.5 prints its page stats after the text

    stats = None
    for l in badge.get_lines():
        s = re.search(r"page stats: received=(\d+) kept=(\d+) .*cut=(yes|no)", l)
        if s:
            stats = s
    if stats:
        row["received"] = stats.group(1)
        row["cut"] = stats.group(3)
    elif row["bytes"]:
        row["received"] = ""
        row["cut"] = "yes" if int(row["bytes"]) >= 65536 else "no"

    row["text"] = str(text_of(content)) if content else "0"
    low = "\n".join(content).lower()
    flags = []
    head = "\n".join(content[:60]).lower()
    if any(w in head for w in COOKIE_WORDS):
        flags.append("cookie")
    if any(w in low for w in JS_WORDS) or (row["status"] == "200" and int(row["text"]) < 400):
        flags.append("js")
    row["flags"] = " ".join(flags)
    row["seconds"] = f"{time.monotonic() - started:.0f}"
    return row


def keep_previous_run(label):
    """An earlier run with this label (its .csv, .log and screenshot folder)
    is renamed to site-survey-<label>-old-<date>-<time>, not overwritten."""
    base = os.path.join(HERE, f"site-survey-{label}")
    parts = [p for p in (base + ".csv", base + ".log", base) if os.path.exists(p)]
    if not parts:
        return
    stamp = time.strftime("%Y%m%d-%H%M", time.localtime(max(os.path.getmtime(p) for p in parts)))
    old = f"{base}-old-{stamp}"
    n = 2
    while any(os.path.exists(old + ext) for ext in (".csv", ".log", "")):
        old = f"{base}-old-{stamp}-{n}"
        n += 1
    for p in parts:
        os.rename(p, old + p[len(base):])
    sys.__stdout__.write(f"Earlier run kept as {os.path.basename(old)} (.csv, .log, screenshots)\n")


def report_crashes(crashes, log_path, label):
    """Where did Mini Browser crash?  badge_crash.py (same folder) turns the
    crash report in the log into function names; it needs the ELF files of
    the firmware build and riscv32-esp-elf-addr2line (. ~/esp-idf/export.sh)."""
    out = sys.__stdout__
    out.write(f"\nMini Browser crashed {len(crashes)} time(s): {', '.join(crashes)} "
              f"(started again each time)\n")
    tool = os.path.join(HERE, "badge_crash.py")
    report = os.path.join(HERE, f"site-survey-{label}-crashes.txt")
    try:
        r = subprocess.run([sys.executable, tool, log_path], capture_output=True, text=True, timeout=120)
        text = (r.stdout + r.stderr).strip()
    except (OSError, subprocess.SubprocessError) as exc:
        text = f"badge_crash.py could not run: {exc}"
    with open(report, "w", encoding="utf-8") as f:
        f.write(f"Crashes at: {', '.join(crashes)}\n\n{text}\n")
    out.write(f"Crash details: {report}\n")
    out.write("  (if it says addr2line was not found: . ~/esp-idf/export.sh and run\n"
              f"   ./badge_crash.py {os.path.basename(log_path)})\n")


def print_table(rows, out=sys.__stdout__):
    out.write("\n{:<5} {:<15} {:>7} {:>7} {:>4} {:>5} {:>6}  {}\n".format(
        "group", "site", "status", "bytes", "cut", "links", "text", "flags"))
    out.write("-" * 66 + "\n")
    for r in rows:
        out.write("{:<5} {:<15} {:>7} {:>7} {:>4} {:>5} {:>6}  {}\n".format(
            r["group"], r["name"][:15], r["status"], r["bytes"], r["cut"], r["links"],
            r["text"], r["flags"]))
    ok = sum(1 for r in rows if r["status"] == "200" and "js" not in r["flags"]
             and int(r["text"] or 0) >= 400)
    cut = sum(1 for r in rows if r["cut"] == "yes")
    out.write("-" * 66 + "\n")
    out.write(f"Readable: {ok} of {len(rows)} sites   Cut off at the size limit: {cut}\n")


def compare(old_path, new_path):
    def read(p):
        with open(p, newline="") as f:
            return {r["name"]: r for r in csv.DictReader(f)}
    old, new = read(old_path), read(new_path)
    print(f"\n{'site':<15} {'text before':>11} {'text after':>10} {'cut':>9}  flags after")
    print("-" * 66)
    for name, n in new.items():
        o = old.get(name, {})
        print(f"{name[:15]:<15} {o.get('text', ''):>11} {n['text']:>10} "
              f"{o.get('cut', '?') + '->' + n['cut']:>9}  {n['flags']}")


def main():
    parser = argparse.ArgumentParser(description="Mini Browser site survey (20 news and tech sites)")
    parser.add_argument("--label", default="run", help="name for the result files, e.g. 4.4 or 4.5-dev1")
    parser.add_argument("--only", help="comma-separated site names or numbers (1-20) to run")
    parser.add_argument("--compare", nargs=2, metavar=("OLD.csv", "NEW.csv"),
                        help="compare two earlier runs and exit")
    parser.add_argument("--list", action="store_true", help="list the sites and exit")
    parser.add_argument("--view", type=float, default=0,
                        help="seconds to keep every site on screen (to watch the badge)")
    parser.add_argument("--screenshots", action="store_true",
                        help="take a WHY+S screenshot of every site into site-survey-<label>/")
    parser.add_argument("--full-page", action="store_true",
                        help="with --screenshots: whole-page WHY+Z screenshots (slow on long pages)")
    parser.add_argument("--look", choices=("web", "badge"),
                        help="4.5-dev2+: page look for the run: web (light, like a normal "
                             "browser) or badge (dark).  Stays set on the badge afterwards")
    parser.add_argument("--overwrite", action="store_true",
                        help="replace an earlier run with the same label (default: the earlier "
                             "run is kept as site-survey-LABEL-old-DATE-TIME)")
    parser.add_argument("--sheet", metavar="LABEL",
                        help="rebuild site-survey-LABEL/index.html from site-survey-LABEL.csv (no badge needed)")
    args = parser.parse_args()

    if args.sheet:
        folder = os.path.join(HERE, f"site-survey-{args.sheet}")
        with open(os.path.join(HERE, f"site-survey-{args.sheet}.csv"), newline="") as f:
            rows = list(csv.DictReader(f))
        write_contact_sheet(rows, folder, args.sheet)
        print(f"Screenshots: {os.path.join(folder, 'index.html')}")
        return 0

    if args.list:
        for i, (g, n, a) in enumerate(SITES, 1):
            print(f"{i:2}. {g:<4} {n:<15} {a}")
        return 0
    if args.compare:
        compare(*args.compare)
        return 0

    sites = SITES
    if args.only:
        wanted = [w.strip().lower() for w in args.only.split(",")]
        sites = [s for i, s in enumerate(SITES, 1)
                 if str(i) in wanted or s[1].lower() in wanted]

    log_path = os.path.join(HERE, f"site-survey-{args.label}.log")
    csv_path = os.path.join(HERE, f"site-survey-{args.label}.csv")
    if not args.overwrite:
        keep_previous_run(args.label)
    real_stdout = sys.stdout
    reg = load_regression()
    reg.VIEW_DELAY = 0
    shot = None
    shot_dir = os.path.join(HERE, f"site-survey-{args.label}")
    if args.screenshots:
        shot = load_screenshot_module()
        os.makedirs(shot_dir, exist_ok=True)
    log = open(log_path, "w", encoding="utf-8", errors="replace")
    sys.stdout = log                       # the badge's serial output goes to the log
    badge = reg.Badge()
    rows = []
    crashes = []
    try:
        badge.settle(1.0)
        if not ensure_browser(badge):
            real_stdout.write("Mini Browser did not start. Start it on the badge and run again. Stopped.\n")
            return 1
        if args.look:
            if not set_look(badge, args.look):
                real_stdout.write(f"Page look: could not switch to {args.look}. Is Mini Browser "
                                  f"4.5-dev2 (or later) running and showing a page? Stopped.\n")
                return 1
            real_stdout.write(f"Page look: {args.look}\n")
            real_stdout.flush()
        for i, (group, name, address) in enumerate(sites, 1):
            real_stdout.write(f"[{i}/{len(sites)}] {name} ({address}) ... ")
            real_stdout.flush()
            row = survey_site(reg, badge, group, name, address)
            rows.append(row)
            if row["status"] == "CRASH":
                crashes.append(name)
            if args.view > 0:
                real_stdout.write(f"(on screen for {args.view:g} s) ")
                real_stdout.flush()
                time.sleep(args.view)
            if shot is not None and row["status"] not in ("timeout",):
                fname = f"{i:02d}-{re.sub(r'[^A-Za-z0-9]+', '-', name).strip('-').lower()}.png"
                badge.settle(1.5)              # let images and the bar settle
                result = capture_screenshot(badge, shot, os.path.join(shot_dir, fname), args.full_page)
                if result.startswith("damaged") or result == "no screenshot received":
                    # Too many lines lost on the serial port: try once more.
                    real_stdout.write("\r" + " " * 100 + f"\r[{i}/{len(sites)}] {name} ... ({result}, again) ")
                    real_stdout.flush()
                    badge.settle(2.0)
                    result = capture_screenshot(badge, shot, os.path.join(shot_dir, fname), args.full_page)
                if result.startswith("ok"):
                    row["screenshot"] = fname
                if result.startswith("CRASH"):
                    # Mini Browser crashed while making the screenshot: note
                    # it, start it again from the BadgeVMS menu, go on.
                    row["flags"] = (row["flags"] + " crash").strip()
                    crashes.append(name)
                    real_stdout.write("\r" + " " * 100 + "\r")
                    restart_browser(badge)
                real_stdout.write("\r" + " " * 100 + f"\r[{i}/{len(sites)}] {name} ({address}) ... ")
                real_stdout.write(f"[screenshot {result}] ")
                real_stdout.flush()
            real_stdout.write(f"{row['status']}, {row['text']} characters"
                              f"{', cut' if row['cut'] == 'yes' else ''}"
                              f"{', ' + row['flags'] if row['flags'] else ''}\n")
            real_stdout.flush()
    finally:
        badge.close()
        sys.stdout = real_stdout
        log.close()

    with open(csv_path, "w", newline="") as f:
        w = csv.DictWriter(f, fieldnames=FIELDS)
        w.writeheader()
        w.writerows(rows)
    print_table(rows, real_stdout)
    print(f"\nResults: {csv_path}\nSerial log: {log_path}")
    if crashes:
        report_crashes(crashes, log_path, args.label)
    if shot is not None:
        write_contact_sheet(rows, shot_dir, args.label)
        print(f"Screenshots: {os.path.join(shot_dir, 'index.html')}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
