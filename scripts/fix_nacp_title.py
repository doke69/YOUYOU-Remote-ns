#!/usr/bin/env python3
"""Write the UTF-8 YOUYOU远程 application title into every NACP title slot.

devkitPro nacptool on a Windows ANSI console writes the Chinese argv title as
CP936. NACP/ControlInfo has 16 language entries at fixed 0x300-byte strides;
only the first 0x200 bytes of each entry are the application title.

This script validates the known 0x4000-byte NACP layout and only replaces those
16 title fields. Author, version and all remaining NACP bytes are preserved.
"""
from pathlib import Path
import sys

NACP_SIZE = 0x4000
LANGUAGE_STRIDE = 0x300
TITLE_FIELD = 0x200
TITLE = "YOUYOU远程".encode("utf-8")
assert len(TITLE) < TITLE_FIELD

path = Path(sys.argv[1])
data = bytearray(path.read_bytes())
if len(data) != NACP_SIZE:
    raise SystemExit(f"unexpected NACP size {len(data)} (want {NACP_SIZE})")

# Validate each known title slot before changing anything: nacptool writes the
# ASCII title into populated slots and zero-fills unsupported slots.
for i in range(16):
    off = i * LANGUAGE_STRIDE
    field = data[off:off + TITLE_FIELD]
    if not (field.startswith(b"YOUYOU") or not any(field)):
        raise SystemExit(f"unexpected NACP title slot {i} at {off:#x}: {field[:8].hex()}")
if data[0:6] != b"YOUYOU":
    raise SystemExit("NACP slot 0 does not start with ASCII YOUYOU")

for i in range(16):
    off = i * LANGUAGE_STRIDE
    data[off:off + TITLE_FIELD] = TITLE + b"\0" * (TITLE_FIELD - len(TITLE))

path.write_bytes(data)

# Numeric verification, not just a print of the string.
check = path.read_bytes()
for i in range(16):
    off = i * LANGUAGE_STRIDE
    field = check[off:off + TITLE_FIELD]
    got = field.split(b"\0", 1)[0]
    if got != TITLE or any(field[len(TITLE):]):
        raise SystemExit(f"slot {i} verification failed: {field[:16].hex()}")
if check[:len(TITLE)] != TITLE:
    raise SystemExit("slot 0 UTF-8 bytes mismatch")
print("patched 16 NACP language title slots; slot0=" + check[:len(TITLE)].hex())
