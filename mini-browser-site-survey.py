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

It needs the custom firmware with the serial keyboard bridge (as the
regression suite) and reuses the regression suite's serial code.  All
serial output goes to site-survey-<label>.log, the results to
site-survey-<label>.csv and a table on the screen.
"""

import argparse
import csv
import importlib.util
import os
import re
import sys
import time

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
    ("news", "Al Jazeera", "aljazeera.com"),
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
          "cut", "links", "text", "flags", "seconds"]


def load_regression():
    path = os.path.join(HERE, "mini-browser-3.0-regression-selectable.py")
    spec = importlib.util.spec_from_file_location("mb_regression", path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def text_of(content):
    """Readable characters in a CONTENT block (no spaces, no [n] numbers)."""
    joined = " ".join(content)
    joined = re.sub(r"\[\d+\]", "", joined)
    return sum(1 for ch in joined if not ch.isspace())


def survey_site(reg, badge, group, name, address):
    row = {"group": group, "name": name, "address": address, "status": "", "final_url": "",
           "bytes": "", "received": "", "cut": "", "links": "", "text": "", "flags": "",
           "seconds": ""}
    started = time.monotonic()
    reg.v41_type_in_omnibox(badge, address, "L")
    badge.clear_log()
    badge.enter()
    try:
        line = badge.wait_for(
            r"\[mini_browser\] (HTTP \d+, \d+ bytes|HTTP \d+ URL=|fetch error \d+|download: offered)",
            120)
    except TimeoutError:
        row["status"] = "timeout"
        row["seconds"] = f"{time.monotonic() - started:.0f}"
        badge.press(0x29)                      # Esc: stop the load
        badge.settle(2.0)
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
        badge.wait_for(r"^--- CONTENT END ---$", 60)
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
    args = parser.parse_args()

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
    real_stdout = sys.stdout
    reg = load_regression()
    reg.VIEW_DELAY = 0
    log = open(log_path, "w", encoding="utf-8", errors="replace")
    sys.stdout = log                       # the badge's serial output goes to the log
    badge = reg.Badge()
    rows = []
    try:
        badge.settle(1.0)
        for i, (group, name, address) in enumerate(sites, 1):
            real_stdout.write(f"[{i}/{len(sites)}] {name} ({address}) ... ")
            real_stdout.flush()
            row = survey_site(reg, badge, group, name, address)
            rows.append(row)
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
    return 0


if __name__ == "__main__":
    sys.exit(main())
