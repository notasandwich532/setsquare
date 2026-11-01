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
