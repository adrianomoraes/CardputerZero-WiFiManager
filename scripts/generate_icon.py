#!/usr/bin/env python3
"""
Generate a 100x100 PNG placeholder icon for CardputerZero Wi-Fi Manager.
Author: Adriano Dias Moraes <adriano.moraes@gmail.com>
SPDX-License-Identifier: MIT
"""
import math
import struct
import zlib
from pathlib import Path


def chunk(kind, data):
    body = kind + data
    return struct.pack(">I", len(data)) + body + struct.pack(">I", zlib.crc32(body) & 0xFFFFFFFF)


def png_rgba(width, height, pixels):
    raw = bytearray()
    for y in range(height):
        raw.append(0)
        for x in range(width):
            raw.extend(pixels(x, y))
    return (
        b"\x89PNG\r\n\x1a\n"
        + chunk(b"IHDR", struct.pack(">IIBBBBB", width, height, 8, 6, 0, 0, 0))
        + chunk(b"IDAT", zlib.compress(bytes(raw), 9))
        + chunk(b"IEND", b"")
    )


def in_rounded_rect(x, y, x0, y0, x1, y1, r):
    if x < x0 or x >= x1 or y < y0 or y >= y1:
        return False
    for cx, cy in (
        (x0 + r, y0 + r),
        (x1 - r - 1, y0 + r),
        (x0 + r, y1 - r - 1),
        (x1 - r - 1, y1 - r - 1),
    ):
        if ((x < x0 + r or x >= x1 - r) and (y < y0 + r or y >= y1 - r)):
            if (x - cx) ** 2 + (y - cy) ** 2 > r * r:
                return False
    return True


def main():
    size = 100

    def pixel(x, y):
        # Outer rounded card
        if not in_rounded_rect(x, y, 8, 8, 92, 92, 18):
            return (0, 0, 0, 0)

        # Subtle vertical dark-teal gradient background
        t = (y - 8) / 84.0
        bg_r = int(14 + t * 8)
        bg_g = int(28 + t * 22)
        bg_b = int(48 + t * 36)
        color = (bg_r, bg_g, bg_b, 255)

        # Subtle inner border highlight
        if not in_rounded_rect(x, y, 10, 10, 90, 90, 16):
            return (56, 189, 248, 220)

        # Wi-Fi signal arcs centered around (50, 58)
        cx, cy = 50.0, 58.0
        dx = x - cx
        dy = y - cy
        dist = math.hypot(dx, dy)
        angle = math.degrees(math.atan2(-dy, dx))  # 90 deg is straight up

        # Center transmitter dot
        if dist <= 5.2:
            return (56, 189, 248, 255)

        # Three concentric upper arcs (angles between 40 and 140 degrees)
        if 40.0 <= angle <= 140.0:
            for arc_r in (15.0, 26.0, 37.0):
                if abs(dist - arc_r) <= 2.8:
                    return (125, 211, 252, 255) if arc_r < 30 else (56, 189, 248, 255)

        # USB dongle badge at bottom-right (x: 58..84, y: 64..84)
        if in_rounded_rect(x, y, 60, 66, 84, 82, 4):
            color = (245, 158, 11, 255)  # Amber USB body
            # USB text / stripes inside body
            if 66 <= x <= 78 and y in (71, 76):
                color = (30, 41, 59, 255)
        # USB metal connector prongs on left of badge
        if 53 <= x < 60 and 69 <= y <= 79:
            color = (226, 232, 240, 255)
            if y in (72, 76) and 55 <= x <= 58:
                color = (71, 85, 105, 255)

        return color

    out = Path(__file__).resolve().parents[1] / "share/images/wifi-manager.png"
    out.parent.mkdir(parents=True, exist_ok=True)
    out.write_bytes(png_rgba(size, size, pixel))
    print(f"Generated {out}")


if __name__ == "__main__":
    main()
