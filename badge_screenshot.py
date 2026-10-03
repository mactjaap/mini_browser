#!/usr/bin/env python3

import argparse
import base64
import binascii
import itertools
import os
import re
import struct
import sys
import time
import zlib
from datetime import datetime
from pathlib import Path
from typing import Any, Dict, Iterator, List, NamedTuple, Optional, Tuple

try:
    import serial
    from serial.tools import list_ports
except ImportError:
    print(
        "ERROR: pyserial is required. Install it with: python3 -m pip install pyserial",
        file=sys.stderr,
    )
    raise SystemExit(2)


DEFAULT_DEVICE = None
DEFAULT_BAUD = 115200
DEFAULT_TIMEOUT = 150.0


def detect_badge_device(explicit=None):
    """Return the WHY2025 badge serial device, or fail clearly if ambiguous."""
    if explicit:
        return explicit

    env_device = os.environ.get("BADGE_PORT")
    if env_device:
        return env_device

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


# Records are matched with re.search and anchored at the end so console noise
# glued in front of a record does not discard it (data/parity CRCs still
# validate the payload).
BEGIN_RE = re.compile(
    r"IMG BEGIN (\d+) (\d+) RGB24 RLE5FEC1 (\d+) (\d+)$"
)
END_RE = re.compile(
    r"IMG END (\d+) ([0-9A-Fa-f]{8}) (\d+)$"
)
DATA_RE = re.compile(
    r"IMG D (\d{6,}) (\d{2}) ([0-9A-Fa-f]{8}) ([A-Za-z0-9+/=]+)$"
)
PARITY_RE = re.compile(
    r"IMG P (\d{6,}) ([0-9A-Fa-f]{8}) ([A-Za-z0-9+/=]+)$"
)
FAIL_RE = re.compile(
    r"IMG (ERROR|ABORT)\b(.*)$"
)

# Header sanity limits; implausible (corrupted) headers are ignored.
MAX_WIDTH = 720
MAX_HEIGHT = 200000
EXPECTED_CHUNK_SIZE = 48
MAX_GROUP_SIZE = 32

# A line without newline is cut to its last MAX_LINE_BYTES bytes.
MAX_LINE_BYTES = 4096

# After a failed END, how long to wait for the repeated END line.
SECOND_END_WAIT = 1.0

# Upper bound on candidate combinations tried for duplicated sequences.
MAX_CANDIDATE_ATTEMPTS = 256


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


def decode_rle5(data: bytes, width: int, height: int) -> bytes:
    if len(data) % 5 != 0:
        raise ValueError(
            f"RLE data length is not a multiple of 5: {len(data)}"
        )

    expected_pixels = width * height
    produced_pixels = 0
    rgb = bytearray()

    for count, pixel in struct.iter_unpack("<H3s", data):
        if count <= 0:
            raise ValueError("RLE stream contains a zero-length run")

        produced_pixels += count

        if produced_pixels > expected_pixels:
            raise ValueError(
                "RLE stream expands beyond screenshot dimensions"
            )

        rgb += pixel * count

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


class ImageHeader(NamedTuple):
    width: int
    height: int
    chunk_size: int
    group_size: int


class ImageEnd(NamedTuple):
    size: int
    crc: int
    chunks: int


class TransferState:
    """Records collected for one IMG BEGIN ... IMG END transfer."""

    def __init__(self, header: ImageHeader) -> None:
        self.header = header
        # Sequence/group numbers are not covered by the record CRC, so a
        # damaged number can make two different payloads claim the same slot.
        # Keep every distinct CRC-valid payload as a candidate.
        self.data_chunks: Dict[int, List[bytes]] = {}
        self.parity_chunks: Dict[int, List[bytes]] = {}
        self.damaged_records = 0
        self.duplicate_records = 0

    def add_candidate(
        self, table: Dict[int, List[bytes]], key: int, payload: bytes
    ) -> None:
        candidates = table.setdefault(key, [])

        if payload in candidates:
            self.duplicate_records += 1
        else:
            candidates.append(payload)


