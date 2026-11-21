#!/usr/bin/env python3
"""Reject firmware UF2 blocks that could overwrite existing BT bonds/nudge settings."""
import struct
import sys
from pathlib import Path

data = Path(sys.argv[1]).read_bytes()
if not data or len(data) % 512:
    raise SystemExit("Invalid UF2 length")
highest = 0
for at in range(0, len(data), 512):
    a, b, flags, address, length, number, count, family = struct.unpack_from("<8I", data, at)
    if (a, b, struct.unpack_from("<I", data, at + 508)[0]) != (0x0A324655, 0x9E5D5157, 0x0AB16F30):
        raise SystemExit("Invalid UF2 framing")
    if flags != 0x2000 or family != 0xE48BFF56 or not 0 < length <= 476:
        raise SystemExit("Expected RP2040 flash UF2 blocks")
    if address < 0x10000000 or address + length > 0x101FE000:
        raise SystemExit("Firmware would overlap the reserved final 8 KiB; refuse to flash")
    highest = max(highest, address + length)
print(f"PASS: firmware ends at 0x{highest:08x}; final 8 KiB remain untouched")
