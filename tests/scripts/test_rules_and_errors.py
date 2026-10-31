# The API enforces the same rules as the UI, with Python exceptions.
from math import isclose


def raises(exc, fn, *args, **kwargs):
    try:
        fn(*args, **kwargs)
    except exc as e:
        return str(e)
    raise AssertionError(f"{fn.__name__} did not raise {exc.__name__}")


sides = s.rectangle(s.origin, (30, 20))
bottom, right, top, left = sides
assert s.dof == 2

# Over-constraining is refused, exactly like the constraint tools.
msg = raises(ValueError, s.horizontal, bottom)
assert "over-constrain" in msg, msg
assert len(s.constraints) == 4

# Bad references / types.
raises(ValueError, s.parallel, bottom, s.origin)
raises(TypeError, s.line, bottom, (1, 2))
raises(ValueError, s.radius, bottom)

# The origin is permanent.
raises(ValueError, s.delete, s.origin)

# A dimension can't be set to something unsolvable or invalid.
w = s.distance(bottom, value=30)
raises(ValueError, setattr, w, "value", -1)
assert isclose(w.value, 30)

# Deleted items say so instead of crashing.
c = s.circle((50, 50), 5)
s.delete(c)
assert not c.exists
raises(ValueError, getattr, c, "radius")

# The UI-only API is unavailable headless.
assert not app.has_ui
raises(RuntimeError, app.fit)
print("rules ok")
