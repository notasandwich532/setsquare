# Fills through the API: paint-bucket areas, holes, recolouring, following the geometry.
from math import isclose, pi

sides = s.rectangle((0, 0), (40, 30))
hole = s.circle((20, 15), 5)

ring = s.fill(at=(5, 5), color="#ff7f27")
assert isclose(ring.area, 40 * 30 - pi * 25, rel_tol=0.01), ring.area
assert sorted(c.id for c in ring.boundary) == sorted(c.id for c in sides)
disc = s.fill(at=(20, 15), color="#00a2e8")
assert disc.boundary == [hole]
assert len(s.fills) == 2

# The same area again recolours rather than stacking another fill.
again = s.fill(at=(35, 25), color="#22b14c")
assert again == ring and len(s.fills) == 2 and ring.color == "#22b14c"
ring.color = "#a349a4"
assert ring.color == "#a349a4"

# Fills follow their outline.
s.move(sides + [hole], dx=100)
assert not ring.broken and not disc.broken
assert isclose(ring.area, 40 * 30 - pi * 25, rel_tol=0.01)

# An open outline: nothing to fill, and the loose ends say where.
doc.new()
a = s.line((0, 0), (20, 0))
b = s.line(a.p2, (20, 20))
c = s.line(b.p2, (0, 20))
assert len(s.loose_ends()) == 2
try:
    s.fill(at=(10, 10))
    raise AssertionError("filled an open outline")
except ValueError as e:
    assert "loose" in str(e), e
d = s.line(c.p2, a.p1)  # close it
f = s.fill(at=(10, 10))
assert isclose(f.area, 400)
s.delete(d)
assert f.broken and f.exists  # waits for its outline
s.delete(f)
assert not f.exists
print("ok")

# Two overlapping circles: three areas with the same two boundary curves (the crescents even have
# the same area), each filled separately and found again after a move.
doc.new()
a = s.circle((0, 0), 10)
b = s.circle((15, 0), 10)
left = s.fill(at=(-5, 0), color="#ff0000")
lens = s.fill(at=(7.5, 0), color="#00ff00")
right = s.fill(at=(20, 0), color="#0000ff")
assert len(s.fills) == 3
assert isclose(lens.area, 45.3, rel_tol=0.02) and isclose(left.area, pi * 100 - lens.area, rel_tol=0.01)
s.move([b], dx=2)
assert lens.area < 30 and left.area > 285 and right.area > 285
assert s.fill(at=(21, 0), color="#ffffff") == right and len(s.fills) == 3 and left.color == "#ff0000"

# Curves drawn on top of each other (a side drawn twice, as an old fillet bug left them) are one
# edge: the area they close still fills.
doc.new()
sq = s.rectangle((0, 0), (20, 20))
twice = s.line(sq[1].p2, sq[1].p1)            # the right side again, between the same points
f = s.fill(at=(10, 10), color="#ff0000")
assert isclose(f.area, 400, rel_tol=1e-6), f.area
part = s.line(sq[0].p1, (12, 0))               # the bottom side again, part way along
s.horizontal(part)
g = s.fill(at=(5, 5), color="#00ff00")
assert g == f and isclose(f.area, 400, rel_tol=1e-6), f.area
assert not s.loose_ends() or all(abs(y) < 1e-6 for _, y in s.loose_ends())
