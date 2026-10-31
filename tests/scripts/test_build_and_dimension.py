# Build a fully constrained bracket entirely through the API.
from math import isclose

# An L-shaped outline anchored on the origin.
corners = [(0, 0), (60, 0), (60, 20), (20, 20), (20, 50), (0, 50)]
pts = [s.origin] + [s.point(x, y) for x, y in corners[1:]]
sides = [s.line(pts[i], pts[(i + 1) % len(pts)]) for i in range(len(pts))]

for side in sides:
    a, b = side.p1, side.p2
    if isclose(a.y, b.y):
        s.horizontal(side)
    else:
        s.vertical(side)
assert s.dof == 4, s.dof

s.distance(sides[0], value=60)
s.distance(sides[1], value=20)
s.distance(sides[2], value=40)
s.distance(sides[5], value=50)
assert s.dof == 0, s.dof
assert all(e.fully_constrained for e in s.entities)

# A hole, dimensioned from the edges.
hole = s.circle((10, 35), 4)
d = s.diameter(hole, value=8)
s.distance(hole.center, sides[5], value=10)
s.distance(hole.center, sides[4], value=15)
assert s.dof == 0
assert isclose(hole.radius, 4)

# Changing a dimension re-solves everything that depends on it.
d.value = 12
assert isclose(hole.radius, 6)
dims = s.dimensions
dims[0].value = 80
assert isclose(sides[0].length, 80), sides[0].length
assert isclose(pts[1].x, 80)
assert s.solve().ok
print("bracket ok:", len(s.entities), "items,", len(s.constraints), "constraints")
