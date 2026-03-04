#!/usr/bin/env python3
import struct
import zlib
import sys

MAGIC = b"FRY0"
VERSION = 1
FLAG_ELF64 = 0x0001


def main():
    if len(sys.argv) != 3:
        print("usage: frypack.py input.elf output.fry")
        return 1
    inp, outp = sys.argv[1], sys.argv[2]
    with open(inp, "rb") as f:
        payload = f.read()
    crc = zlib.crc32(payload) & 0xFFFFFFFF
    header = struct.pack("<4sHHII", MAGIC, VERSION, FLAG_ELF64, crc, len(payload))
    with open(outp, "wb") as f:
        f.write(header)
        f.write(payload)
    return 0


if __name__ == "__main__":
    sys.exit(main())
