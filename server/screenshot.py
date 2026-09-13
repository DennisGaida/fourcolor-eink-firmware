#!/usr/bin/env python3
"""
screenshot.py — pulls a real on-device render from the busy-light firmware's
/screenshot debug endpoint and decodes it to a PNG.

Dev-loop tool: instead of flashing + photographing the panel to check a
layout change, hit the device's LAN HTTP server directly.

Wire format (see ApTransferServer::ScreenshotHandler in
firmware/main/ui/renderers/rawdraw/ap_transfer_server.cc):
    bytes 0-1: width  (uint16 LE)
    bytes 2-3: height (uint16 LE)
    bytes 4-:  raw 2bpp framebuffer, packed MSB-first, 4 pixels/byte,
               (width*2+7)//8 bytes per row. 2-bit codes: BLACK=0, WHITE=1,
               YELLOW=2, RED=3 (see rawdraw::Color in firmware/main/rawdraw/rawdraw.h).

Usage:
    python3 screenshot.py <device-ip> [output.png]
"""

import struct
import sys
from urllib.request import urlopen

from PIL import Image

# Approximate SSD2683 four-color e-paper palette (not colorimetrically
# calibrated — good enough to judge layout, not exact on-panel color).
PALETTE = {
    0: (20, 20, 20),      # BLACK
    1: (245, 245, 240),   # WHITE
    2: (235, 200, 40),    # YELLOW
    3: (200, 30, 30),     # RED
}


def decode_framebuffer(width: int, height: int, raw: bytes) -> Image.Image:
    bytes_per_row = (width * 2 + 7) // 8
    img = Image.new("RGB", (width, height))
    pixels = img.load()

    for y in range(height):
        row_off = y * bytes_per_row
        for x in range(width):
            byte = raw[row_off + (x >> 2)]
            shift = 6 - ((x & 0x03) << 1)
            code = (byte >> shift) & 0x03
            pixels[x, y] = PALETTE[code]

    return img


def main():
    if len(sys.argv) < 2:
        print(__doc__)
        sys.exit(1)

    device_ip = sys.argv[1]
    out_path = sys.argv[2] if len(sys.argv) > 2 else "screenshot.png"

    url = f"http://{device_ip}/screenshot"
    print(f"Fetching {url} ...")
    with urlopen(url, timeout=10) as resp:
        blob = resp.read()

    if len(blob) < 4:
        print(f"Response too short ({len(blob)} bytes)")
        sys.exit(1)

    width, height = struct.unpack("<HH", blob[:4])
    raw = blob[4:]
    expected = ((width * 2 + 7) // 8) * height
    if len(raw) < expected:
        print(f"Truncated framebuffer: got {len(raw)} bytes, expected {expected}")
        sys.exit(1)

    img = decode_framebuffer(width, height, raw)
    img.save(out_path)
    print(f"Saved {width}x{height} screenshot to {out_path}")


if __name__ == "__main__":
    main()
