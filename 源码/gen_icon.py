#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""生成应用图标 app.ico（红色圆角方块 + 白色播放三角），纯标准库，无依赖。"""
import struct

SIZES = [256, 64, 48, 32, 16]
RED = (232, 36, 59)      # #E8243B
WHITE = (255, 255, 255)

def in_rounded_rect(x, y, size):
    """圆角矩形：[0.08,0.92] 区域，圆角半径 0.20*size。x,y 为子像素坐标（0..2*size）。"""
    s2 = size * 2.0
    m0 = 0.08 * s2
    m1 = 0.92 * s2
    r = 0.20 * s2
    if x < m0 or x > m1 or y < m0 or y > m1:
        return False
    # 四角圆角判断
    cx = m0 + r if x < m0 + r else (m1 - r if x > m1 - r else x)
    cy = m0 + r if y < m0 + r else (m1 - r if y > m1 - r else y)
    if cx != x and cy != y:  # 在角区
        dx, dy = x - cx, y - cy
        if dx * dx + dy * dy > r * r:
            return False
    return True

def in_triangle(x, y, size):
    """播放三角：顶点相对坐标。"""
    s2 = size * 2.0
    x0, y0 = 0.34 * s2, 0.27 * s2
    x1, y1 = 0.34 * s2, 0.73 * s2
    x2, y2 = 0.78 * s2, 0.50 * s2
    def sign(ax, ay, bx, by, px, py):
        return (px - bx) * (ay - by) - (ax - bx) * (py - by)
    d1 = sign(x0, y0, x1, y1, x, y)
    d2 = sign(x1, y1, x2, y2, x, y)
    d3 = sign(x2, y2, x0, y0, x, y)
    neg = (d1 < 0) or (d2 < 0) or (d3 < 0)
    pos = (d1 > 0) or (d2 > 0) or (d3 > 0)
    return not (neg and pos)

def render(size):
    """返回 BGRA 像素行（自下而上），含 2x2 超采样抗锯齿。"""
    ss = 2
    rows = []
    for py in range(size - 1, -1, -1):
        row = bytearray()
        for px in range(size):
            r_acc = g_acc = b_acc = a_acc = 0.0
            for sy in range(ss):
                for sx in range(ss):
                    x = px * ss + sx + 0.5
                    y = py * ss + sy + 0.5
                    in_rect = in_rounded_rect(x, y, size)
                    in_tri = in_triangle(x, y, size)
                    if in_rect:
                        col = WHITE if in_tri else RED
                        r_acc += col[0]; g_acc += col[1]; b_acc += col[2]; a_acc += 255.0
            n = ss * ss
            if a_acc > 0:
                row += bytes((int(b_acc / a_acc * a_acc / n),   # B
                              int(g_acc / a_acc * a_acc / n),   # G
                              int(r_acc / a_acc * a_acc / n),   # R
                              int(a_acc / n)))                  # A
            else:
                row += b"\x00\x00\x00\x00"
        rows.append(bytes(row))
    return rows

def build_ico(path):
    images = []
    for size in SIZES:
        rows = render(size)
        # BITMAPINFOHEADER + 像素 + AND 掩码
        hdr = struct.pack("<IiiHHIIiiII", 40, size, size * 2, 1, 32, 0,
                          size * size * 4, 0, 0, 0, 0)
        and_mask = b"\x00" * (size * ((size + 31) // 32) * 4)
        data = hdr + b"".join(rows) + and_mask
        entry = struct.pack("<BBBBHHII", size % 256, size % 256, 0, 0, 1, 32,
                            len(data), 0)  # offset 稍后回填
        images.append((entry, data))

    offset = 6 + 16 * len(images)
    out = struct.pack("<HHH", 0, 1, len(images))
    for entry, data in images:
        out += entry[:12] + struct.pack("<I", offset)
        offset += len(data)
    for entry, data in images:
        out += data
    with open(path, "wb") as f:
        f.write(out)
    print("icon written:", path, len(out), "bytes")

if __name__ == "__main__":
    build_ico("app.ico")
