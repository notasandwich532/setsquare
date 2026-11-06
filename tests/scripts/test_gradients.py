# Gradients on fills, strokes and text: made, read back, saved, exported, following the geometry,
# and recorded as scripts that make them again.
import os
import re
import tempfile
from math import isclose

RED, BLUE = "#ff0000", "#0000ff"

# --- making them ----------------------------------------------------------------------------
g = drawapp.linear([RED, BLUE])
assert g.type == "linear" and g.stops == [(0.0, RED), (1.0, BLUE)], g.stops
assert g.start == (0.0, 0.5) and g.end == (1.0, 0.5) and g.spread == "pad"
assert g.color_at(0.5) == "#800080", g.color_at(0.5)
three = drawapp.linear([(0, RED), (0.25, "#00ff0080"), (1, BLUE)], start=(0, 0), end=(0, 1), spread="reflect")
assert three.stops[1] == (0.25, "#00ff0080") and three.spread == "reflect"
r = drawapp.radial([RED, BLUE], center=(0.5, 0.5), edge=(1, 0.5), focal=(0.6, 0.5), aspect=0.5, spread="repeat")
assert r.type == "radial" and r.focal == (0.6, 0.5) and r.aspect == 0.5
assert eval(repr(r)) == r and eval(repr(three)) == three  # repr is the call that makes it
for bad in (lambda: drawapp.linear([]), lambda: drawapp.linear([(2, RED)]),
            lambda: drawapp.linear([RED], spread="sideways"), lambda: drawapp.radial([RED], aspect=0)):
    try:
        bad()
        raise AssertionError("accepted a bad gradient")
    except ValueError:
        pass

# --- fills ------------------------------------------------------------------------------------
sides = s.rectangle((0, 0), (40, 20))
f = s.fill(at=(5, 5), color=g)
assert f.gradient == g, f.gradient
assert "gradient" in repr(f)
f.color = "#22b14c"  # a plain colour replaces it
assert f.gradient is None and f.color == "#22b14c"
f.gradient = r
assert f.gradient == r
f.gradient = None
assert f.gradient is None and f.color == "#22b14c"
f.gradient = g

svg = doc.to_svg(undecorated=False, margin=0)
# Drawing units, y down: left to right across the 40 x 20 box at mid-height.
m = re.search(r'<linearGradient id="(gradient\d+)" gradientUnits="userSpaceOnUse" x1="([^"]+)" y1="([^"]+)" '
              r'x2="([^"]+)" y2="([^"]+)"', svg)
assert m, svg[:800]
assert [float(v) for v in m.groups()[1:]] == [0, 10, 40, 10], m.groups()
assert f'fill="url(#{m.group(1)})"' in svg
assert '<stop offset="0" stop-color="#ff0000"/>' in svg and '<stop offset="1" stop-color="#0000ff"/>' in svg
assert svg.index("<defs>") < svg.index("<g ")  # defined before use

# It follows the geometry: wider box, wider gradient.
s.distance(sides[0], value=80)
s.solve()
m = re.search(r'<linearGradient[^>]* x1="([^"]+)" y1="[^"]+" x2="([^"]+)"', doc.to_svg(undecorated=False, margin=0))
assert [float(v) for v in m.groups()] == [0, 80], m.groups()

# Radial: a circle round the origin moved into place, turned and squashed, its focal point off
# the centre; and the spread.
f.gradient = drawapp.radial([RED, BLUE], center=(0.5, 0.5), edge=(0.5, 1), focal=(0.5, 0.6), aspect=2,
                            spread="reflect")
svg = doc.to_svg(undecorated=False, margin=0)
m = re.search(r'<radialGradient [^>]*>', svg)
assert m, svg[:800]
tag = m.group(0)
assert 'cx="0" cy="0" r="10"' in tag, tag  # half the 20 high box
assert 'fx="2" fy="0"' in tag, tag          # a tenth of the height along the first axis (up)
assert 'gradientTransform="translate(40 10) rotate(-90) scale(1 2)"' in tag, tag
assert 'spreadMethod="reflect"' in tag

# See-through stops keep their opacity.
f.gradient = drawapp.linear([RED, BLUE + "40"])
assert 'stop-color="#0000ff" stop-opacity="0.251"' in doc.to_svg(), doc.to_svg()[:900]

# --- strokes: a stroked outline shares one gradient across its box ---------------------------
s.stroke(sides, color=g, width=1)
assert sides[0].stroke["gradient"] == g and sides[0].stroke["width"] == 1
svg = doc.to_svg(undecorated=False, margin=0)
strokes = svg[svg.index('inkscape:label="Strokes"'):]
ids = re.findall(r'stroke="url\(#(gradient\d+)\)"', strokes)
assert len(ids) == 4, strokes[:600]
coords = {re.search(rf'<linearGradient id="{i}"[^>]*>', svg).group(0).split('gradientUnits="userSpaceOnUse"')[1]
          for i in ids}
assert len(coords) == 1, coords  # the same box for all four sides: the whole outline's (with the line's width)
assert 'x1="0" y1="10.5" x2="81" y2="10.5"' in coords.pop()
s.stroke(sides[0], color="#000000")  # a plain colour on one side takes it out of the group
assert "gradient" not in sides[0].stroke

# DXF: one colour for a line, so the gradient's average.
dxf = doc.to_dxf()
assert "STROKES" in dxf

# --- text ---------------------------------------------------------------------------------------
t = s.text("Hi", at=(0, 40), height=10, color=r)
assert t.gradient == r and t.color is not None
s.edit_text(t, color="#123456")
assert t.gradient is None and t.color == "#123456"
s.edit_text(t, color=three)
assert t.gradient == three
assert doc.to_svg().count("<linearGradient") >= 2

# --- saved and opened -------------------------------------------------------------------------
before = (f.gradient, sides[1].stroke, t.gradient)
with tempfile.TemporaryDirectory() as folder:
    path = os.path.join(folder, "g.dsk")
    doc.save(path)
    doc.open(path)
assert (f.gradient, sides[1].stroke, t.gradient) == before  # (by id: the same items)

# --- recorded: the history makes it again ---------------------------------------------------------
doc.new()
sides = s.rectangle((0, 0), (40, 20))
f = s.fill(at=(5, 5), color=r)
s.stroke(sides[:2], color=three, width=0.5)
f.gradient = g
lines = doc.history
assert any("drawapp.radial(" in l for l in lines) and any(".gradient = drawapp.linear(" in l for l in lines), lines
svg = doc.to_svg()


class Names(dict):  # as the console has them: p12, l7, f20 ... are the items with those ids
    def __missing__(self, key):
        m = re.match(r"^(sp|ea|[plcakdfte])(\d+)$", key)
        if not m:
            raise KeyError(key)
        return s[int(m.group(2))]


doc.new()
exec("\n".join(lines), globals(), Names())
assert doc.to_svg() == svg
print("ok")
