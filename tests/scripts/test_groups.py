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
    path = os.path.join(folder, "group.dsk")
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
print("ok")
