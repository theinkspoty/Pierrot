#!/usr/bin/env python3
"""Gera packaging/windows/pierrot.ico (256x256) sem dependências externas."""
import math
import os
import struct

S = 256


def rounded_rect_dist(px, py, x0, y0, x1, y1, r):
    cx = (x0 + x1) / 2.0
    cy = (y0 + y1) / 2.0
    hw = (x1 - x0) / 2.0 - r
    hh = (y1 - y0) / 2.0 - r
    qx = abs(px - cx) - hw
    qy = abs(py - cy) - hh
    ax = max(qx, 0.0)
    ay = max(qy, 0.0)
    return math.hypot(ax, ay) - r


def in_triangle(px, py, a, b, c):
    def sign(x1, y1, x2, y2, x3, y3):
        return (x1 - x3) * (y2 - y3) - (x2 - x3) * (y1 - y3)

    d1 = sign(px, py, a[0], a[1], b[0], b[1])
    d2 = sign(px, py, b[0], b[1], c[0], c[1])
    d3 = sign(px, py, c[0], c[1], a[0], a[1])
    neg = (d1 < 0) or (d2 < 0) or (d3 < 0)
    pos = (d1 > 0) or (d2 > 0) or (d3 > 0)
    return not (neg and pos)


def dist_segment(px, py, x1, y1, x2, y2):
    vx, vy = x2 - x1, y2 - y1
    wx, wy = px - x1, py - y1
    l2 = vx * vx + vy * vy
    t = max(0.0, min(1.0, (wx * vx + wy * vy) / l2)) if l2 else 0.0
    dx, dy = px - (x1 + t * vx), py - (y1 + t * vy)
    return math.hypot(dx, dy)


def lerp(a, b, t):
    return a + (b - a) * t


BG = (0, 0, 0, 0)
TILE = (31, 31, 34, 255)
VIDEO = (32, 62, 116, 255)
VIDEO_BORDER = (90, 138, 210, 255)
TRI = (234, 239, 249, 255)
CUT = (255, 70, 70, 255)

pix = [[BG for _ in range(S)] for _ in range(S)]

for y in range(S):
    for x in range(S):
        px = x + 0.5
        py = y + 0.5

        if rounded_rect_dist(px, py, 8, 8, 248, 248, 52) <= 0.0:
            pix[y][x] = TILE

        d_inner = rounded_rect_dist(px, py, 40, 36, 216, 160, 16)
        if d_inner <= 0.0:
            pix[y][x] = VIDEO_BORDER if d_inner >= -6.0 else VIDEO

        if in_triangle(px, py, (108, 74), (150, 98), (108, 122)):
            pix[y][x] = TRI

        d_line = dist_segment(px, py, 48, 200, 208, 200)
        if d_line <= 5.0:
            pix[y][x] = CUT

        for cx, cy in ((96, 200), (160, 200)):
            d = math.hypot(px - cx, py - cy)
            if 13.0 <= d <= 21.0:
                pix[y][x] = CUT

buf = bytearray()
for y in range(S - 1, -1, -1):
    for x in range(S):
        b, g, r, a = pix[y][x]
        buf += struct.pack('<BBBB', b, g, r, a)

mask_row = (S + 31) // 32 * 4
mask = bytearray(mask_row * S)

dib = bytearray()
dib += struct.pack('<IiiHHIIiiII', 40, S, S * 2, 1, 32, 0, len(buf), 0, 0, 0, 0)
dib += buf
dib += mask

header = struct.pack('<HHH', 0, 1, 1)
entry = struct.pack('<BBBBHHII', 0, 0, 0, 0, 1, 32, len(dib), 22)
out = header + entry + dib

out_path = os.path.join(os.path.dirname(os.path.abspath(__file__)), 'pierrot.ico')
with open(out_path, 'wb') as f:
    f.write(out)
print(f'{out_path} ({len(out)} bytes)')
