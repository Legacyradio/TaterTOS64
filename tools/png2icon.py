#!/usr/bin/env python3
"""
png2icon.py — Convert PNG icons to TaterTOS .icon format
Format: 4-byte width (LE) + 4-byte height (LE) + w*h*4 bytes BGRX pixels
Scale down to 48x48 for desktop display.
"""

import sys, os, struct
from PIL import Image

ICON_SIZE = 48  # desktop icon dimension

def convert(src_png, dst_icon):
    im = Image.open(src_png).convert('RGBA')
    im = im.resize((ICON_SIZE, ICON_SIZE), Image.LANCZOS)

    pixels = bytearray()
    for y in range(ICON_SIZE):
        for x in range(ICON_SIZE):
            r, g, b, a = im.getpixel((x, y))
            if a < 128:
                # Transparent → desktop background color 0x0D1117
                pixels += struct.pack('BBBB', 0x17, 0x11, 0x0D, 0xFF)
            else:
                pixels += struct.pack('BBBB', b, g, r, 0xFF)  # BGRX

    with open(dst_icon, 'wb') as f:
        f.write(struct.pack('<II', ICON_SIZE, ICON_SIZE))
        f.write(pixels)

    print(f"  {os.path.basename(src_png)} -> {os.path.basename(dst_icon)} ({ICON_SIZE}x{ICON_SIZE})")

def main():
    src_dir = sys.argv[1] if len(sys.argv) > 1 else 'TaterTOS64-icons-v2'
    dst_dir = sys.argv[2] if len(sys.argv) > 2 else 'out/icons'

    os.makedirs(dst_dir, exist_ok=True)

    for f in sorted(os.listdir(src_dir)):
        if f.endswith('.png'):
            name = f[:-4].upper()  # browse.png -> BROWSE
            convert(os.path.join(src_dir, f), os.path.join(dst_dir, name + '.ICON'))

if __name__ == '__main__':
    main()
