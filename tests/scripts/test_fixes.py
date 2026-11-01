# Regressions: midpoints that are already implied, and both-sides offsets.
from math import isclose

# A "W" whose four strokes are equal and whose top and bottom are level: it's symmetric, so its
# middle top point is already halfway along the top line. A midpoint there would say that twice;
# it becomes "on the line" instead, which still puts the point at the middle.
p = [s.point(x, y) for x, y in [(-202, 23), (-183, -78), (-137, 14), (-91, -78), (-71, 23)]]
strokes = [s.line(p[i], p[i + 1]) for i in range(4)]
for other in strokes[1:]:
    s.equal(strokes[0], other)
bottom = s.line(p[1], p[3])
bottom.construction = True
s.horizontal(bottom)
top = s.line(p[0], p[4])
top.construction = True
s.horizontal(top)

k = s.midpoint(p[2], top)
assert k.type == "coincident", k.type
mid_x = (top.p1.x + top.p2.x) / 2
assert isclose(p[2].x, mid_x, abs_tol=1e-6) and isclose(p[2].y, top.p1.y, abs_tol=1e-6)
assert not s.solve().redundant

# Offsets to both sides, capped: a slot round a polyline.
doc.new()
a = s.line((0, 0), (30, 0))
b = s.line(a.p2, (45, 20))
before = s.dof
o = s.offset([a, b], 4, both_sides=True, caps=True)
assert len(o.copies) == 2 and s.dof == before
assert len(s.arcs) == 2 and all(isclose(arc.radius, 4) for arc in s.arcs)
o.value = 6
assert all(isclose(arc.radius, 6) for arc in s.arcs)
r = s.solve()
assert r.ok and not r.redundant, r
try:
    s.offset([a], 2)  # one side needs to know which
    raise AssertionError("accepted an offset with no side")
except ValueError:
    pass
print("ok")
