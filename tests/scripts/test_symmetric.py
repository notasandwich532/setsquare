# Symmetry: points, lines, circles and arcs, including where the points are already level.
from math import isclose, hypot


def near(a, b, tol=1e-6):
    return isclose(a, b, abs_tol=tol)


def mirrored(p, q, axis):
    # p and q mirror images in the (vertical) axis line
    x = axis.p1.x
    return near(p.x + q.x, 2 * x) and near(p.y, q.y)


axis = s.line((0, -10), (0, 200))
s.vertical(axis)
s.coincident(axis.p1, s.y_axis)

# A rectangle's top corners: the top is already horizontal, so "square to the line" is implied.
# That used to be refused as over-constraining; now just the midpoint goes on the line.
bottom, right, top, left = s.rectangle((-20, 0), (25, 30))
tl, tr = sorted([top.p1, top.p2], key=lambda p: p.x)
dof = s.dof
s.symmetric(tl, tr, axis)
assert mirrored(tl, tr, axis), (tl.x, tr.x)
assert s.dof == dof - 1, (dof, s.dof)
# Said twice is still refused.
try:
    s.symmetric(tl, tr, axis)
    raise AssertionError("a second symmetric should be refused")
except ValueError as e:
    assert "over-constrain" in str(e), e

# Two free points: both parts apply.
p, q = s.point(-5, 40), s.point(7, 43)
dof = s.dof
s.symmetric(p, q, axis)
assert mirrored(p, q, axis) and s.dof == dof - 2

# Two lines: ends paired as they lie.
l1, l2 = s.line((-15, 50), (-5, 60)), s.line((6, 52), (14, 61))
dof = s.dof
s.symmetric(l1, l2, axis)
assert mirrored(l1.p1, l2.p1, axis) and mirrored(l1.p2, l2.p2, axis)
assert s.dof == dof - 4

# Lines drawn the other way round pair the other way.
l3, l4 = s.line((-15, 70), (-5, 75)), s.line((5, 76), (15, 71))
s.symmetric(l3, l4, axis)
assert mirrored(l3.p1, l4.p2, axis) and mirrored(l3.p2, l4.p1, axis)

# A V with its apex on the axis already.
v1 = s.line((0, 120), (-8, 130))
v2 = s.line(v1.p1, (9, 131))
s.coincident(v1.p1, axis)
s.symmetric(v1, v2, axis)
assert mirrored(v1.p2, v2.p2, axis)

# Circles: centres mirrored, radii equal.
c1, c2 = s.circle((-10, 80), 3), s.circle((9, 81), 5)
dof = s.dof
s.symmetric(c1, c2, axis)
assert mirrored(c1.center, c2.center, axis) and near(c1.radius, c2.radius)
assert s.dof == dof - 3

# Arcs: a mirror swaps an arc's ends.
a1 = s.arc((-10, 100), (-6, 100), (-10, 104))
a2 = s.arc((11, 101), (11, 105), (7, 101))
dof = s.dof
s.symmetric(a1, a2, axis)
assert mirrored(a1.center, a2.center, axis)
assert mirrored(a1.start, a2.end, axis) and mirrored(a1.end, a2.start, axis)
assert near(a1.radius, a2.radius)
assert s.dof == dof - 5

# Click order: the line can come anywhere for points/circles; for lines it's the last.
c3, c4 = s.circle((-30, 150), 2), s.circle((31, 152), 2)
s.symmetric(axis, c3, c4)
assert mirrored(c3.center, c4.center, axis)

# Dragging still works with the symmetric rectangle (the left side pulls the right one along).
s.drag([left.p1, left.p2], -5, 0)
assert mirrored(tl, tr, axis)
assert s.solve().ok
