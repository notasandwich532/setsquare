# Measuring from scripts: lengths, the distance between points (and dx, dy), areas.
from math import isclose, pi

sides = s.rectangle((0, 0), (20, 10))
hole = s.circle((5, 5), 2)

assert isclose(s.measure_length(sides[0]), 20, abs_tol=1e-9)
assert isclose(s.measure_length(sides), 60, abs_tol=1e-9)  # (a list: the total)
assert isclose(s.measure_length(hole), 4 * pi, abs_tol=1e-9)

# Angles between lines, as drawn: a corner's inside angle; a taper's narrow one (the lines meet
# beyond their ends); parallel lines, 0.
assert isclose(s.measure_angle(sides[0], sides[1]), 90)
left = s.line((40, 0), (44, -26))
right = s.line((72, -26), (76, 0))
bottom = s.line((44, -26), (72, -26))
assert isclose(s.measure_angle(left, right), 2 * 8.746162, rel_tol=1e-6), s.measure_angle(left, right)
assert isclose(s.measure_angle(left, bottom), 180 - 81.253838, rel_tol=1e-6), s.measure_angle(left, bottom)
assert s.measure_angle(sides[0], sides[2]) == 0

d, dx, dy = s.measure_between(sides[0].p1, (3, 4))
assert isclose(d, 5) and isclose(dx, 3) and isclose(dy, 4), (d, dx, dy)
d, dx, dy = s.measure_between(sides[0].p2, sides[0].p1)
assert isclose(d, 20) and isclose(dx, -20) and isclose(dy, 0, abs_tol=1e-9), (d, dx, dy)

assert isclose(s.measure_area((15, 5)), 200 - 4 * pi, rel_tol=1e-5), s.measure_area((15, 5))
assert isclose(s.measure_area((5, 5)), 4 * pi, rel_tol=1e-5)
assert s.measure_area((50, 50)) is None

try:
    s.measure_length(sides[0].p1)
    raise AssertionError("a point has no length")
except ValueError:
    pass
print("ok")
