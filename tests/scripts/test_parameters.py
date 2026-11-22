# Parameters from Python: named values that dimensions follow.
r = s.rectangle((0, 0), (100, 50))
w = s.distance(r[0], value=100)
h = s.distance(r[1], value=50)

assert s.parameter("width", 120) == 120
assert s.parameter("height", "width / 2") == 60
w.value = "width"
h.value = "height - 10"
assert (w.expression, h.expression) == ("width", "height - 10")
assert abs(r[0].length - 120) < 1e-6 and abs(r[1].length - 50) < 1e-6

s.parameter("width", 200)                       # everything using it follows
assert s.parameter("height") == 100             # (read)
assert abs(r[1].length - 90) < 1e-6

s.rename_parameter("width", "W")
assert w.expression == "W" and [p.expression for p in s.parameters] == ["200", "W / 2"]

for bad, says in [(lambda: s.parameter("a", "b"), "don't know 'b'"),
                  (lambda: s.parameter("W", "height"), "loop"),
                  (lambda: s.delete_parameter("W"), "used by"),
                  (lambda: s.parameter("sqrt", 2), "already means")]:
    try:
        bad()
        raise AssertionError("should have been refused: " + says)
    except ValueError as e:
        assert says in str(e), e

w.value = 150                                   # a number: a plain value again
assert w.expression is None
print("ok")
# Patterns follow parameters too.
s.parameter("holes", 3)
dot = s.circle((0, 100), 1)
k = s.rectangular_pattern([dot], count="holes", spacing="W / 40")
assert len([c for c in s.circles if c.radius == 1]) == 3
s.parameter("holes", 5)
assert len([c for c in s.circles if c.radius == 1]) == 5
try:
    s.parameter("holes", 4.5)
    raise AssertionError("a count of 4.5 should be refused")
except ValueError as e:
    assert "whole" in str(e), e
print("ok, patterns")
