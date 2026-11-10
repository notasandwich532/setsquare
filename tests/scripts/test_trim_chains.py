# Trimming (to construction or not) again and again: pieces of circles and lines pile up joined
# end to end round a centre or along a line. However they're cut, the result must solve with
# nothing redundant, and every point that isn't fixed must still drag (redundant equations from
# the pieces once left drags unable to converge, so nothing moved).
import math, random
problems = 0
trims = 0
for seed in range(60):
    rnd = random.Random(seed)
    doc.new()
    for i in range(3):
        s.circle((rnd.uniform(-6, 6), rnd.uniform(-6, 6)), rnd.uniform(6, 10))
    for i in range(rnd.randint(1, 3)):
        s.line((rnd.uniform(-20, -10), rnd.uniform(-8, 8)), (rnd.uniform(10, 20), rnd.uniform(-8, 8)))
    for step in range(rnd.randint(3, 9)):
        solid = [c for c in s.circles + s.arcs + s.lines if not c.construction]
        if not solid:
            break
        c = rnd.choice(solid)
        if c in s.lines:
            u = rnd.uniform(0.05, 0.95)
            near = (c.p1.x + u * (c.p2.x - c.p1.x), c.p1.y + u * (c.p2.y - c.p1.y))
            try:
                s.trim(c, near=near, construction=rnd.random() < 0.8)
                trims += 1
            except ValueError:
                pass
            continue
        ang = rnd.uniform(0, 2 * math.pi)
        if c in s.arcs:
            a0 = math.atan2(c.start.y - c.center.y, c.start.x - c.center.x)
            a1 = math.atan2(c.end.y - c.center.y, c.end.x - c.center.x)
            sweep = (a1 - a0) % (2 * math.pi)
            ang = a0 + rnd.uniform(0.05, 0.95) * sweep
        near = (c.center.x + c.radius * math.cos(ang), c.center.y + c.radius * math.sin(ang))
        try:
            s.trim(c, near=near, construction=rnd.random() < 0.8)
            trims += 1
        except ValueError:
            continue
        r = s.solve()
        stuck = []
        for p in s.points:
            if p.fixed:
                continue
            x0, y0 = p.pos
            s.drag([p], 0.2, 0.1)
            if p.pos == (x0, y0):
                stuck.append(p.name)
        if not r.ok or r.redundant or r.conflicting or stuck:
            problems += 1
            print(f"seed {seed} step {step}: ok={r.ok} redundant={r.redundant} conflicting={r.conflicting} stuck={stuck}")
            break
assert problems == 0, f"{problems} sketches went wrong in {trims} trims"
assert trims > 200, trims

# Constraints the pieces make implied are skipped, not lost: break the chain and they apply again.
doc.new()
a = s.circle((0, 0), 10)
b = s.circle((8, 0), 10)
s.trim(b, near=(18, 0), construction=True)
s.trim(a, near=(-10, 0), construction=True)
s.delete([x for x in s.arcs if x.construction][0])
r = s.solve()
assert r.ok and not r.redundant, r.redundant

# A circle touching three sides of a box, trimmed between two of the touches: one side goes (the
# box being construction lines or not: construction cuts too, as in Fusion). The cut ends sit
# where the circle touched, kept there without anything said twice, and it all still drags.
for construction in (True, False):
    doc.new()
    s.rectangle((0, 0), (40, 30))
    sides = list(s.lines)
    for side in sides:
        side.construction = construction
    mid = lambda l: ((l.p1.x + l.p2.x) / 2, (l.p1.y + l.p2.y) / 2)
    left = min(sides, key=lambda l: mid(l)[0])
    bottom = min(sides, key=lambda l: mid(l)[1])
    top = max(sides, key=lambda l: mid(l)[1])
    c = s.circle_tangent([left, bottom, top], near=(15, 15))
    r = s.solve()
    assert r.ok and not r.redundant, r
    k = c.radius / math.sqrt(2)
    s.trim(c, near=(c.center.x - k, c.center.y - k))  # between the left and bottom touches
    assert len(s.arcs) == 1 and not s.circles, (construction, s.arcs, s.circles)
    arc = s.arcs[0]
    ends = sorted([(arc.start.x, arc.start.y), (arc.end.x, arc.end.y)])
    assert all(math.isclose(a, b, abs_tol=1e-6) for a, b in zip(ends[0] + ends[1], (0, 15, 15, 0))), (construction, ends)
    r = s.solve()
    assert r.ok and not r.redundant, (construction, r.redundant)
    # It still drags: the box's top right corner, and the circle grows with it.
    far = max(s.points, key=lambda p: p.x + p.y)
    s.drag([far], 0, 6)
    assert math.isclose(far.y, 36, abs_tol=1e-6), (construction, far.y)
    assert math.isclose(arc.radius, 18, abs_tol=1e-6), (construction, arc.radius)
    # (the box, free to, may have shifted along a bit) still touching where it's cut
    ends = sorted([(arc.start.x, arc.start.y), (arc.end.x, arc.end.y)])
    want = (left.p1.x, arc.center.y, arc.center.x, bottom.p1.y)
    assert all(math.isclose(a, b, abs_tol=1e-6) for a, b in zip(ends[0] + ends[1], want)), (construction, ends, want)

# A sketch with something said twice (redundant, as files made before that was caught can have):
# drags still move what's free, rather than nothing at all.
import json, os, tempfile
doc.new()
top = s.point(0, 20)
bottom = s.point(0, 0)
rail = s.line(top, bottom)
s.vertical(rail)
p = s.point(0, 9)
s.coincident(p, rail)
down = s.line(p, bottom)
q = s.point(10, 12)
s.line(p, q)
path = os.path.join(tempfile.mkdtemp(), "redundant.dsk")
doc.save(path)
with open(path, encoding="utf-8") as f:
    data = json.load(f)
data["constraints"].append({"id": 999, "refs": [down.id], "type": "vertical", "sublayer": data["constraints"][0]["sublayer"]})
with open(path, "w", encoding="utf-8") as f:
    json.dump(data, f)
doc.open(path)
r = s.solve()
assert r.redundant, "the file should have something said twice"
p = s[p.id]
s.drag([p], 0, 3)
assert math.isclose(p.y, 12, abs_tol=1e-6) and math.isclose(p.x, s[top.id].x, abs_tol=1e-6), (p.x, p.y)
