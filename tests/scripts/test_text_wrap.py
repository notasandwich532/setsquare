# Text in a box, wrapped to its width; the box as tall as the text if it grows (the side away from
# where it's aligned moving), refused if that side can't move; saved, and remade as the box changes.
import os
import tempfile

def box_text(text, wrap=True, grow=False, valign="top", width=40):
    doc.new()
    a = s.point(0, 0)
    b = s.point(width, 30)
    t = s.text(text, box=(a, b), height=3, font="Arial", wrap=wrap, grow=grow, valign=valign)
    return t, a, b

words = "the quick brown fox jumps over the lazy dog again and again"

# Not wrapped: one long line, wider than the box.
t, a, b = box_text(words, wrap=False)
assert not t.wrap and not t.grow
assert t.bounds[2] > 40.5

# Wrapped: every line within the box.
t, a, b = box_text(words)
assert t.wrap
assert t.bounds[2] <= 40.0 + 1e-6, t.bounds[2]

# Growing (top aligned): the bottom moves down, the top stays.
t, a, b = box_text(words, grow=True)
corners = [p for p in s.points]
top_y = max(p.y for p in corners)
bottom_y = min(p.y for p in corners)
assert abs(top_y - 30) < 1e-6, top_y
assert bottom_y < 0 or bottom_y > 0  # moved from 0
tall = top_y - bottom_y
# Narrower: more lines, taller still.
s.drag([b], -15, 0)
corners = [p for p in s.points]
taller = max(p.y for p in corners) - min(p.y for p in corners)
assert taller > tall + 1, (tall, taller)

# Bottom aligned: the top moves instead.
t, a, b = box_text(words, grow=True, valign="bottom")
assert abs(min(p.y for p in s.points)) < 1e-6
assert max(p.y for p in s.points) != 30

# The side that'd move held: refused, and nothing changes.
t, a, b = box_text(words, grow=False)
bottom = [p for p in s.points if abs(p.y) < 1e-9]
for p in bottom:
    p.fixed = True
try:
    s.edit_text(t, grow=True)
    raise AssertionError("should be refused: the bottom side is fixed")
except ValueError as e:
    assert "bottom side" in str(e), e
assert not s.texts[0].grow

# grow without wrap, or outside a box: refused.
for bad in (lambda: s.edit_text(t, wrap=False, grow=True), lambda: s.text("x", at=(0, 50), wrap=True)):
    try:
        bad()
        raise AssertionError("should be refused")
    except ValueError:
        pass

# Saved and read back.
t, a, b = box_text(words, grow=True)
path = os.path.join(tempfile.mkdtemp(), "wrap.setsq")
doc.save(path)
doc.open(path)
t = s.texts[0]
assert t.wrap and t.grow
print("wrap ok")
