# Groups: geometry held rigidly together, free to move and turn as one unless something else
# holds it; constraints inside carried by the group; saved, copied, recorded, ungrouped.
import os
import tempfile
from math import hypot, isclose


def dist(a, b):
    return hypot(a.x - b.x, a.y - b.y)


def distances(points):
    return [dist(a, b) for i, a in enumerate(points) for b in points[i + 1:]]


def same(xs, ys, tol=1e-6):
    return all(isclose(x, y, abs_tol=tol) for x, y in zip(xs, ys))


def healthy():
    r = s.solve()
    assert r.ok and not r.redundant and not r.conflicting, (r.redundant, r.conflicting)
    return r


# --- a triangle (no other constraints): moves and turns as one ------------------------------------
a, b, c = s.point(0, 0), s.point(30, 0), s.point(10, 20)
tri = [s.line(a, b), s.line(b, c), s.line(c, a)]
other = s.line((50, 0), (60, 10))
before = distances([a, b, c])
g = s.group(tri)
assert g.type == "group", g
healthy()
assert s.solve().dof == 3 + 4, s.solve().dof  # the group (move, turn) and the loose line

c.move_to(14, 31)  # dragged by a corner: the whole triangle comes, the same shape
assert same(distances([a, b, c]), before), (distances([a, b, c]), before)
assert (a.x, a.y) != (0, 0)
assert other.p1.pos == (50, 0)  # not in the group: untouched

# Held at a point (here, on the origin), it turns about it.
s.coincident(a, s.origin)
healthy()
c.move_to(-5, 25)
assert same(distances([a, b, c]), before) and same(a.pos, (0, 0))
assert s.solve().dof == 1 + 4
s.horizontal(tri[0])  # and something setting its direction stops it turning
r = healthy()
assert r.dof == 4, r.dof  # (just the loose line)
assert isclose(a.y, b.y, abs_tol=1e-9)

# Dimensions inside a group are held by it.
d = s.distance(tri[1])
try:
    d.value = d.value + 5
    raise AssertionError("changed a dimension inside a group")
except ValueError as e:
    assert "group" in str(e), e

# Ungrouped: free again.
s.delete(g)
healthy()
b.move_to(40, 0)
assert not isclose(dist(a, b), before[0])

# --- a rectangle with its own H/V: grouped, nothing redundant; it slides but doesn't turn -----------
doc.new()
rect = s.rectangle((0, 0), (20, 10))
ring = s.circle((40, 5), 4)
corners = [l.p1 for l in rect]
g = s.group(rect + [ring])
healthy()
corner = corners[2]
assert same([corners[0].x, corners[0].y], [0, 0])
x0, y0 = corner.pos
corner.move_to(x0 + 7, y0 - 3)
assert same([rect[0].p1.y], [rect[0].p2.y])  # (still level)
assert isclose(ring.radius, 4)
assert same([ring.center.x - corners[0].x, ring.center.y - corners[0].y], [40, 5])  # (came along)
moved = [p.pos for p in corners]
assert moved != [(0, 0), (20, 0), (20, 10), (0, 10)]

# Two fixed points: the whole group stays put (and that's no problem for the solver).
rect[0].p1.fixed = True
rect[0].p2.fixed = True
healthy()
corner.move_to(100, 100)
assert same(corner.pos, moved[2])

# --- making one -------------------------------------------------------------------------------------------
for bad in (lambda: s.group([s.point(1, 1)]), lambda: s.group([])):
    try:
        bad()
        raise AssertionError("grouped too little")
    except ValueError:
        pass

# --- saved, copied, recorded ---------------------------------------------------------------------------
doc.new()
p = [s.point(0, 0), s.point(30, 0), s.point(10, 20)]
tri = [s.line(p[0], p[1]), s.line(p[1], p[2]), s.line(p[2], p[0])]
g = s.group(tri)
lines = doc.history
assert lines[-1].startswith(f"k{g.id} = s.group([") or lines[-1].startswith(f"d{g.id} = s.group(["), lines[-1]
with tempfile.TemporaryDirectory() as folder:
    path = os.path.join(folder, "group.setsq")
    doc.save(path)
    doc.open(path)
