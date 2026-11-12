# Fills go with the shapes they fill: dragged or moved (turned too) across other lines, a filled
# shape takes its fill along rather than leaving it behind; the area around a shape that moves
# stays the area around it.
from math import isclose, pi


def near(a, b, tol=1e-3):
    return isclose(a, b, abs_tol=tol)


box = s.rectangle((-30, -30), (90, 30))
wall = s.line((20, -30), (20, 30))  # (from side to side: it splits the box in two)
left, right = s.fill(at=(-25, 25), color="#cccccc"), None

# A filled circle dragged across the wall and on past it.
c = s.circle((0, 0), 5)
f = s.fill(at=(0, 0), color="#ff0000")
assert near(f.area, pi * 25, 0.1)
s.drag([c.center], 20, 0)  # (straddling the wall: one side of it is filled)
assert f.area < pi * 25 / 2 + 0.1, f.area
s.drag([c.center], 20, 0)
assert near(f.area, pi * 25, 0.1), f.area  # (all of it again: the fill came along)

# The area round it (the box's left part, with the circle a hole in it) didn't go anywhere.
before = left.area
assert near(before, 50 * 60, 0.5), before  # (the circle's gone from it now)

# Moved with the Move tool: a rectangle of lines, onto the wall and on past it.
r = s.rectangle((-20, -20), (-10, -10))
g = s.fill(at=(-15, -15), color="#00aa00")
assert near(g.area, 100)
s.move(r, dx=35, dy=0)  # (straddling: x from 15 to 25)
assert near(g.area, 50), g.area
s.move(r, dx=30, dy=0)
assert near(g.area, 100), g.area

# Turned too: a triangle (free to turn), from straddling the wall to past it.
a, b, c3 = s.point(16, 15), s.point(26, 15), s.point(21, 24)
tri = [s.line(a, b), s.line(b, c3), s.line(c3, a)]
t = s.fill(at=(17, 16), color="#0000ff")
assert t.area < 25, t.area  # (the part left of the wall)
s.move(tri, dx=20, dy=0, angle=40, about=(21, 18))
assert near(t.area, 45), t.area  # (all of it: 10 wide, 9 high)

print("ok")
