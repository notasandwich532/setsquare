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