def parse_header(match: "re.Match[str]") -> Optional[ImageHeader]:
    header = ImageHeader(*(int(match.group(i)) for i in range(1, 5)))

    if not (
        0 < header.width <= MAX_WIDTH
        and 0 < header.height <= MAX_HEIGHT
        and header.chunk_size == EXPECTED_CHUNK_SIZE
        and 1 <= header.group_size <= MAX_GROUP_SIZE
    ):
        return None

    return header


def parse_end(match: "re.Match[str]", header: ImageHeader) -> Optional[ImageEnd]:
    end = ImageEnd(
        int(match.group(1)),
        int(match.group(2), 16),
        int(match.group(3)),
    )

    expected_chunks = (
        end.size + header.chunk_size - 1
    ) // header.chunk_size

    if (
        end.size <= 0
        or end.size % 5 != 0
        or end.size > header.width * header.height * 5
        or end.chunks != expected_chunks
    ):
        return None

    return end


def decode_record(
    payload_b64: str, declared_crc: str, max_length: int
) -> Optional[bytes]:
    """Decode a base64 record payload; None if it fails validation."""
    try:
        payload = base64.b64decode(payload_b64, validate=True)
    except (ValueError, binascii.Error):
        return None

    if not payload or len(payload) > max_length:
        return None

    if zlib.crc32(payload) & 0xFFFFFFFF != int(declared_crc, 16):
        return None

    return payload


def parse_line(
    line: str, state: Optional[TransferState]
) -> Tuple[str, Any]:
    """Classify one serial line and record data/parity into `state`.

    Returns one of ("begin", ImageHeader), ("end", match), ("fail", text),
    ("data", None), ("damaged", None), ("bad-begin", None) or ("other", None).
    """
    if "IMG " not in line:
        return "other", None

    match = FAIL_RE.search(line)

    if match:
        return "fail", match.group(0)

    match = BEGIN_RE.search(line)

    if match:
        header = parse_header(match)
        return ("begin", header) if header else ("bad-begin", None)

    if state is None:
        return "other", None

    chunk_size = state.header.chunk_size

    match = DATA_RE.search(line)

    if match:
        payload = decode_record(match.group(4), match.group(3), chunk_size)

        if payload is None or len(payload) != int(match.group(2)):
            state.damaged_records += 1
            return "damaged", None

        state.add_candidate(state.data_chunks, int(match.group(1)), payload)
        return "data", None

    match = PARITY_RE.search(line)

    if match:
        payload = decode_record(match.group(3), match.group(2), chunk_size)

        if payload is None or len(payload) != chunk_size:
            state.damaged_records += 1
            return "damaged", None

        state.add_candidate(state.parity_chunks, int(match.group(1)), payload)
        return "data", None

    match = END_RE.search(line)

    if match:
        return "end", match

    return "other", None


def xor_bytes(chunks: List[bytes], size: int) -> bytes:
    """XOR chunks (zero-padded to `size`) together."""
    accumulator = 0

    for chunk in chunks:
        accumulator ^= int.from_bytes(chunk, "little")

    return accumulator.to_bytes(size, "little")


