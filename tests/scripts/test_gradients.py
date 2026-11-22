# Gradients on fills, strokes and text, placed by sketch geometry: made, read back, following the
# geometry, saved, exported, copied, and recorded as scripts that make them again.
import os
import re
import tempfile

RED, BLUE = "#ff0000", "#0000ff"

sides = s.rectangle((0, 0), (40, 20))
a, b = sides[0].p1, sides[1].p2  # (0, 0) and (40, 20)
ring = s.circle((20, 10), 8)
ring.construction = True
spot = s.point(16, 12)

# --- making them ----------------------------------------------------------------------------
g = setsquare.linear([RED, BLUE], start=a, end=b)
assert g.type == "linear" and g.stops == [(0.0, RED), (1.0, BLUE)], g.stops
assert g.start == a and g.end == b and g.shape is None and g.spread == "pad"
assert g.color_at(0.5) == "#800080", g.color_at(0.5)
r = setsquare.radial([RED, (0.25, "#00ff0080"), BLUE], shape=ring, focal=spot, spread="reflect")
assert r.type == "radial" and r.shape == ring and r.focal == spot and r.stops[1] == (0.25, "#00ff0080")
assert eval(repr(r).replace(f"c{ring.id}", "ring").replace(f"p{spot.id}", "spot")) == r
# Only sketch geometry places them: points for the ends and focal point, a round curve for radial.
for bad in (lambda: setsquare.linear([RED, BLUE], start=(0, 0), end=b),
            lambda: setsquare.linear([RED, BLUE], start=a, end=a),
            lambda: setsquare.linear([RED, BLUE], start=sides[0], end=b),
            lambda: setsquare.radial([RED, BLUE], shape=sides[0]),
            lambda: setsquare.radial([RED, BLUE], shape=ring, focal=ring),
            lambda: setsquare.linear([], start=a, end=b),
            lambda: setsquare.linear([(2, RED)], start=a, end=b),
            lambda: setsquare.linear([RED], start=a, end=b, spread="sideways")):
    try:
        bad()
        raise AssertionError("accepted a bad gradient")
    except (ValueError, TypeError):
        pass

# --- fills ------------------------------------------------------------------------------------
f = s.fill(at=(2, 2), color=g)
assert f.gradient == g, f.gradient
assert "gradient" in repr(f)
f.color = "#22b14c"  # a plain colour replaces it
assert f.gradient is None and f.color == "#22b14c"
f.gradient = r
assert f.gradient == r
f.gradient = None
assert f.gradient is None and f.color == "#22b14c"
f.gradient = g


def svg():
    return doc.to_svg(undecorated=False, margin=0)


def linear_coords(text):
    m = re.search(r'<linearGradient id="(gradient\d+)" gradientUnits="userSpaceOnUse" x1="([^"]+)" y1="([^"]+)" '
                  r'x2="([^"]+)" y2="([^"]+)"', text)
    assert m, text[:800]
    return m.group(1), [float(v) for v in m.groups()[1:]]


# In the SVG, from point to point (y down).
gid, coords = linear_coords(svg())
assert coords == [0, 20, 40, 0], coords
assert f'fill="url(#{gid})"' in svg()
assert '<stop offset="0" stop-color="#ff0000"/>' in svg() and '<stop offset="1" stop-color="#0000ff"/>' in svg()
assert svg().index("<defs>") < svg().index("<g ")  # defined before use

# It follows its points.
s.distance(sides[0], value=80)
s.solve()
assert linear_coords(svg())[1] == [0, 20, 80, 0], linear_coords(svg())

# Radial: round the circle's centre, out to it, the focal point off the centre, and the spread.
f.gradient = r
tag = re.search(r'<radialGradient [^>]*>', svg()).group(0)
assert 'cx="0" cy="0" r="8"' in tag, tag
assert 'fx="-4" fy="-2"' in tag, tag  # the focal point from the centre, y down
x = round(ring.center.x - a.x, 4)  # (the SVG starts at the drawing's left edge: a, after it grew both ways)
assert f'gradientTransform="translate({x:g} 10)"' in tag, (tag, x)
assert 'spreadMethod="reflect"' in tag
s.radius(ring, value=9)
s.solve()
assert 'r="9"' in svg()

