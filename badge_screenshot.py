#!/usr/bin/env python3

import argparse
import base64
import re
import struct
import sys
import time
import zlib
from datetime import datetime
from pathlib import Path

try:
    import serial
except ImportError:
    print(
        "ERROR: pyserial is required. Install it with: python3 -m pip install pyserial",
        file=sys.stderr,
    )
    raise SystemExit(2)


DEFAULT_DEVICE = "/dev/cu.wchusbserial10"
DEFAULT_BAUD = 115200
DEFAULT_TIMEOUT = 150.0

BEGIN_RE = re.compile(
    r"IMG BEGIN (\d+) (\d+) RGB24 RLE5FEC1 (\d+) (\d+)"
)
END_RE = re.compile(
    r"IMG END (\d+) ([0-9A-Fa-f]{8}) (\d+)"
)
DATA_RE = re.compile(
    r"IMG D (\d{6}) (\d{2}) ([0-9A-Fa-f]{8}) ([A-Za-z0-9+/=]+)"
)
PARITY_RE = re.compile(
    r"IMG P (\d{6}) ([0-9A-Fa-f]{8}) ([A-Za-z0-9+/=]+)"
)


def png_chunk(chunk_type, payload):
    crc = zlib.crc32(chunk_type)
    crc = zlib.crc32(payload, crc) & 0xFFFFFFFF

    return (
        struct.pack(">I", len(payload))
        + chunk_type
        + payload
        + struct.pack(">I", crc)
    )


def write_png_rgb(path, width, height, rgb):
    expected = width * height * 3

    if len(rgb) != expected:
        raise ValueError(
            f"RGB byte count mismatch: got {len(rgb)}, expected {expected}"
        )

    rows = bytearray()
    stride = width * 3

    for y in range(height):
        rows.append(0)
        start = y * stride
        rows.extend(rgb[start:start + stride])

    png = bytearray(b"\x89PNG\r\n\x1a\n")
    png.extend(
        png_chunk(
            b"IHDR",
            struct.pack(">IIBBBBB", width, height, 8, 2, 0, 0, 0),
        )
    )
    png.extend(
        png_chunk(
            b"IDAT",
            zlib.compress(bytes(rows), 9),
        )
    )
    png.extend(png_chunk(b"IEND", b""))

    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_bytes(png)


def decode_rle5(data, width, height):
    if len(data) % 5 != 0:
        raise ValueError(
            f"RLE data length is not a multiple of 5: {len(data)}"
        )

    expected_pixels = width * height
    produced_pixels = 0
    rgb = bytearray()

    for offset in range(0, len(data), 5):
        count = data[offset] | (data[offset + 1] << 8)
        r = data[offset + 2]
        g = data[offset + 3]
        b = data[offset + 4]

        if count <= 0:
            raise ValueError("RLE stream contains a zero-length run")

        produced_pixels += count

        if produced_pixels > expected_pixels:
            raise ValueError(
                "RLE stream expands beyond screenshot dimensions"
            )

        rgb.extend(bytes((r, g, b)) * count)

    if produced_pixels != expected_pixels:
        raise ValueError(
            f"RLE pixel count mismatch: got {produced_pixels}, "
            f"expected {expected_pixels}"
        )

    return bytes(rgb)


def default_output(full_page=False):
    timestamp = datetime.now().strftime("%Y%m%d-%H%M%S")
    prefix = "minibrowser-full" if full_page else "minibrowser"
    return Path(f"{prefix}-{timestamp}.png")


def request_screenshot(port, full_page=False):
    # WHY key down
    port.write(b"E E3 1 00\n")
    port.flush()
    time.sleep(0.03)

    if full_page:
        # Z down/up (USB HID scancode 0x1D) -> WHY+Z
        port.write(b"E 1D 1 7A\n")
        port.flush()
        time.sleep(0.03)

        port.write(b"E 1D 0 00\n")
        port.flush()
        time.sleep(0.03)
    else:
        # S down/up (USB HID scancode 0x16) -> WHY+S
        port.write(b"E 16 1 73\n")
        port.flush()
        time.sleep(0.03)

        port.write(b"E 16 0 00\n")
        port.flush()
        time.sleep(0.03)

    # WHY key up
    port.write(b"E E3 0 00\n")
    port.flush()


