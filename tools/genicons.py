#!/usr/bin/env python3
"""Render the shipped app icons (icons/<Icon= name>.png, 32x32 RGBA) with 4x4 supersampling.

Usage: python3 -I tools/genicons.py icons
"""
import math
import os
import struct
import sys
import zlib

N = 32
SS = 4


def rrect(x0, y0, x1, y1, r):
    def f(x, y):
        cx = min(max(x, x0 + r), x1 - r)
        cy = min(max(y, y0 + r), y1 - r)
        return x0 <= x <= x1 and y0 <= y <= y1 and (x - cx) ** 2 + (y - cy) ** 2 <= r * r
    return f


def circle(cx, cy, r):
    return lambda x, y: (x - cx) ** 2 + (y - cy) ** 2 <= r * r


def ring(cx, cy, r0, r1):
    return lambda x, y: r0 * r0 <= (x - cx) ** 2 + (y - cy) ** 2 <= r1 * r1


def rect(x0, y0, x1, y1):
    return lambda x, y: x0 <= x <= x1 and y0 <= y <= y1


def line(x0, y0, x1, y1, w):
    dx, dy = x1 - x0, y1 - y0
    ll = dx * dx + dy * dy

    def f(x, y):
        t = max(0.0, min(1.0, ((x - x0) * dx + (y - y0) * dy) / ll))
        return (x - (x0 + t * dx)) ** 2 + (y - (y0 + t * dy)) ** 2 <= (w / 2) ** 2
    return f


def poly(pts):
    def f(x, y):
        inside = False
        j = len(pts) - 1
        for i in range(len(pts)):
            xi, yi = pts[i]
            xj, yj = pts[j]
            if (yi > y) != (yj > y) and x < (xj - xi) * (y - yi) / (yj - yi) + xi:
                inside = not inside
            j = i
        return inside
    return f


def union(*fs):
    return lambda x, y: any(f(x, y) for f in fs)


def sub(a, b):
    return lambda x, y: a(x, y) and not b(x, y)


def tile(top, bottom, r=6):
    """Rounded square base with a vertical gradient, as a scene entry."""
    shape = rrect(1, 1, 31, 31, r)

    def color(y):
        t = (y - 1) / 30
        return tuple(top[i] + (bottom[i] - top[i]) * t for i in range(3))
    return shape, color


W = (255, 255, 255)


def scene_term():
    return [tile((58, 66, 82), (28, 32, 42)),
            (line(8, 10, 14, 16, 2.6), (120, 230, 140)), (line(14, 16, 8, 22, 2.6), (120, 230, 140)),
            (rect(17, 21, 25, 23.4), W)]


def scene_files():
    return [tile((120, 160, 230), (60, 100, 190)),
            (poly([(6, 9), (14, 9), (16, 12), (26, 12), (26, 14), (6, 14)]), (240, 190, 70)),
            (rrect(6, 12, 26, 25, 1.5), (250, 208, 100))]


def scene_editor():
    out = [tile((110, 120, 140), (70, 78, 98)), (rrect(8, 5, 24, 27, 1.5), (250, 250, 252))]
    for i in range(5):
        out.append((rect(10.5, 9 + i * 3.6, 21.5 - (i % 2) * 4, 10.4 + i * 3.6), (70, 110, 190)))
    return out


def ellipse_ring(cx, cy, rx, ry, t):
    def f(x, y):
        d = math.hypot((x - cx) / rx, (y - cy) / ry)
        return abs(d - 1) * min(rx, ry) <= t / 2
    return f


def scene_browser():
    pale = (245, 250, 255)
    return [tile((90, 160, 240), (40, 90, 190)), (circle(16, 16, 11.5), pale), (circle(16, 16, 10), (60, 130, 220)),
            (ellipse_ring(16, 16, 4.6, 10, 1.6), pale), (rect(6, 15.2, 26, 16.8), pale),
            (rect(8, 10.6, 24, 11.8), pale), (rect(8, 20.2, 24, 21.4), pale)]


def scene_player():
    return [tile((150, 90, 210), (80, 40, 150)), (poly([(12, 8), (24, 16), (12, 24)]), W)]


def scene_clock():
    return [tile((70, 190, 170), (30, 120, 120)), (circle(16, 16, 11.5), W), (circle(16, 16, 10.4), (40, 60, 80)),
            (circle(16, 16, 9.6), (250, 250, 250)), (line(16, 16, 16, 9, 1.8), (40, 40, 50)),
            (line(16, 16, 21, 18.5, 1.8), (40, 40, 50)), (circle(16, 16, 1.4), (200, 50, 50))]


def scene_calc():
    out = [tile((90, 96, 110), (50, 54, 66)), (rrect(7, 5, 25, 27, 2), (225, 228, 235)),
           (rrect(9, 7, 23, 12.5, 1), (90, 160, 120))]
    for r in range(3):
        for c in range(3):
            out.append((rrect(9 + c * 5, 14.5 + r * 4, 12.4 + c * 5, 17.4 + r * 4, 0.6),
                        (230, 130, 50) if c == 2 else (120, 126, 140)))
    return out


def scene_notify():
    bell = union(poly([(16, 6), (21, 9), (22, 17), (25, 21), (7, 21), (10, 17), (11, 9)]), circle(16, 7.5, 3.2))
    return [tile((250, 170, 80), (210, 100, 50)), (bell, W), (circle(16, 24, 2.4), W)]


