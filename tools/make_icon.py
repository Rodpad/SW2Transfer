#!/usr/bin/env python3
"""Generates the SW2Transfer icon (256x256 PNG) in the style of the SW2NES /
SW2SNES / SW2PS2 icons: square navy background, pixel-font "SW2 XFER" title,
artwork, coloured bar. Artwork: parallel up and down arrows (several
connections moving one file). Convert with sips to sw2transfer.jpg."""
import struct, sys, zlib

W = H = 256
BG = (26, 29, 40)
UP, DOWN, BAR = (79, 163, 255), (91, 209, 122), (228, 72, 64)
px = [[BG] * W for _ in range(H)]

def rect(x0, y0, x1, y1, c, r=0):
    for y in range(max(0, y0), min(H, y1)):
        for x in range(max(0, x0), min(W, x1)):
            if r:
                cx = min(max(x, x0 + r), x1 - 1 - r); cy = min(max(y, y0 + r), y1 - 1 - r)
                if (x - cx) ** 2 + (y - cy) ** 2 > r * r:
                    continue
            px[y][x] = c

def tri(cx, tip, base, half, c):
    # isosceles triangle pointing up (tip < base) or down (tip > base)
    for y in range(min(tip, base), max(tip, base) + 1):
        w = int(half * abs(y - tip) / max(1, abs(base - tip)))
        for x in range(cx - w, cx + w + 1):
            px[y][x] = c

font = {"S": ["01111", "10000", "01110", "00001", "11110"], "W": ["10001", "10001", "10101", "11011", "10001"],
        "2": ["01110", "10001", "00110", "01000", "11111"], "X": ["10001", "01010", "00100", "01010", "10001"],
        "F": ["11111", "10000", "11110", "10000", "10000"], "E": ["11111", "10000", "11110", "10000", "11111"],
        "R": ["11110", "10001", "11110", "10100", "10011"]}

def text(s, x, y, scale, c):
    for ch in s:
        g = font[ch]
        for gy, bits in enumerate(g):
            for gx, bit in enumerate(bits):
                if bit == "1":
                    rect(x + gx * scale, y + gy * scale, x + (gx + 1) * scale, y + (gy + 1) * scale, c)
        x += (len(g[0]) + 1) * scale

text("SW2", 18, 28, 5, (154, 163, 181))
text("XFER", 116, 28, 5, (238, 241, 247))
# three up arrows (upload, blue) and three down arrows (download, green)
for cx in (50, 82, 114):
    rect(cx - 6, 106, cx + 6, 190, UP, 3)
    tri(cx, 76, 112, 14, UP)
for cx in (142, 174, 206):
    rect(cx - 6, 76, cx + 6, 160, DOWN, 3)
    tri(cx, 190, 154, 14, DOWN)
rect(18, 206, 238, 216, BAR)

raw = b"".join(b"\x00" + bytes(v for p in row for v in p) for row in px)
def chunk(t, d):
    return struct.pack(">I", len(d)) + t + d + struct.pack(">I", zlib.crc32(t + d) & 0xFFFFFFFF)
png = b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", W, H, 8, 2, 0, 0, 0)) + chunk(b"IDAT", zlib.compress(raw, 9)) + chunk(b"IEND", b"")
open(sys.argv[1] if len(sys.argv) > 1 else "icon.png", "wb").write(png)
