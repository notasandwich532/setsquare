# The M6 shape API: every variant builds, solves cleanly and has the expected freedom.
from math import isclose, hypot


def clean(dof):
    r = s.solve()
    assert r.ok and not r.redundant and not r.conflicting, r
    assert r.dof == dof, (r.dof, dof)


def fresh():
    doc.new()


fresh()
sides = s.rectangle_3point((0, 0), (30, 10), (20, 40))
assert len(sides) == 4
clean(5)

fresh()
s.rectangle_center((0, 0), (10, 5))
clean(4)

fresh()
c = s.circle_2point((0, 0), (10, 0))
assert isclose(c.radius, 5)
c3 = s.circle_3point((0, 10), (10, 0), (0, -10))
assert isclose(c3.radius, 10)

fresh()
a, b = s.line((0, 0), (40, 0)), s.line((0, 0), (0, 40))
a.fixed = b.fixed = True
t = s.circle_tangent([a, b], near=(10, 10), radius=5)
assert isclose(t.radius, 5) and isclose(t.center.x, 5) and isclose(t.center.y, 5)
clean(1)  # its size is still free

fresh()
base = s.line((0, 0), (20, 0))
arc = s.arc_tangent(base, base.p2, (30, 10))
assert isclose(arc.center.x, 20, abs_tol=1e-6) and isclose(arc.center.y, 10, abs_tol=1e-6)

for kind in ("inscribed", "circumscribed"):
    fresh()
    hexagon = s.polygon((0, 0), (10, 0), sides=6, circumscribed=(kind == "circumscribed"))
    assert len(hexagon) == 6
    assert all(isclose(side.length, hexagon[0].length) for side in hexagon)
    clean(4)

fresh()
tri = s.polygon_edge((0, 0), (10, 0), sides=3)
assert all(isclose(side.length, 10) for side in tri)
clean(4)

fresh()
outline = s.slot((0, 0), (30, 0), 8)
assert len(outline) == 4
clean(5)
fresh()
s.slot_overall((0, 0), (40, 0), 10)
clean(5)
fresh()
s.slot_center_point((0, 0), (15, 0), 6)
clean(5)
fresh()
arcs = s.arc_slot((0, 0), (20, 0), (0, 20), 4)
assert isclose(arcs[0].radius - arcs[2].radius, 4)
clean(6)
fresh()
s.arc_slot_3point((20, 0), (-20, 0), (0, 20), 4)
clean(6)

fresh()
l1, l2 = s.line((0, 0), (10, 0)), s.line((20, 3), (30, 5))
l1.fixed = True
s.collinear(l1, l2)
assert isclose(l2.p1.y, 0, abs_tol=1e-9) and isclose(l2.p2.y, 0, abs_tol=1e-9)
# Angle to the X axis (e.g. to set a polygon's rotation) and distance from the Y axis.
fresh()
hexagon = s.polygon((20, 0), (30, 0), sides=6, circumscribed=True)
s.angle(s.x_axis, hexagon[0], value=90)
s.distance(hexagon[0].p1, s.y_axis, value=25)
assert isclose(abs(hexagon[0].p1.x - hexagon[0].p2.x), 0, abs_tol=1e-6)  # side 0 now vertical

print("shapes ok")
