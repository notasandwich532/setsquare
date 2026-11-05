# Ellipses and splines: made from points, solved, constrained, filled, trimmed, exported, saved.
import math, os, random, tempfile
from math import isclose


def healthy(label):
    """Solves with nothing redundant, and every point that isn't fixed still drags."""
    r = s.solve()
    assert r.ok and not r.redundant and not r.conflicting, (label, r.redundant, r.conflicting)
    for p in s.points:
        if p.fixed:
            continue
        x0, y0 = p.pos
        s.drag([p], 0.3, 0.2)
        assert p.pos != (x0, y0), (label, "stuck", p)


# --- ellipses ------------------------------------------------------------------------------
e = s.ellipse((0, 0), (20, 0), 10)
assert (e.major_radius, e.minor_radius, e.angle) == (20, 10, 0)
assert s.solve().dof == 5  # centre 2, major point 2, minor point 2, less the right angle
s.drag([e.major], 0, 10)    # turning it: the minor axis keeps square
d = (e.major.x - e.center.x) * (e.minor.x - e.center.x) + (e.major.y - e.center.y) * (e.minor.y - e.center.y)
assert abs(d) < 1e-6, d
r = s.radius(e, value=30)
assert isclose(e.major_radius, 30)
p = s.point(32, 12)
s.coincident(p, e)
f = e.center
local = ((p.x - f.x) * math.cos(math.radians(e.angle)) + (p.y - f.y) * math.sin(math.radians(e.angle)),
         -(p.x - f.x) * math.sin(math.radians(e.angle)) + (p.y - f.y) * math.cos(math.radians(e.angle)))
assert isclose((local[0] / e.major_radius) ** 2 + (local[1] / e.minor_radius) ** 2, 1, abs_tol=1e-6)
healthy("ellipse")

# Fills: a whole ellipse, and half of one cut by a line.
doc.new()
e = s.ellipse((0, 0), (20, 0), 10)
whole = s.fill(at=(0, 5), color="#ff0000")
assert isclose(whole.area, math.pi * 200, rel_tol=0.005), whole.area
s.line((-25, 0), (25, 0))
half = s.fill(at=(0, 5), color="#00ff00")
assert isclose(half.area, math.pi * 100, rel_tol=0.005), half.area

# Tangent to a line.
doc.new()
e = s.ellipse((0, 0), (20, 0), 10)
l = s.line((-30, 15), (30, 15))
s.horizontal(l)
s.tangent(l, e)
assert isclose(l.p1.y - e.center.y, e.minor_radius, rel_tol=1e-6)
healthy("tangent")

# Trimming leaves an elliptical arc; breaking, two that make the whole; to construction, the rest.
doc.new()
e = s.ellipse((0, 0), (20, 0), 10)
s.line((-30, 3), (30, 3))
s.trim(e, near=(0, 10))
arcs = [x for x in s.entities if type(x).__name__ == "EllipseArc"]
assert len(arcs) == 1 and arcs[0].start.y < 3.1 and arcs[0].end.y < 3.1
healthy("trimmed")
doc.new()
e = s.ellipse((0, 0), (20, 0), 10)
s.line((-30, 3), (30, 3))
s.trim(e, near=(0, 10), construction=True)
arcs = [x for x in s.entities if type(x).__name__ == "EllipseArc"]
assert sorted(a.construction for a in arcs) == [False, True]
healthy("trimmed to construction")

# --- splines -------------------------------------------------------------------------------
doc.new()
fit = s.spline([(0, 0), (10, 10), (20, 0), (30, 10)])
ctl = s.spline([(0, 20), (10, 30), (20, 20), (30, 30)], kind="control")
ring = s.spline([(50, 0), (60, 10), (70, 0), (60, -10)], closed=True)
assert (fit.kind, ctl.kind, ring.closed) == ("fit", "control", True)
assert s.solve().dof == 24  # just their points
healthy("splines")
# A fit spline goes through its points; a control one through its ends only.
svg = doc.to_svg()
assert " C" in svg  # cubic Beziers
area = s.fill(at=(60, 0), color="#0000ff").area
assert 150 < area < 400, area
q = s.point(5, 50)
s.coincident(q, fit)
healthy("point on spline")

# Joined smoothly to a line at its end.
doc.new()
l = s.line((-20, 0), (0, 0))
s.horizontal(l)
sp = s.spline([l.p2, (10, 5), (20, 0)])
s.tangent(l, sp)
r = s.solve()
assert r.ok and not r.redundant
healthy("tangent at the end")

# Trimming leaves a piece of the same curve; a closed one opens.
doc.new()
sp = s.spline([(0, 0), (10, 10), (20, 0), (30, 10)], kind="control")
s.line((15, -10), (15, 20))
s.trim(sp, near=(25, 5))
assert sp.start is not None and sp.end is not None
healthy("trimmed spline")
doc.new()
ring = s.spline([(50, 0), (60, 10), (70, 0), (60, -10)], closed=True)
s.line((55, -20), (55, 20))
s.trim(ring, near=(51, 0), construction=True)
pieces = [x for x in s.entities if type(x).__name__ == "Spline"]
assert len(pieces) == 2 and all(x.start is not None for x in pieces)
healthy("closed spline trimmed")

# --- copies, mirrors, patterns, saving --------------------------------------------------------
doc.new()
e = s.ellipse((0, 0), (20, 0), 10)
sp = s.spline([(30, 0), (40, 10), (50, 0)])
s.copy([e, sp], dy=40)
s.mirror([e, sp], s.y_axis)
healthy("copied and mirrored")
doc.new()
e = s.ellipse((0, 0), (20, 0), 10)
s.line((-25, 3), (25, 3))
s.trim(e, near=(0, 10))
arcs = [x for x in s.entities if type(x).__name__ == "EllipseArc"]
s.rectangular_pattern(arcs, count=3, spacing=50)
healthy("patterned")
path = os.path.join(tempfile.gettempdir(), "drawapp_curves.dsk")
doc.save(path)
doc.open(path)
assert sorted(type(x).__name__ for x in s.entities if type(x).__name__ not in ("Point", "Line")) == ["EllipseArc"] * 3
healthy("reloaded")
os.remove(path)
assert "POLYLINE" in doc.to_dxf()

# --- trimming again and again (random) ---------------------------------------------------------
trims = 0
for seed in range(25):
    rnd = random.Random(seed)
    doc.new()
    s.ellipse((rnd.uniform(-5, 5), rnd.uniform(-5, 5)), (rnd.uniform(10, 16), rnd.uniform(-4, 4)), rnd.uniform(5, 9))
    s.spline([(rnd.uniform(-20, -10), rnd.uniform(-10, 10)), (rnd.uniform(-5, 5), rnd.uniform(-12, 12)),
              (rnd.uniform(10, 20), rnd.uniform(-10, 10))], kind=rnd.choice(["fit", "control"]))
    s.line((rnd.uniform(-20, -15), rnd.uniform(-6, 6)), (rnd.uniform(15, 20), rnd.uniform(-6, 6)))
    for step in range(4):
        curves = [c for c in s.entities if type(c).__name__ in ("Ellipse", "EllipseArc", "Spline", "Line")
                  and not c.construction]
        if not curves:
            break
        c = rnd.choice(curves)
        pts = [p.pos for p in s.points]
        near = rnd.choice(pts) if rnd.random() < 0.3 else (rnd.uniform(-15, 15), rnd.uniform(-10, 10))
        try:
            s.trim(c, near=near, construction=rnd.random() < 0.7)
            trims += 1
        except ValueError:
            continue
        healthy(f"seed {seed} step {step}")
assert trims > 30, trims