def receive_screenshot(port, output_path, timeout, verbose=False):
    deadline = time.monotonic() + timeout
    buffer = bytearray()

    begin_match = None
    end_match = None

    data_chunks = {}
    parity_chunks = {}
    damaged_records = 0
    duplicate_records = 0

    print("Waiting for Mini Browser screenshot...")

    while time.monotonic() < deadline:
        data = port.read(1024)

        if not data:
            continue

        buffer.extend(data)

        while b"\n" in buffer:
            raw_line, _, buffer = buffer.partition(b"\n")
            line = raw_line.decode(
                "utf-8",
                errors="replace",
            ).rstrip("\r")
            stripped = line.strip()

            # Treat --timeout as an inactivity timeout once an image transfer
            # has started. Full-page WHY+Z screenshots can legitimately take
            # much longer than a 716x716 viewport screenshot.
            if begin_match is not None and stripped.startswith("IMG "):
                deadline = time.monotonic() + timeout

            if begin_match is None:
                match = BEGIN_RE.fullmatch(stripped)

                if match:
                    begin_match = match
                    width = int(match.group(1))
                    height = int(match.group(2))
                    chunk_size = int(match.group(3))
                    group_size = int(match.group(4))

                    data_chunks.clear()
                    parity_chunks.clear()
                    damaged_records = 0
                    duplicate_records = 0

                    print(
                        f"Receiving {width}x{height} screenshot "
                        f"(chunk {chunk_size}, FEC group {group_size})..."
                    )
                    deadline = time.monotonic() + timeout
                    continue

                if verbose and stripped:
                    print(line)

                continue

            if stripped.startswith("IMG ERROR "):
                raise RuntimeError(
                    f"Badge screenshot failed: {stripped}"
                )

            match = DATA_RE.fullmatch(stripped)

            if match:
                sequence = int(match.group(1))
                declared_length = int(match.group(2))
                expected_crc = int(match.group(3), 16)

                try:
                    chunk = base64.b64decode(
                        match.group(4),
                        validate=True,
                    )
                except Exception:
                    damaged_records += 1
                    continue

                if (
                    declared_length <= 0
                    or declared_length > chunk_size
                    or len(chunk) != declared_length
                ):
                    damaged_records += 1
                    continue

                actual_crc = zlib.crc32(chunk) & 0xFFFFFFFF

                if actual_crc != expected_crc:
                    damaged_records += 1
                    continue

                if sequence in data_chunks:
                    duplicate_records += 1
                else:
                    data_chunks[sequence] = chunk

                continue

            match = PARITY_RE.fullmatch(stripped)

            if match:
                group = int(match.group(1))
                expected_crc = int(match.group(2), 16)

                try:
                    parity = base64.b64decode(
                        match.group(3),
                        validate=True,
                    )
                except Exception:
                    damaged_records += 1
                    continue

                if len(parity) != chunk_size:
                    damaged_records += 1
                    continue

                actual_crc = zlib.crc32(parity) & 0xFFFFFFFF

                if actual_crc != expected_crc:
                    damaged_records += 1
                    continue

                if group in parity_chunks:
                    duplicate_records += 1
                else:
                    parity_chunks[group] = parity

                continue

            match = END_RE.fullmatch(stripped)

            if match:
                end_match = match
                break

            if verbose and stripped:
                print(line)

        if end_match is not None:
            break

    if begin_match is None:
        raise TimeoutError(
            "Timed out waiting for IMG BEGIN. "
            "Press WHY+S or WHY+Z while Mini Browser is running."
        )

    if end_match is None:
        raise TimeoutError(
            "Timed out waiting for complete IMG screenshot transfer"
        )

    width = int(begin_match.group(1))
    height = int(begin_match.group(2))
    chunk_size = int(begin_match.group(3))
    group_size = int(begin_match.group(4))

    declared_size = int(end_match.group(1))
    declared_crc = int(end_match.group(2), 16)
    declared_chunks = int(end_match.group(3))

    if chunk_size <= 0 or group_size <= 0:
        raise ValueError(
            "Invalid screenshot FEC parameters from badge"
        )

    repaired_chunks = 0
    total_groups = (
        declared_chunks + group_size - 1
    ) // group_size

    for group in range(total_groups):
        first_sequence = group * group_size
        last_sequence = min(
            first_sequence + group_size,
            declared_chunks,
        )

        group_sequences = list(
            range(first_sequence, last_sequence)
        )

        missing = [
            sequence
            for sequence in group_sequences
            if sequence not in data_chunks
        ]

        if not missing:
            continue

        if len(missing) != 1 or group not in parity_chunks:
            raise ValueError(
                f"FEC could not repair group {group}: "
                f"missing chunks {missing}, "
                f"parity={'yes' if group in parity_chunks else 'no'}"
            )

        missing_sequence = missing[0]
        recovered = bytearray(parity_chunks[group])

        for sequence in group_sequences:
            if sequence == missing_sequence:
                continue

            chunk = data_chunks[sequence]

            for index, value in enumerate(chunk):
                recovered[index] ^= value

        if missing_sequence == declared_chunks - 1:
            missing_length = (
                declared_size
                - chunk_size * (declared_chunks - 1)
            )
        else:
            missing_length = chunk_size

        if missing_length <= 0 or missing_length > chunk_size:
            raise ValueError(
                f"FEC calculated invalid recovered chunk length "
                f"{missing_length} for sequence {missing_sequence}"
            )

        data_chunks[missing_sequence] = bytes(
            recovered[:missing_length]
        )
        repaired_chunks += 1

    still_missing = [
        sequence
        for sequence in range(declared_chunks)
        if sequence not in data_chunks
    ]

    if still_missing:
        raise ValueError(
            f"Screenshot still has missing chunks: "
            f"{still_missing[:12]}"
        )

    compressed = b"".join(
        data_chunks[sequence]
        for sequence in range(declared_chunks)
    )

    if len(compressed) != declared_size:
        raise ValueError(
            f"Screenshot compressed-size mismatch: "
            f"got {len(compressed)}, expected {declared_size}"
        )

    actual_crc = zlib.crc32(compressed) & 0xFFFFFFFF

    if actual_crc != declared_crc:
        raise ValueError(
            f"Screenshot CRC32 mismatch after FEC: "
            f"got {actual_crc:08X}, expected {declared_crc:08X}"
        )

    rgb = decode_rle5(
        compressed,
        width,
        height,
    )

    write_png_rgb(
        output_path,
        width,
        height,
        rgb,
    )

    return {
        "path": str(output_path),
        "width": width,
        "height": height,
        "compressed_bytes": len(compressed),
        "crc32": f"{actual_crc:08X}",
        "fec_repaired_chunks": repaired_chunks,
        "damaged_records": damaged_records,
        "duplicate_records": duplicate_records,
    }