assert any(k.type == "group" for k in s.constraints)
healthy()

copies = s.copy(tri, dx=50)
groups = [k for k in s.constraints if k.type == "group"]
assert len(groups) == 2, groups
healthy()
cp = [l.p1 for l in copies]
before = distances(cp)
cp[0].move_to(70, 30)
assert same(distances(cp), before)
assert same([v for q in p for v in q.pos], [0, 0, 30, 0, 10, 20])  # the original group stays

# --- curves with rules of their own: arcs on a circle (a planet), a lone arc, ellipses, splines ------
doc.new()
planet = s.circle((0, 0), 50)
p1, p2 = s.point(30, 40), s.point(48, 14)
ring = s.arc((60, 30), p2, p1)
s.coincident(p1, planet)
s.coincident(p2, planet)
moon = s.circle((-60, 10), 8)
s.vertical(s.line(planet.center, (0, -70)))
g = s.group([planet, ring, moon] + [l for l in s.lines])
healthy()
pts = [planet.center, ring.center, p1, p2, moon.center]
before, radius = distances(pts), planet.radius
planet.center.move_to(12, 7)  # the whole planet slides (the vertical line stops it turning)
assert same(distances(pts), before), (distances(pts), before)
assert isclose(moon.radius, 8) and isclose(planet.radius, radius, rel_tol=1e-9)
assert same(planet.center.pos, (12, 7))

doc.new()
q1, q2 = s.point(30, 40), s.point(48, 14)
lone = s.arc((60, 30), q2, q1)
s.group([lone])  # (its own rules and the group's don't say the same thing twice)
healthy()
before = distances([lone.center, q1, q2])
q1.move_to(20, 50)
assert same(distances([lone.center, q1, q2]), before)

doc.new()
e = s.ellipse((0, 0), (20, 5), 8)
spl = s.spline([(30, 0), (40, 8), (50, -3), (60, 5)], "control")
s.group([e, spl])
healthy()
pts = list(spl.points) + [e.center, e.major, e.minor]
before = distances(pts)
spl.points[1].move_to(45, 20)
assert same(distances(pts), before)

# Held by something fixed: a grouped spline doesn't move at all.
doc.new()
spl = s.spline([(0, 0), (10, 8), (20, -3), (30, 5)])
base = s.line((0, -20), (30, -20))
base.p1.fixed = base.p2.fixed = True
s.group([spl, base])
r = healthy()
assert r.dof == 0, r.dof
before = [p.pos for p in spl.points]
spl.points[2].move_to(25, 10)
assert [p.pos for p in spl.points] == before

# --- groups don't chain: grouping with something in a group adds to that group ----------------------
doc.new()
a1 = s.line((0, 0), (10, 0))
a2 = s.line((20, 0), (30, 5))
a3 = s.line((40, 0), (50, 5))
g1 = s.group([a1, a2])
g2 = s.group([a2, a3])  # a2 is in g1 already: one group of all three
groups = [k for k in s.constraints if k.type == "group"]
assert len(groups) == 1 and not g1.exists, groups
a4 = s.line((60, 0), (70, 5))
g3 = s.group([g2, a4])  # a group's symbol among the items: add to it
groups = [k for k in s.constraints if k.type == "group"]
assert len(groups) == 1 and not g2.exists and g3.exists
assert doc.history[-1] == f"k{g3.id} = s.group([k{g2.id}, l{a4.id}])", doc.history[-1]
healthy()
pts = [a1.p1, a2.p2, a3.p1, a4.p2]
before = distances(pts)
a4.p2.move_to(80, 20)
assert same(distances(pts), before)
print("ok")