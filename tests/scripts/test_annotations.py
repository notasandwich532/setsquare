# Annotations (dimensions and notes drawn on the drawing) from Python: every kind, what they say,
# editing, the drawing's decimals, going with their geometry, and the recorder's calls replaying.
import re
from math import isclose, pi

sides = s.rectangle((0, 0), (40, 20))
hole = s.circle((10, 10), 4)
arc = s.arc((30, 10), (34, 10), (30, 14))  # (a quarter, radius 4)

w = s.annotate("horizontal", sides[0], at=(20, -8))
assert w.kind == "horizontal" and w.label == "40.00", w.label
assert isclose(w.at[0], 20) and isclose(w.at[1], -8), w.at
h = s.annotate("vertical", [sides[1].p1, sides[1].p2], at=(48, 10), text="<> TYP", decimals=1)
assert h.label == "20.0 TYP", h.label
d = s.annotate("diameter", hole, at=(-10, 20))
assert d.label == "Ø8.00", d.label
r = s.annotate("radius", arc)
assert r.label == "R4.00"
al = s.annotate("arc_length", arc)
assert al.label == "⌒%.2f" % (2 * pi), al.label
ang = s.annotate("angle", [sides[0], sides[1]])
assert ang.label == "90.00°", ang.label
ox = s.annotate("ordinate", sides[0].p2)  # (from the origin: x and y)
assert ox.label == "X40.00  Y0.00" and ox.value == (40, 0), (ox.label, ox.value)
oy = s.annotate("ordinate", sides[1].p2, origin=sides[0].p1, text="(<x>, <y>)")  # (from a corner)
assert oy.label == "(40.00, 20.00)", oy.label
assert oy.refs[1] == sides[0].p1
note = s.annotate("leader", hole, along=1.0, text="2 holes")
assert note.label == "2 holes" and note.value == 0
free = s.annotate("leader", tip=(60, 60), at=(70, 70), text="here")
assert free.label == "here" and free.refs == []
assert len(s.annotations) == 10
# (all in the layer's own sublayer for them; the active one is still where the geometry is)
assert {a.sublayer.name for a in s.annotations} == {"Annotations"}, {a.sublayer.name for a in s.annotations}
assert w.sublayer.annotations and not s.sublayer.annotations  # (s.sublayer: the active one)
assert sides[0].sublayer == s.sublayer

# Their look: black and 0.25 mm unless given; changed with edit_annotation.
assert w.color == "#000000" and isclose(w.width, 0.25) and isclose(w.text_height, 3.5)
s.edit_annotation(w, color="#ff0000", width=0.5, text_height=5, decimals=3)
assert w.color == "#ff0000" and isclose(w.width, 0.5) and isclose(w.text_height, 5) and w.label == "40.000"
s.edit_annotation(w, decimals=-1)
assert w.decimals is None and w.label == "40.00"

# The drawing's decimals: every one that doesn't say its own.
s.dimension_decimals = 0
assert w.label == "40" and h.label == "20.0 TYP"
s.dimension_decimals = 2

# They follow the geometry: drag a corner and the numbers change.
s.drag([sides[1].p1, sides[1].p2], 10, 0)
assert w.label == "50.00", w.label

# Refused: what that kind can't measure.
for bad in (lambda: s.annotate("arc_length", hole), lambda: s.annotate("radius", sides[0]),
            lambda: s.annotate("nonsense", sides[0])):
    try:
        bad()
        raise AssertionError("should have been refused")
    except ValueError:
        pass

# Deleting what one measures deletes it.
s.delete(hole)
assert not d.exists and not note.exists and w.exists

# --- exported: in the SVG, DXF and PDF unless asked not to -------------------------------------
import os
import tempfile

svg = doc.to_svg()
assert ">50.00</text>" in svg and "text-anchor=\"middle\"" in svg, svg[-2000:]
assert ">20.0 TYP</text>" in svg
assert "50.00" not in doc.to_svg(dimensions=False)
dxf = doc.to_dxf()
assert "DIMENSIONS" in dxf and "SOLID" in dxf
assert re.search(r"\n50\.00\r?\n", dxf)  # (a TEXT's words)
assert not re.search(r"\n50\.00\r?\n", doc.to_dxf(dimensions=False))
folder = tempfile.mkdtemp()
with_dims, without = os.path.join(folder, "with.pdf"), os.path.join(folder, "without.pdf")
doc.export_pdf(with_dims)
doc.export_pdf(without, dimensions=False)
assert os.path.getsize(with_dims) > os.path.getsize(without)  # (their lines and text)

# --- recorded: the history makes them again ------------------------------------------------------
lines = [l for l in doc.history if "annotat" in l or "dimension_decimals" in l]
assert any(l.startswith("n") and "= s.annotate(\"horizontal\"" in l for l in lines), lines
assert any("s.edit_annotation(" in l for l in lines), lines
before = sorted((a.kind, a.label, tuple(round(v, 6) for v in a.at)) for a in s.annotations)
history = doc.history
doc.new()


class Names(dict):  # as the console has them: p12, l7, n20 ... are the items with those ids
    def __missing__(self, key):
        m = re.match(r"^(sp|ea|[plcakdftein])(\d+)$", key)
        if not m:
            raise KeyError(key)
        return s[int(m.group(2))]


names = Names(s=s, doc=doc, setsquare=setsquare)
for line in history:
    exec(line, names)
after = sorted((a.kind, a.label, tuple(round(v, 6) for v in a.at)) for a in s.annotations)
assert after == before, (before, after)
print("ok")
