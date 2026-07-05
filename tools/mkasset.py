#!/usr/bin/env python3
"""
mkasset.py - SacabambaspOS boot image asset builder.

Converts a source JPG/PNG into a raw BGRA framebuffer blob that both the
UEFI (GOP) and BIOS (VBE) boot stages can blit directly to the screen with
NO in-OS image decoder. Pixel order is B,G,R,X to match:
  - UEFI EFI_GRAPHICS_OUTPUT_BLT_PIXEL (Blue,Green,Red,Reserved)
  - VESA 32bpp linear framebuffers on little-endian x86 (0xXXRRGGBB in memory
    reads as B,G,R,X byte order)

Format 'SBMP' v1 header (32 bytes, little-endian), then width*height BGRA px,
top row first:

  off  size  field
  0    4     magic  "SBMP"
  4    4     version (=1)
  8    4     width   (pixels)
  12   4     height  (pixels)
  16   4     bpp     (=32)
  20   4     pixfmt  (0 = BGRA)
  24   4     reserved0
  28   4     reserved1
"""
import sys, struct, argparse
from PIL import Image

MAGIC = b"SBMP"
VERSION = 1
PIXFMT_BGRA = 0


def build(src, dst, max_w, max_h):
    im = Image.open(src).convert("RGBA")
    # Downscale to fit within max box, preserve aspect. Never upscale here;
    # the boot stages upscale-to-fit at runtime for the actual panel res.
    w, h = im.size
    if w > max_w or h > max_h:
        scale = min(max_w / w, max_h / h)
        nw, nh = max(1, int(w * scale)), max(1, int(h * scale))
        im = im.resize((nw, nh), Image.LANCZOS)
        w, h = im.size

    # RGBA -> BGRA
    r, g, b, a = im.split()
    bgra = Image.merge("RGBA", (b, g, r, a))
    px = bgra.tobytes()  # rows top-to-bottom, B,G,R,A per pixel

    hdr = struct.pack("<4sIIIIIII", MAGIC, VERSION, w, h, 32, PIXFMT_BGRA, 0, 0)
    with open(dst, "wb") as f:
        f.write(hdr)
        f.write(px)
    print(f"[mkasset] {src} -> {dst}  {w}x{h}  {len(hdr)+len(px)} bytes")
    return w, h


if __name__ == "__main__":
    ap = argparse.ArgumentParser()
    ap.add_argument("src")
    ap.add_argument("dst")
    ap.add_argument("--max-w", type=int, default=1920)
    ap.add_argument("--max-h", type=int, default=1080)
    a = ap.parse_args()
    build(a.src, a.dst, a.max_w, a.max_h)
