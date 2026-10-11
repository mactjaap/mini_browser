#!/usr/bin/env python3
"""
Where did a BadgeVMS app crash?

Reads a serial log (for example site-survey-<label>.log, or a copy of the
idf.py monitor output) and, for every

    Task N caused an unhandled exception ...
    Task N: mcause 0x... mtval 0x... mepc 0x... ra 0x... sp 0x...

turns mepc (where it crashed) and ra (who called it) into function names and
source lines with addr2line, using the app's ELF from the firmware build.

The app is loaded at a different address each start; the last line
"Start ELF file entrypoint at 0x..." before the crash gives the offset.

    ./badge_crash.py site-survey-4.5-dev1-look.log
    ./badge_crash.py log.txt --elf ~/WHY2025/firmware/build/app_elfs/mini_browser.elf
"""

import argparse
import glob
import os
import re
import shutil
import struct
import subprocess
import sys

MCAUSE = {
    0: "instruction address misaligned", 1: "instruction access fault", 2: "illegal instruction",
    3: "breakpoint", 4: "load address misaligned", 5: "load access fault",
    6: "store address misaligned", 7: "store access fault", 8: "ecall (user)",
    11: "ecall (machine)", 12: "instruction page fault", 13: "load page fault", 15: "store page fault",
}


def elf_entry(path):
    with open(path, "rb") as f:
        head = f.read(64)
    if head[:4] != b"\x7fELF" or head[4] != 1:
        raise SystemExit(f"{path}: not a 32-bit ELF file")
    return struct.unpack_from("<I", head, 24)[0]


def find_addr2line():
    tool = shutil.which("riscv32-esp-elf-addr2line")
    if tool:
        return tool
    for base in ("~/.espressif", "~/WHY2025/root/.espressif"):
        hits = glob.glob(os.path.expanduser(base) + "/tools/riscv32-esp-elf/*/riscv32-esp-elf/bin/riscv32-esp-elf-addr2line")
        if hits:
            return sorted(hits)[-1]
    raise SystemExit("riscv32-esp-elf-addr2line not found: run '. ~/esp-idf/export.sh' first")


def is_firmware(addr):
    return 0x40000000 <= addr < 0x44000000 or 0x4FF00000 <= addr < 0x4FFC0000


def is_app(addr):
    return 0x48000000 <= addr < 0x4C000000


def lookup(tool, elf, addr):
    out = subprocess.run([tool, "-f", "-C", "-p", "-e", elf, hex(addr)],
                         capture_output=True, text=True).stdout.strip()
    return out


def main():
    here = os.path.dirname(os.path.abspath(__file__))
    default_elf = os.path.join(here, "..", "firmware", "build", "app_elfs", "mini_browser.elf")
    default_fw = os.path.join(here, "..", "firmware", "build", "badgevms.elf")
    p = argparse.ArgumentParser(description="Turn a BadgeVMS app crash in a serial log into source lines")
    p.add_argument("log")
    p.add_argument("--elf", default=default_elf, help="the app's ELF (default: firmware/build/app_elfs/mini_browser.elf)")
    p.add_argument("--firmware-elf", default=default_fw, help="the firmware ELF (default: firmware/build/badgevms.elf)")
    args = p.parse_args()

    entry = elf_entry(args.elf)
    tool = find_addr2line()
    have_fw = os.path.exists(args.firmware_elf)
    loaded = None
    found = 0
    offset = None

    def where(addr, back=0):
        """Function and line of addr: firmware or app code.  For a return
        address (ra, stack) back=2 looks up the call instruction before it."""
        addr -= back
        if is_firmware(addr):
            return ("firmware", lookup(tool, args.firmware_elf, addr)) if have_fw else ("firmware", "(no badgevms.elf)")
        if is_app(addr) and offset is not None:
            return ("app", lookup(tool, args.elf, addr - offset))
        return (None, None)

    with open(args.log, errors="replace") as f:
        for line in f:
            m = re.search(r"Start ELF file entrypoint at (0x[0-9a-fA-F]+)", line)
            if m:
                loaded = int(m.group(1), 16)
                continue
            m = re.search(r"Task (\d+): mcause (0x[0-9a-f]+) mtval (0x[0-9a-f]+) mepc (0x[0-9a-f]+) "
                          r"ra (0x[0-9a-f]+) sp (0x[0-9a-f]+)", line)
            if m:
                found += 1
                cause = int(m.group(2), 16) & 0x7FFFFFFF
                mtval, mepc, ra = (int(m.group(i), 16) for i in (3, 4, 5))
                print(f"\nCrash {found}: {MCAUSE.get(cause, 'cause %d' % cause)}, address 0x{mtval:08x}")
                offset = loaded - entry if loaded is not None else None
                for label, addr, back in (("crashed in", mepc, 0), ("called from", ra, 2)):
                    kind, text = where(addr, back)
                    if kind:
                        print(f"  {label:<11} [{kind}] {text}")
                    else:
                        print(f"  {label:<11} 0x{addr:08x} (no 'Start ELF file entrypoint' line yet: cannot map)")
                continue
            m = re.search(r"Task \d+: ELF entry (0x[0-9a-f]+)", line)
            if m:                                   # 4.5-dev2 firmware: exact load address
                offset = int(m.group(1), 16) - entry
                print(f"  (app loaded at 0x{int(m.group(1), 16):08x})")
                continue
            m = re.search(r"Task \d+: text at a0: (.*)$", line)
            if m:
                print(f"  message: {m.group(1).strip()}")
                continue
            m = re.search(r"Task \d+: stack((?: 0x[0-9a-f]+)+)", line)
            if m:
                for word in m.group(1).split():
                    kind, text = where(int(word, 16), 2)
                    if kind and text and not text.startswith("??"):
                        print(f"  stack       [{kind}] {text}")
    if not found:
        print("No 'Task N: mcause ...' line in the log (needs the firmware with the crash report).")


if __name__ == "__main__":
    main()
