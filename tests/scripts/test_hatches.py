# Hatched fills: the built-in patterns and ones of your own, made, read back, cut exactly to their
# areas (holes too) in the exports, following the geometry, saved, and recorded as scripts
# that make them again.
import os
import re
import tempfile
from math import cos, hypot, isclose, pi, sin

RED = "#ff0000"

# --- the patterns -------------------------------------------------------------------------------
names = [n for n, _ in drawapp.hatch_patterns()]
assert {"ANSI31", "ANSI37", "NET", "DOTS", "BRICK"} <= set(names), names
h = drawapp.hatch("ansi31", angle=15, scale=2, width=0.25)  # (any case)
assert h.pattern == "ANSI31" and h.angle == 15 and h.scale == 2 and h.width == 0.25
assert h.lines == [(45.0, 0.0, 0.0, 0.0, 3.175, [])], h.lines
assert eval(repr(h)) == h, repr(h)
own = drawapp.hatch("TILES", lines=[(0, 0, 0, 0, 5, [4, -1]), (90, 0, 0, 0, 5, [4, -1])], scale=0.5)
assert own.pattern == "TILES" and len(own.lines) == 2 and eval(repr(own)) == own, repr(own)
for bad in (lambda: drawapp.hatch("NO SUCH"),
            lambda: drawapp.hatch("ANSI31", scale=0),
            lambda: drawapp.hatch("ANSI31", width=-1),
            lambda: drawapp.hatch("X", lines=[(0, 0, 0, 0, 0)]),
            lambda: drawapp.hatch("X", lines=[])):
    try:
        bad()
        raise AssertionError("accepted a bad hatch")
    except (ValueError, TypeError):
        pass

# --- a hatched fill ---------------------------------------------------------------------------------
sides = s.rectangle((0, 0), (40, 20))
plain = drawapp.hatch("ANSI31")
f = s.fill(at=(5, 5), color=RED, hatch=plain)
assert f.hatch == plain and f.gradient is None and f.color == RED and "hatch" in repr(f)
f.hatch = None
assert f.hatch is None and f.color == RED
f.hatch = plain
# A gradient takes the hatch off, and a hatch the gradient.
f.gradient = drawapp.linear([RED, "#0000ff"], start=sides[0].p1, end=sides[0].p2)
assert f.hatch is None
f.hatch = plain
assert f.gradient is None


def segments(text, colour=RED):
    m = re.search(r'<path d="([^"]+)" fill="none" stroke="' + colour, text)
    assert m, text[:1500]
    out = []
    for piece in re.finditer(r"M([-\d.]+),([-\d.]+) (?:L([-\d.]+),([-\d.]+)|l0,0)", m.group(1)):
        a = (float(piece.group(1)), float(piece.group(2)))
        b = (float(piece.group(3)), float(piece.group(4))) if piece.group(3) else a
        out.append((a, b))
    return out


def svg():
    return doc.to_svg(undecorated=False, margin=0)


# Cut exactly to the area: 45 degree lines 3.175 apart, every end on the outline.
segs = segments(svg())
assert 12 <= len(segs) <= 15, len(segs)  # ((40 + 20) / sqrt 2 / 3.175: about 13)
for a, b in segs:
    for x, y in (a, b):
        assert -1e-3 <= x <= 40 + 1e-3 and -1e-3 <= y <= 20 + 1e-3, (x, y)
        assert min(abs(x), abs(x - 40), abs(y), abs(y - 20)) < 1e-3, (x, y)  # (on an edge)
    assert isclose(abs(b[0] - a[0]), abs(b[1] - a[1]), abs_tol=1e-3)  # 45 degrees
assert 'stroke-width="0.05"' in svg()  # (hairlines)

# A hole: none of it inside.
ring = s.circle((20, 10), 6)
f = s.fill(at=(2, 2), color=RED, hatch=plain)
for a, b in segments(svg()):
    for t in (0.25, 0.5, 0.75):
        x, y = a[0] + (b[0] - a[0]) * t, (a[1] + (b[1] - a[1]) * t)
        assert hypot(x - 20, y - 10) > 6 - 1e-3, (x, y)
    for x, y in (a, b):
        assert hypot(x - 20, y - 10) > 6 - 0.05, (x, y)  # (ends on the ring at most: its polygon)

# Dashes and dots, and the DXF: LINEs and POINTs on the HATCH layer.
f.hatch = drawapp.hatch("DOTS", scale=4)
dots = segments(svg())
assert dots and all(a == b for a, b in dots), dots[:5]
dxf = doc.to_dxf().replace("\r\n", "\n")
assert "\nHATCH\n" in dxf and dxf.count("\nPOINT\n") == len(dots), (dxf.count("\nPOINT\n"), len(dots))
f.hatch = drawapp.hatch("DASH", scale=2, width=0.3)
dashed = segments(svg())
lengths = [hypot(b[0] - a[0], b[1] - a[1]) for a, b in dashed]
assert all(l <= 6.35 + 1e-3 for l in lengths), lengths  # (dashes 6.35 long; cut short at the edges and hole)
assert sum(isclose(l, 6.35, abs_tol=1e-3) for l in lengths) > len(lengths) / 2, lengths
assert 'stroke-width="0.3"' in svg()
assert doc.to_dxf().replace("\r\n", "\n").count("\nLINE\n") >= len(dashed)

# It follows the geometry.
before = len(segments(svg()))
s.distance(sides[0], value=80)
s.solve()
assert len(segments(svg())) > before

# Too fine to lay out line by line: filled solid in the exports instead of failing.
f.hatch = drawapp.hatch("LINE", scale=1e-6)
assert 'fill="#ff0000"' in svg()

# --- saved and opened ----------------------------------------------------------------------------
f.hatch = own
with tempfile.TemporaryDirectory() as folder:
    path = os.path.join(folder, "h.dsk")
    doc.save(path)
    doc.open(path)
assert s.fills[-1].hatch == own, s.fills[-1].hatch

# --- recorded: the history makes it again --------------------------------------------------------
doc.new()
sides = s.rectangle((0, 0), (40, 20))
f = s.fill(at=(5, 5), color=RED, hatch=drawapp.hatch("ANSI37", angle=10, scale=1.5))
f.hatch = own
g = s.fill(at=(5, 5), color="#00ff00", hatch=drawapp.hatch("NET"))
lines = doc.history
assert any("hatch=drawapp.hatch(\"ANSI37\", angle=10, scale=1.5)" in l for l in lines), lines
assert any(".hatch = drawapp.hatch(\"TILES\", lines=[" in l for l in lines), lines
expected = svg()


class Names(dict):  # as the console has them: p12, l7, f20 ... are the items with those ids
    def __missing__(self, key):
        m = re.match(r"^(sp|ea|[plcakdfte])(\d+)$", key)
        if not m:
            raise KeyError(key)
        return s[int(m.group(2))]


doc.new()
exec("\n".join(lines), globals(), Names())
assert svg() == expected
print("ok")