# An ellipse makes it elliptical: turned and squashed to the ellipse.
oval = s.ellipse((60, 10), (66, 10), 3)
oval.construction = True
f.gradient = setsquare.radial([RED, BLUE], shape=oval)
tag = re.search(r'<radialGradient [^>]*>', svg()).group(0)
assert 'r="6"' in tag and "scale(1 0.5)" in tag and "fx=" not in tag, tag

# See-through stops keep their opacity.
f.gradient = setsquare.linear([RED, BLUE + "40"], start=a, end=b)
assert 'stop-color="#0000ff" stop-opacity="0.251"' in svg()

# Its geometry deleted: painted in its average colour; the next edit makes that its plain colour.
gone = s.point(5, 5)
f.gradient = setsquare.linear([RED, BLUE], start=a, end=gone)
saved = f.gradient
s.delete(gone)
assert "<linearGradient" not in svg() and 'fill="#800080"' in svg(), svg()[:900]
f.gradient = saved  # (any edit)
assert f.gradient is None and f.color == "#800080"

# Only geometry in the same layer.
base = s.layer
other = s.add_layer("Other")
far = s.point(1, 1)
s.layer = base
try:
    f.gradient = setsquare.linear([RED, BLUE], start=a, end=far)
    raise AssertionError("used another layer's point")
except ValueError as e:
    assert "layer" in str(e), e
s.delete_layer(other)

# --- strokes ----------------------------------------------------------------------------------------
s.stroke(sides, color=g, width=1)
assert sides[0].stroke["gradient"] == g and sides[0].stroke["width"] == 1
strokes = svg()
assert len(re.findall(r'stroke="url\(#gradient\d+\)"', strokes)) == 4, strokes[:600]
s.stroke(sides[0], color="#000000")  # a plain colour replaces it
assert "gradient" not in sides[0].stroke
assert re.search(r"\n *62\r?\n", doc.to_dxf())  # (DXF: the average colour, as an AutoCAD colour)

# --- text ---------------------------------------------------------------------------------------
t = s.text("Hi", at=(0, 40), height=10, color=r)
assert t.gradient == r and t.color is not None
s.edit_text(t, color="#123456")
assert t.gradient is None and t.color == "#123456"
s.edit_text(t, color=g)
assert t.gradient == g

# --- copies use the copied geometry where it was copied too --------------------------------------
s.stroke(sides, color=g, width=1)
copies = s.copy(sides, dy=-50)
copied = copies[1].stroke["gradient"]
assert copied.start not in (a, b) and copied.end not in (a, b), copied
assert abs(copied.start.y - (a.y - 50)) < 1e-9
partial = s.copy([ring], dx=100)  # the stroke's points weren't copied: still the originals
f.gradient = setsquare.radial([RED, BLUE], shape=ring)

# --- saved and opened -------------------------------------------------------------------------
before = (f.gradient, sides[1].stroke, t.gradient)
with tempfile.TemporaryDirectory() as folder:
    path = os.path.join(folder, "g.setsq")
    doc.save(path)
    doc.open(path)
assert (f.gradient, sides[1].stroke, t.gradient) == before  # (by id: the same items)

# --- recorded: the history makes it again ---------------------------------------------------------
doc.new()
sides = s.rectangle((0, 0), (40, 20))
ring = s.circle((20, 10), 8)
ring.construction = True
f = s.fill(at=(2, 2), color=setsquare.radial([RED, BLUE], shape=ring, focal=sides[0].p1))
s.stroke(sides[:2], color=setsquare.linear([RED, BLUE], start=sides[0].p1, end=sides[1].p2), width=0.5)
f.gradient = setsquare.linear([BLUE, RED], start=sides[0].p1, end=sides[0].p2, spread="repeat")
lines = doc.history
assert any("setsquare.radial(" in l and "shape=c" in l for l in lines), lines
assert any(".gradient = setsquare.linear(" in l for l in lines), lines
expected = svg()


class Names(dict):  # as the console has them: p12, l7, f20 ... are the items with those ids
    def __missing__(self, key):
        m = re.match(r"^(sp|ea|[plcakdfte])(\d+)$", key)
        if not m:
            raise KeyError(key)
        return s[int(m.group(2))]


doc.new()
exec("\n".join(lines), globals(), Names())
assert svg() == expected
print("ok")
