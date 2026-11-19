# Turning a rectangle by other than a half turn drops its horizontal/vertical constraints, with a warning.
import warnings

r = s.rectangle((0, 0), (40, 20))
def level():
    return sum(1 for c in s.constraints if c.type in ("horizontal", "vertical"))

assert level() == 4, level()
with warnings.catch_warnings(record=True) as caught:
    warnings.simplefilter("always")
    s.move(r, angle=180)
    assert level() == 4 and not caught, caught
    s.move(r, angle=30)
assert level() == 0, level()
assert any("dropped 4 horizontal/vertical constraints" in str(w.message) for w in caught), [str(w.message) for w in caught]
print("ok")