def apply_fec(
    header: ImageHeader,
    end: ImageEnd,
    data: Dict[int, bytes],
    parity: Dict[int, bytes],
) -> Tuple[Dict[int, bytes], int]:
    """Repair at most one missing chunk per FEC group.

    Returns the completed chunk map and the number of repaired chunks.
    """
    chunk_size = header.chunk_size
    group_size = header.group_size
    chunks = dict(data)
    repaired_chunks = 0

    total_groups = (end.chunks + group_size - 1) // group_size

    for group in range(total_groups):
        first_sequence = group * group_size
        group_sequences = range(
            first_sequence,
            min(first_sequence + group_size, end.chunks),
        )

        missing = [
            sequence
            for sequence in group_sequences
            if sequence not in chunks
        ]

        if not missing:
            continue

        if len(missing) != 1 or group not in parity:
            raise ValueError(
                f"FEC could not repair group {group}: "
                f"missing chunks {missing}, "
                f"parity={'yes' if group in parity else 'no'}"
            )

        missing_sequence = missing[0]

        if missing_sequence == end.chunks - 1:
            missing_length = end.size - chunk_size * (end.chunks - 1)
        else:
            missing_length = chunk_size

        if missing_length <= 0 or missing_length > chunk_size:
            raise ValueError(
                f"FEC calculated invalid recovered chunk length "
                f"{missing_length} for sequence {missing_sequence}"
            )

        recovered = xor_bytes(
            [parity[group]]
            + [
                chunks[sequence]
                for sequence in group_sequences
                if sequence != missing_sequence
            ],
            chunk_size,
        )

        chunks[missing_sequence] = recovered[:missing_length]
        repaired_chunks += 1

    return chunks, repaired_chunks


def assemble_once(
    header: ImageHeader,
    end: ImageEnd,
    data: Dict[int, bytes],
    parity: Dict[int, bytes],
) -> Tuple[bytes, int]:
    """Apply FEC and verify size and CRC; returns (compressed, repaired)."""
    chunks, repaired_chunks = apply_fec(header, end, data, parity)

    compressed = b"".join(
        chunks[sequence]
        for sequence in range(end.chunks)
    )

    if len(compressed) != end.size:
        raise ValueError(
            f"Screenshot compressed-size mismatch: "
            f"got {len(compressed)}, expected {end.size}"
        )

    actual_crc = zlib.crc32(compressed) & 0xFFFFFFFF

    if actual_crc != end.crc:
        raise ValueError(
            f"Screenshot CRC32 mismatch after FEC: "
            f"got {actual_crc:08X}, expected {end.crc:08X}"
        )

    return compressed, repaired_chunks


def assemble(state: TransferState, end: ImageEnd) -> Tuple[bytes, int]:
    """Reassemble the compressed stream for `end`.

    The first candidate of every slot is tried first.  If that fails and
    some sequence/group numbers received several different payloads, a
    bounded number of other combinations is tried.
    """
    header = state.header
    data_slots = {
        sequence: candidates
        for sequence, candidates in state.data_chunks.items()
        if sequence < end.chunks
    }
    parity_slots = state.parity_chunks

    ambiguous = [
        ("D", key, len(candidates))
        for key, candidates in sorted(data_slots.items())
        if len(candidates) > 1
    ] + [
        ("P", key, len(candidates))
        for key, candidates in sorted(parity_slots.items())
        if len(candidates) > 1
    ]

    choices = itertools.islice(
        itertools.product(*(range(count) for _, _, count in ambiguous)),
        MAX_CANDIDATE_ATTEMPTS,
    )

    first_error: Optional[ValueError] = None

    for choice in choices:
        data = {key: candidates[0] for key, candidates in data_slots.items()}
        parity = {key: candidates[0] for key, candidates in parity_slots.items()}

        for (kind, key, _), index in zip(ambiguous, choice):
            if kind == "D":
                data[key] = data_slots[key][index]
            else:
                parity[key] = parity_slots[key][index]

        try:
            return assemble_once(header, end, data, parity)
        except ValueError as exc:
            if first_error is None:
                first_error = exc

    assert first_error is not None
    raise first_error


def iter_lines(port: Any, deadline_ref: List[float]) -> Iterator[str]:
    """Yield decoded serial lines until the deadline in deadline_ref passes.

    deadline_ref is a one-element list so the caller can extend the
    deadline while iterating.
    """
    buffer = bytearray()

    while time.monotonic() < deadline_ref[0]:
        data = port.read(1024)

        if not data:
            continue

        buffer += data
        start = 0

        while True:
            newline = buffer.find(b"\n", start)

            if newline < 0:
                break

            yield buffer[start:newline].decode(
                "utf-8",
                errors="replace",
            ).rstrip("\r")
            start = newline + 1

        del buffer[:start]

        if len(buffer) > MAX_LINE_BYTES:
            # Runaway line without newline: keep only its tail, which is
            # enough for an end-anchored record glued to the noise.
            del buffer[:-MAX_LINE_BYTES]


