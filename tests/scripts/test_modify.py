# Trim / extend / break / fillet / chamfer through the API.
from math import isclose


def clean(dof=None):
    r = s.solve()
    assert r.ok and not r.redundant and not r.conflicting, r
    if dof is not None:
        assert r.dof == dof, (r.dof, dof)


# A plate with every corner treated, anchored on the origin.
bottom, right, top, left = s.rectangle(s.origin, (60, 40))
s.distance(bottom, value=60)
s.distance(left, value=40)
clean(0)

r1 = s.fillet(bottom, right, 5)                # shared corner: no `near` needed
c1 = s.chamfer(top, left, 4)                   # the opposite corner
clean(0)
assert isclose(r1.radius, 5)
assert isclose(c1.length, (4 ** 2 * 2) ** 0.5)
assert all(side.construction for side in (bottom, right, top, left))  # the originals
c2 = None

# Resize a fillet through its dimension.
radius_dim = [d for d in s.dimensions if d.type == "radius" and r1 in d.refs][0]
radius_dim.value = 7
assert isclose(r1.radius, 7)
clean(0)

# Trim / extend / break on simple lines.
doc.new()
h = s.line((0, 10), (50, 10))
v1 = s.line((20, 0), (20, 20))
v2 = s.line((70, 0), (70, 20))
s.trim(h, near=(40, 10))
assert isclose(h.p2.x, 20)
s.extend(h, near=(19, 10))
assert isclose(h.p2.x, 70)
pieces = s.break_curve(h, near=(10, 10))
assert len(pieces) == 2
clean()

# Trim to construction: the piece stays, dashed.
doc.new()
h = s.line((0, 10), (50, 10))
s.line((20, 0), (20, 20))
before = len(s.lines)
s.trim(h, near=(40, 10), construction=True)
assert len(s.lines) == before + 1
assert sum(1 for l in s.lines if l.construction) == 1
clean()

# Fillet two crossing lines needs to know which quadrant.
doc.new()
a = s.line((-20, 0), (20, 0))
b = s.line((0, -20), (0, 20))
arc = s.fillet(a, b, 3, near=((10, 0), (0, -10)))
assert isclose(arc.center.x, 3) and isclose(arc.center.y, -3)
print("modify ok")
