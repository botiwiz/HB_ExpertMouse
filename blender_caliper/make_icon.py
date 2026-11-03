"""Generate the toolbar icon (Blender triangle-icon .dat format): a digital caliper."""
import os
import struct

BLACK = (0, 0, 0, 255)
GRAY = (229, 229, 229, 255)
GREEN = (117, 255, 175, 255)

O = 7  # outline thickness

# (x0, y0, x1, y1, color), y up, 0..255
parts = [
    (18, 150, 238, 176, GRAY),     # beam
    (18, 40, 56, 176, GRAY),       # fixed jaw
    (100, 108, 190, 206, GRAY),    # sliding head
    (152, 40, 190, 108, GRAY),     # sliding jaw
]
details = [
    (110, 128, 180, 192, GREEN),   # display
] + [(x, 158, x + 4, 168, BLACK) for x in range(66, 96, 9)] + \
    [(x, 158, x + 4, 168, BLACK) for x in range(200, 232, 9)]


def quad(x0, y0, x1, y1, col):
    v = [(x0, y0), (x1, y0), (x1, y1), (x0, y0), (x1, y1), (x0, y1)]
    return v, [col] * 6


coords, colors = [], []
for x0, y0, x1, y1, _ in parts:  # outlines first
    v, c = quad(max(0, x0 - O), max(0, y0 - O), min(255, x1 + O), min(255, y1 + O), BLACK)
    coords += v
    colors += c
for p in parts + details:
    v, c = quad(*p)
    coords += v
    colors += c

out = os.path.join(os.path.dirname(os.path.abspath(__file__)), "hb_caliper", "icons", "hb_caliper.tool.dat")
with open(out, "wb") as f:
    f.write(b"VCO\x00" + bytes((255, 255, 0, 0)))
    f.write(b"".join(struct.pack("BB", x, y) for x, y in coords))
    f.write(b"".join(struct.pack("BBBB", *c) for c in colors))
print("Wrote", out, len(coords) // 3, "triangles")
