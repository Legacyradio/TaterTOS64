#!/usr/bin/env python3
import struct
import zlib
import sys

MAGIC = b"TOT1"
VERSION = 1
TYPE_DRIVER = 0x0001


def main():
    if len(sys.argv) < 3:
        print("usage: totpack.py input.o output.tot [type]")
        return 1
    inp, outp = sys.argv[1], sys.argv[2]
    mod_type = TYPE_DRIVER
    if len(sys.argv) >= 4:
        mod_type = int(sys.argv[3], 0)
    with open(inp, "rb") as f:
        payload = f.read()
    crc = zlib.crc32(payload) & 0xFFFFFFFF
    header = struct.pack("<4sHHII", MAGIC, VERSION, mod_type, crc, len(payload))
    with open(outp, "wb") as f:
        f.write(header)
        f.write(payload)
    return 0


if __name__ == "__main__":
    sys.exit(main())
