# A filled shape deleted takes its fill with it: a new shape drawn in its place isn't coloured.
# A shape only broken (a side deleted) keeps its fill, which comes back when the side is drawn again.

# Deleted outright.
c = s.circle((0, 0), 5)
f = s.fill(at=(0, 0), color="#ff0000")
assert len(s.fills) == 1
s.delete(c)
assert len(s.fills) == 0, s.fills
again = s.circle((0, 0), 5)
assert len(s.fills) == 0, s.fills

# A rectangle's four sides deleted one at a time (each its own edit): gone once the last goes.
sides = s.rectangle((20, 0), (30, 10))
g = s.fill(at=(25, 5), color="#00ff00")
for side in sides[:3]:
    s.delete(side)
    assert len(s.fills) == 1
s.delete(sides[3])
assert len(s.fills) == 0, s.fills

# Broken, then mended: the fill comes back.
sides = s.rectangle((40, 0), (50, 10))
h = s.fill(at=(45, 5), color="#0000ff")
gap = (sides[0].p1.pos, sides[0].p2.pos)
s.delete(sides[0])
assert len(s.fills) == 1  # (kept, though there's no area to show)
s.line(gap[0], gap[1])
assert len(s.fills) == 1 and abs(h.area - 100) < 1e-6, h.area
print("ok")