def receive_screenshot(
    port: Any,
    output_path: Path,
    timeout: float,
    verbose: bool = False,
) -> Dict[str, Any]:
    deadline = [time.monotonic() + timeout]
    state: Optional[TransferState] = None
    compressed: Optional[bytes] = None
    repaired_chunks = 0

    tried_ends: List[ImageEnd] = []
    end_error: Optional[Exception] = None
    end_wait_until: Optional[float] = None

    print("Waiting for Mini Browser screenshot...")

    for line in iter_lines(port, deadline):
        stripped = line.strip()
        kind, value = parse_line(stripped, state)

        # Treat --timeout as an inactivity timeout once an image transfer
        # has started. Full-page WHY+Z screenshots can legitimately take
        # much longer than a 716x716 viewport screenshot.
        if state is not None and kind != "other":
            deadline[0] = time.monotonic() + timeout

        if end_wait_until is not None:
            deadline[0] = min(deadline[0], end_wait_until)

        if kind == "fail":
            raise RuntimeError(
                f"Badge screenshot failed: {value}"
            )

        if kind == "begin":
            # Any BEGIN (including a repeated one) starts a fresh transfer.
            if state is None or state.data_chunks or state.header != value:
                print(
                    f"Receiving {value.width}x{value.height} screenshot "
                    f"(chunk {value.chunk_size}, FEC group {value.group_size})..."
                )
            state = TransferState(value)
            tried_ends = []
            end_error = None
            end_wait_until = None
            deadline[0] = time.monotonic() + timeout
            continue

        if kind == "end" and state is not None:
            end = parse_end(value, state.header)

            if end is None:
                end_error = ValueError(
                    f"Implausible screenshot END record: {stripped}"
                )
            elif end not in tried_ends:
                tried_ends.append(end)

                try:
                    compressed, repaired_chunks = assemble(state, end)
                    break
                except ValueError as exc:
                    end_error = exc

            # Wait briefly for the repeated END line before giving up.
            if end_wait_until is None:
                end_wait_until = time.monotonic() + SECOND_END_WAIT
                deadline[0] = min(deadline[0], end_wait_until)

            continue

        if kind in ("other", "bad-begin") and verbose and stripped:
            print(line)

    if compressed is None:
        if end_error is not None:
            raise end_error

        if state is None:
            raise TimeoutError(
                "Timed out waiting for IMG BEGIN. "
                "Press WHY+S or WHY+Z while Mini Browser is running."
            )

        raise TimeoutError(
            "Timed out waiting for complete IMG screenshot transfer"
        )

    assert state is not None  # compressed is only set for a transfer
    header = state.header
    actual_crc = zlib.crc32(compressed) & 0xFFFFFFFF

    rgb = decode_rle5(
        compressed,
        header.width,
        header.height,
    )

    write_png_rgb(
        output_path,
        header.width,
        header.height,
        rgb,
    )

    return {
        "path": str(output_path),
        "width": header.width,
        "height": header.height,
        "compressed_bytes": len(compressed),
        "crc32": f"{actual_crc:08X}",
        "fec_repaired_chunks": repaired_chunks,
        "damaged_records": state.damaged_records,
        "duplicate_records": state.duplicate_records,
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
        default=None,
        help=(
            "serial device (default: $BADGE_PORT, else auto-detect "
            "WHY2025 badge)"
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

    device = detect_badge_device(args.device)

    output_path = (
        Path(args.output)
        if args.output
        else default_output(args.full_page)
    )

    port = None

    try:
        port = serial.Serial(
            device,
            args.baud,
            timeout=0.1,
        )

        # Throw away stale serial text from before this receiver started.
        port.reset_input_buffer()

        print(f"Serial: {device} @ {args.baud}")

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