def scene_settings():
    teeth = [rect(14, 3, 18, 29), rect(3, 14, 29, 18)]
    for a in (math.pi / 4, -math.pi / 4):
        c, s = math.cos(a), math.sin(a)
        teeth.append(line(16 - 13 * c, 16 - 13 * s, 16 + 13 * c, 16 + 13 * s, 4))
    gear = union(circle(16, 16, 9.5), *teeth)
    return [tile((130, 140, 160), (80, 88, 108)), (sub(gear, circle(16, 16, 4)), W)]


def scene_about():
    return [tile((90, 160, 240), (40, 90, 200)), (circle(16, 16, 11.5), W), (circle(16, 9.8, 1.9), (50, 110, 210)),
            (rrect(14.2, 13.4, 17.8, 23.4, 0.8), (50, 110, 210))]


def scene_default():
    return [tile((160, 166, 180), (110, 116, 132)), (rrect(6, 7, 26, 25, 2), W),
            (rect(6, 7, 26, 11.5), (90, 100, 125)), (rect(9, 14.5, 23, 22), (205, 210, 222))]


def arc(cx, cy, r0, r1):
    rg = ring(cx, cy, r0, r1)
    return lambda x, y: rg(x, y) and x > cx and abs(y - cy) <= x - cx


def speaker():
    return [(rect(5, 12, 11, 20), W), (poly([(11, 12), (18, 6.5), (18, 25.5), (11, 20)]), W)]


def scene_vol_muted():
    return speaker() + [(line(21, 12, 28, 20, 2.4), (230, 90, 80)), (line(28, 12, 21, 20, 2.4), (230, 90, 80))]


def scene_vol_low():
    return speaker() + [(arc(18, 16, 4, 6.4), W)]


def scene_vol_high():
    return speaker() + [(arc(18, 16, 4, 6.4), W), (arc(18, 16, 8.4, 10.8), W)]


def battery(level, color):
    body = sub(rrect(2, 9, 27, 23, 2), rrect(4, 11, 25, 21, 1))
    out = [(body, W), (rect(27, 13, 30, 19), W)]
    if level:
        out.append((rect(5.5, 12.5, 5.5 + 18 * level, 19.5), color))
    return out


def scene_bat_low():
    return battery(0.2, (230, 90, 80))


def scene_bat_medium():
    return battery(0.55, (240, 190, 70))


def scene_bat_full():
    return battery(1, (110, 210, 120))


def scene_bat_charging():
    bolt = poly([(17.5, 5), (9.5, 17), (15, 17), (13.5, 27), (22.5, 14), (17, 14)])
    return battery(1, (110, 210, 120)) + [(bolt, W)]


def port(color):
    shell = sub(rrect(4, 7, 28, 24, 2), rect(9, 7, 23, 14))
    pins = [rect(8 + i * 3.4, 17, 9.4 + i * 3.4, 22) for i in range(5)]
    return [(shell, color)] + [(p, (30, 34, 44)) for p in pins]


def scene_net_ethernet():
    return port(W)


def scene_net_disconnected():
    return port((130, 138, 154)) + [(line(5, 27, 27, 5, 2.4), (230, 90, 80))]


ICONS = {
    'utilities-terminal': scene_term, 'system-file-manager': scene_files, 'accessories-text-editor': scene_editor,
    'web-browser': scene_browser, 'multimedia-player': scene_player, 'preferences-system-time': scene_clock,
    'accessories-calculator': scene_calc, 'preferences-system-notifications': scene_notify,
    'preferences-system': scene_settings, 'help-about': scene_about, 'application-default': scene_default,
    'audio-volume-muted': scene_vol_muted, 'audio-volume-low': scene_vol_low, 'audio-volume-high': scene_vol_high,
    'battery-low': scene_bat_low, 'battery-medium': scene_bat_medium, 'battery-full': scene_bat_full,
    'battery-charging': scene_bat_charging, 'network-ethernet': scene_net_ethernet,
    'network-ethernet-disconnected': scene_net_disconnected,
}


def sample(scene, x, y):
    """Colour at (x, y): the last shape covering it wins; None if nothing does."""
    for shape, col in reversed(scene):
        if shape(x, y):
            return col(y) if callable(col) else col
    return None


def render(scene):
    px = []
    for j in range(N):
        for i in range(N):
            r = g = b = a = 0.0
            for sj in range(SS):
                for si in range(SS):
                    c = sample(scene, i + (si + 0.5) / SS, j + (sj + 0.5) / SS)
                    if c:
                        r, g, b, a = r + c[0], g + c[1], b + c[2], a + 1
            n = SS * SS
            if a:
                px.append((round(r / a), round(g / a), round(b / a), round(255 * a / n)))
            else:
                px.append((0, 0, 0, 0))
    return px


def png(px):
    raw = b''.join(b'\0' + b''.join(struct.pack('4B', *px[y * N + x]) for x in range(N)) for y in range(N))

    def chunk(t, d):
        c = struct.pack('>I', len(d)) + t + d
        return c + struct.pack('>I', zlib.crc32(t + d))
    return (b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', N, N, 8, 6, 0, 0, 0)) +
            chunk(b'IDAT', zlib.compress(raw, 9)) + chunk(b'IEND', b''))


if __name__ == '__main__':
    out = sys.argv[1]
    os.makedirs(out, exist_ok=True)
    for name, scene in ICONS.items():
        with open(os.path.join(out, name + '.png'), 'wb') as f:
            f.write(png(render(scene())))