def main():
    parser = argparse.ArgumentParser(
        description=(
            "Receive Mini Browser WHY+S viewport or WHY+Z full-page "
            "screenshots from a WHY2025 badge over USB serial and save "
            "them as PNG files."
        )
    )

    parser.add_argument(
        "device",
        nargs="?",
        default=DEFAULT_DEVICE,
        help=(
            f"serial device "
            f"(default: {DEFAULT_DEVICE})"
        ),
    )

    parser.add_argument(
        "output",
        nargs="?",
        help=(
            "PNG output filename "
            "(default: minibrowser-YYYYMMDD-HHMMSS.png)"
        ),
    )

    parser.add_argument(
        "--baud",
        type=int,
        default=DEFAULT_BAUD,
        help=f"serial baud rate (default: {DEFAULT_BAUD})",
    )

    parser.add_argument(
        "--timeout",
        type=float,
        default=DEFAULT_TIMEOUT,
        help=(
            f"inactivity timeout in seconds "
            f"(default: {DEFAULT_TIMEOUT:g})"
        ),
    )

    parser.add_argument(
        "--request",
        action="store_true",
        help=(
            "send the selected WHY shortcut to the badge instead of "
            "waiting for the physical keyboard shortcut; requires the "
            "custom serial-keyboard firmware"
        ),
    )

    parser.add_argument(
        "--full-page",
        action="store_true",
        help=(
            "receive/request a full rendered page with WHY+Z instead of "
            "the normal 716x716 WHY+S viewport screenshot"
        ),
    )

    parser.add_argument(
        "-v",
        "--verbose",
        action="store_true",
        help="show unrelated firmware serial output",
    )

    args = parser.parse_args()

    output_path = (
        Path(args.output)
        if args.output
        else default_output(args.full_page)
    )

    port = None

    try:
        port = serial.Serial(
            args.device,
            args.baud,
            timeout=0.1,
        )

        # Throw away stale serial text from before this receiver started.
        port.reset_input_buffer()

        print(f"Serial: {args.device} @ {args.baud}")

        shortcut = "WHY+Z" if args.full_page else "WHY+S"

        if args.request:
            print(f"Requesting screenshot with {shortcut}...")
            request_screenshot(port, full_page=args.full_page)
        else:
            print(f"Press {shortcut} on the badge.")

        result = receive_screenshot(
            port,
            output_path,
            args.timeout,
            args.verbose,
        )

        print()
        print(f"Saved: {result['path']}")
        print(
            f"Image: {result['width']}x{result['height']}"
        )
        print(
            f"Compressed: "
            f"{result['compressed_bytes']} RLE bytes"
        )
        print(
            f"CRC32: {result['crc32']} OK"
        )
        print(
            f"FEC repaired: "
            f"{result['fec_repaired_chunks']} chunk(s)"
        )
        print(
            f"Damaged records discarded: "
            f"{result['damaged_records']}"
        )

        if result["duplicate_records"]:
            print(
                f"Duplicate records ignored: "
                f"{result['duplicate_records']}"
            )

        return 0

    except KeyboardInterrupt:
        print("\nCancelled.", file=sys.stderr)
        return 130

    except Exception as exc:
        print(f"ERROR: {exc}", file=sys.stderr)
        return 1

    finally:
        if port is not None:
            port.close()


if __name__ == "__main__":
    raise SystemExit(main())
