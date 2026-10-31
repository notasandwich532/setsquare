# Move / copy / scale / mirror, patterns, offsets and paste through the API.
from math import isclose, hypot


def clean(dof=None):
    r = s.solve()
    assert r.ok and not r.redundant and not r.conflicting, r
    if dof is not None:
        assert r.dof == dof, (r.dof, dof)


def near(p, x, y):
    return isclose(p.x, x, abs_tol=1e-6) and isclose(p.y, y, abs_tol=1e-6)


# --- move and copy ---------------------------------------------------------------------------
bottom, right, top, left = s.rectangle((10, 10), (30, 20))
width = s.distance(bottom, value=20)
clean(3)

s.move([bottom, right, top, left], dx=5, dy=-5)
assert near(bottom.p1, 15, 5)
assert isclose(bottom.length, 20)

# Turning: a free triangle (a horizontal/vertical-constrained shape couldn't turn).
a = s.line((0, 0), (10, 0))
b = s.line(a.p2, (0, 5))
s.distance(a, value=10)
s.move([a, b], angle=90, about=(0, 0))
assert near(a.p2, 0, 10), a.p2
assert near(b.p2, -5, 0), b.p2

copies = s.copy([bottom, right, top, left], dx=100)
assert len(copies) == 4 and copies[0].length > 0
assert isclose(copies[0].p1.x, bottom.p1.x + 100)
clean(6 + 5)  # the copy keeps its own horizontal/vertical and width; the triangle has 5
assert len(s.dimensions) == 3

# --- scale -----------------------------------------------------------------------------------
doc.new()
sides = s.rectangle((0, 0), (10, 5))
w = s.distance(sides[0], value=10)
c = s.circle((5, 2.5), 1)
s.scale(sides + [c], 3, about=(0, 0))
assert isclose(w.value, 30)
assert isclose(c.radius, 3)
assert near(sides[2].p1, 30, 15)
clean()

# --- mirror ----------------------------------------------------------------------------------
doc.new()
tri = [s.line((0, 0), (10, 0)), None, None]
tri[1] = s.line(tri[0].p2, (5, 8))
tri[2] = s.line(tri[1].p2, tri[0].p1)
before = s.dof
images = s.mirror(tri, s.y_axis)
assert len(images) == 3
assert near(images[0].p2, -10, 0)
assert s.dof == before
s.drag([tri[1].p2], 2, 1)
assert near(images[1].p2, -tri[1].p2.x, tri[1].p2.y)

# --- rectangular pattern ---------------------------------------------------------------------
doc.new()
hole = s.circle((0, 0), 2)
before = s.dof
k = s.rectangular_pattern([hole], count=4, spacing=10, count2=2, spacing2=15)
assert k.is_feature and k.type == "linearPattern"
assert len(k.copies) == 7
assert s.dof == before
last = k.copies[-1][0]
assert near(last.center, 30, 15)
k.value = 12  # spacing
assert near(last.center, 36, 15)
assert near(hole.center, 0, 0)  # the original stays put
k = s.set_pattern(k, count=2)
assert len(k.copies) == 3
clean(before)

# --- circular pattern ------------------------------------------------------------------------
doc.new()
spoke = s.line((5, 0), (20, 0))
k = s.circular_pattern([spoke], s.origin, count=4)
assert near(k.copies[0][0].p2, 0, 20)
k.value = 90
assert near(k.copies[2][0].p2, 0, 20)
clean(4)
s.delete(k)  # copies stay, unlinked
assert len(s.lines) == 4
clean(16)

# --- offset ----------------------------------------------------------------------------------
doc.new()
outline = s.slot((0, 0), (30, 0), 10)
before = s.dof
o = s.offset(outline[0], 2, toward=(15, 20))  # one curve: its whole chain
assert len(o.copies[0]) == 4
assert s.dof == before
o.value = 3
for src, off in zip(o.sources, o.copies[0]):
    if hasattr(src, "radius"):
        assert isclose(off.radius, src.radius + 3)
clean(before)

# --- extract and paste -----------------------------------------------------------------------
doc.new()
sq = s.rectangle((0, 0), (4, 4))
s.distance(sq[0], value=4)
data = s.extract(sq)
made = s.paste(data, at=(50, 50))  # centred there
lines = [m for m in made if isinstance(m, type(sq[0]))]
assert len(lines) == 4
xs = [p.x for l in lines for p in (l.p1, l.p2)]
assert isclose(min(xs), 48) and isclose(max(xs), 52)
clean(6)
print("ok")
