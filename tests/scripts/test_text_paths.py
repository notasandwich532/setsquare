# Text along a path: any curve (ellipses and splines too), and chains of curves joined end to end,
# put in order, followed as they change, saved, cut short when a curve of them goes, and recorded.
import os
import re
import tempfile
from math import hypot, isclose

# --- one curve: an ellipse ----------------------------------------------------------------------
oval = s.ellipse((0, 0), (30, 0), (0, 15))
t = s.text("Round the oval", path=oval, height=4, align="center")
assert t.path == oval and t.path_curves == [oval] and t.contours, t

# --- a chain: a line, a quarter arc round the corner, a line up (given out of order) ------------------
a = s.line((0, 40), (40, 40))
corner = s.arc((40, 50), a.p2, (50, 50))
up = s.line(corner.end, (50, 90))
along = s.text("Along the bottom, round the corner and up", path=[up, a, corner], height=3)
assert along.path_curves == [a, corner, up], along.path_curves  # (in order along the chain)


def extent(text):
    xs = [p[0] for c in text.contours for p in c]
    ys = [p[1] for c in text.contours for p in c]
    return min(xs), max(xs), min(ys), max(ys)


x0, x1, y0, y1 = extent(along)
assert x1 > 45 and y1 > 55, (x1, y1)  # (round the corner and up the side, not off the end of the line)
assert x0 >= -1 and y0 >= 39, (x0, y0)
# It follows the chain as it changes.
before = extent(along)
up.p2.move_to(50, 120)
s.solve()
s.distance(a, value=60)
s.solve()
assert extent(along) != before

# A closed chain (a triangle) goes round it.
tri = [s.line((100, 0), (140, 0))]
tri.append(s.line(tri[0].p2, (120, 30)))
tri.append(s.line(tri[1].p2, tri[0].p1))
round_ = s.text("Round and round the triangle", path=tri, height=2)
assert len(round_.path_curves) == 3

# Not a chain: refused.
for bad in ([a, tri[0]], [oval, a]):
    try:
        s.text("No", path=bad)
        raise AssertionError("accepted curves that aren't one chain")
    except ValueError as e:
        assert "chain" in str(e) or "closed" in str(e), e

# --- saved and opened -------------------------------------------------------------------------------
with tempfile.TemporaryDirectory() as folder:
    path = os.path.join(folder, "t.setsq")
    doc.save(path)
    doc.open(path)
along = [x for x in s.texts if x.text.startswith("Along")][0]
assert len(along.path_curves) == 3

# --- a curve of the chain deleted: cut short there; the first: the text goes ------------------------
a2, corner2, up2 = along.path_curves
s.delete(up2)
assert along.exists and along.path_curves == [a2, corner2], along.path_curves
s.delete(a2)
assert not along.exists

# --- recorded: the history makes it again -----------------------------------------------------------
doc.new()
l1 = s.line((0, 0), (30, 0))
l2 = s.line(l1.p2, (30, 30))
tx = s.text("Corner", path=[l1, l2], height=5)
lines = doc.history
assert any("path=[" in l for l in lines), lines
expected = [x.contours for x in s.texts]


class Names(dict):  # as the console has them: p12, l7, f20 ... are the items with those ids
    def __missing__(self, key):
        m = re.match(r"^(sp|ea|[plcakdfte])(\d+)$", key)
        if not m:
            raise KeyError(key)
        return s[int(m.group(2))]


doc.new()
exec("\n".join(lines), globals(), Names())
assert [x.contours for x in s.texts] == expected
print("ok")
