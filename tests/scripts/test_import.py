# Importing SVG and DXF: shapes, units, transforms, styles, layers (each a group), fills, options,
# what's skipped, and recording.
import os
import re
import tempfile
from math import hypot, isclose, pi

folder = tempfile.mkdtemp()


def write(name, text):
    path = os.path.join(folder, name)
    with open(path, "w", encoding="utf-8") as f:
        f.write(text)
    return path


def near(a, b, tol=1e-6):
    return all(isclose(x, y, abs_tol=tol) for x, y in zip(a, b))


def kinds():
    out = {}
    for e in s.entities:
        k = type(e).__name__
        if k != "Point":
            out[k] = out.get(k, 0) + 1
    return out


def healthy():
    r = s.solve()
    assert r.ok and not r.redundant and not r.conflicting, (r.redundant, r.conflicting)
    return r


# --- SVG: a page in mm, Inkscape layers, CSS, transforms, every kind of shape -------------------------
svg = write("plan.svg", """<?xml version="1.0"?>
<svg xmlns="http://www.w3.org/2000/svg" xmlns:inkscape="http://www.inkscape.org/namespaces/inkscape"
     width="100mm" height="50mm" viewBox="0 0 200 100">
  <style>.red { stroke: #ff0000; fill: none; stroke-width: 2 } #disc { fill: rgb(0, 0, 255) }</style>
  <defs><linearGradient id="g"><stop offset="0" stop-color="#000"/><stop offset="1" stop-color="#fff"/></linearGradient></defs>
  <g inkscape:groupmode="layer" inkscape:label="Outline">
    <rect class="red" x="0" y="0" width="40" height="20"/>
    <rect class="red" x="50" y="0" width="40" height="20" rx="5"/>
    <path class="red" d="M 100 10 L 120 10 C 130 0 140 20 150 10 Q 160 0 170 10 A 10 10 0 0 1 190 10"/>
    <polyline class="red" points="0,40 10,50 20,40"/>
    <line class="red" x1="0" y1="90" x2="30" y2="90" stroke-dasharray="8 4"/>
    <text x="0" y="0">a label</text>
  </g>
  <g inkscape:groupmode="layer" inkscape:label="Paint">
    <circle id="disc" cx="40" cy="60" r="10"/>
    <ellipse cx="80" cy="60" rx="15" ry="5" fill="url(#g)" transform="rotate(30 80 60)"/>
    <path d="M 110 40 h 30 v 30 h -30 Z M 120 50 h 10 v 10 h -10 Z" fill="#00ff00" fill-rule="evenodd"/>
    <polygon points="160,40 190,40 175,70" fill="orange" stroke="black" stroke-width="0.5"/>
  </g>
</svg>""")
rep = s.import_file(svg)
assert rep["sublayers"] == ["Outline", "Paint"], rep
assert rep["groups"] == 2 and rep["skipped"] == {"texts": 1}, rep
assert "gradients came in as plain colours (their average)" in rep["notes"], rep
healthy()
k = kinds()
assert k.get("Circle") == 1 and k.get("Spline") == 2 and k.get("Ellipse") == 1, k  # (a C and a Q: exact)
assert k.get("Arc", 0) == 5 and "EllipseArc" not in k, k  # (the A, and the rounded rect's round corners)

# Sizes in mm (100 mm wide for 200 units: half), y up, the page's bottom-left on the origin.
disc = [c for c in s.circles][0]
assert near(disc.center.pos, (20, 50 - 30)) and isclose(disc.radius, 5), (disc.center.pos, disc.radius)
red = [l for l in s.lines if l.stroke and l.stroke["color"] == "#ff0000"]
assert red and isclose(red[0].stroke["width"], 1.0), red[0].stroke  # (2 units)
dashed = [l for l in s.lines if l.stroke and l.stroke["dash"] != "solid"]
assert len(dashed) == 1 and dashed[0].stroke["dash"] == "dashed"
# The square with a hole (even-odd): one fill, the ring; the disc's; the ellipse's; the triangle's.
fills = sorted(f.color for f in s.fills)
assert fills == sorted(["#0000ff", "#00ff00", "#808080", "#ffa500"]), fills
ring = [f for f in s.fills if f.color == "#00ff00"][0]
assert isclose(ring.area, 15 * 15 - 5 * 5, rel_tol=1e-6), ring.area
# A turned ellipse stays an ellipse (30 degrees, y flipped: -30).
e = [x for x in s.entities if type(x).__name__ == "Ellipse"][0]
ang = (e.major.pos[0] - e.center.pos[0], e.major.pos[1] - e.center.pos[1])
assert isclose(abs(abs(hypot(*ang)) - 7.5), 0, abs_tol=1e-6) or isclose(hypot(*ang), 2.5, abs_tol=1e-6)

# Each layer is a group: dragging one corner moves its whole layer only.
before_disc = disc.center.pos
corner = red[0].p1
x0, y0 = corner.pos
corner.move_to(x0 + 7, y0 + 3)
assert near(disc.center.pos, before_disc)  # (another layer's group)
assert near(red[1].p1.pos, (red[1].p1.pos[0], red[1].p1.pos[1]))
healthy()

# --- options: one sublayer, a new layer, centred, smooth curves ------------------------------------
doc.new()
rep = s.import_file(svg, into="new_layer", keep_layers=False, placement="center", at=(500, 500), curves="smooth")
assert [l.name for l in s.layers][0] == "plan" and rep["sublayers"] == ["plan"] and rep["groups"] == 1, rep
assert kinds().get("Spline") == 1, kinds()  # (the C and Q run on smoothly: one fit spline)
xs = [p.x for p in s.points]
ys = [p.y for p in s.points]
assert isclose((min(xs) + max(xs)) / 2, 500, abs_tol=1) and isclose((min(ys) + max(ys)) / 2, 500, abs_tol=1)

doc.new()
s.set_page("A4", orientation="landscape", origin="center")
s.import_file(svg, into="sublayer", placement="page")
xs = [p.x for p in s.points]
assert isclose(min(xs), -148.5, abs_tol=1e-6), min(xs)  # (the SVG's page on ours: left edges together)

# --- DXF: units, layers and colours, bulges, ellipses, splines, blocks ------------------------------
dxf = write("part.dxf", "\n".join(str(x) for x in [
    0, "SECTION", 2, "HEADER", 9, "$INSUNITS", 70, 1, 0, "ENDSEC",
    0, "SECTION", 2, "TABLES", 0, "TABLE", 2, "LAYER",
    0, "LAYER", 2, "Cut", 62, 1, 6, "CONTINUOUS", 370, 50,
    0, "LAYER", 2, "Guide", 62, 5, 6, "DASHED",
    0, "ENDTAB", 0, "ENDSEC",
    0, "SECTION", 2, "BLOCKS",
    0, "BLOCK", 2, "BOLT", 10, 0, 20, 0,
    0, "CIRCLE", 8, "0", 10, 0, 20, 0, 40, 0.25,
    0, "ENDBLK",
    0, "ENDSEC",
    0, "SECTION", 2, "ENTITIES",
    0, "LINE", 8, "Cut", 10, 0, 20, 0, 11, 4, 21, 0,
    0, "LWPOLYLINE", 8, "Cut", 90, 4, 70, 1, 10, 0, 20, 1, 10, 2, 20, 1, 42, 1, 10, 2, 20, 2, 10, 0, 20, 2,
    0, "ARC", 8, "Guide", 10, 6, 20, 0, 40, 1, 50, 0, 51, 90,
    0, "ELLIPSE", 8, "Guide", 10, 6, 20, 3, 11, 1, 21, 0, 40, 0.5, 41, 0, 42, 6.283185307179586,
    0, "SPLINE", 8, "Cut", 70, 8, 71, 3, 72, 8, 73, 4, 40, 0, 40, 0, 40, 0, 40, 0, 40, 1, 40, 1, 40, 1, 40, 1,
    10, 0, 20, 4, 10, 1, 20, 5, 10, 2, 20, 3, 10, 3, 20, 4,
    0, "INSERT", 8, "Cut", 2, "BOLT", 10, 8, 20, 0, 41, 2, 42, 2,
    0, "TEXT", 8, "Cut", 10, 0, 20, 0, 40, 1, 1, "hello",
    0, "HATCH", 8, "Cut",
    0, "ENDSEC", 0, "EOF"]) + "\n")
doc.new()
rep = s.import_file(dxf)
assert sorted(rep["sublayers"]) == ["Cut", "Guide"] and rep["groups"] == 2, rep
assert rep["skipped"] == {"hatches": 1, "texts": 1}, rep["skipped"]
healthy()
k = kinds()
assert k.get("Spline") == 1 and k.get("Ellipse") == 1 and k.get("Arc") == 2 and k.get("Circle") == 1, k
line = [l for l in s.lines if isclose(l.length, 4 * 25.4)]  # (inches)
assert line and line[0].stroke["color"] == "#ff0000" and isclose(line[0].stroke["width"], 0.5), line
guide = [a for a in s.arcs if isclose(a.radius, 25.4)][0]
assert guide.stroke["dash"] == "dashed" and guide.stroke["color"] == "#0000ff", guide.stroke
bulge = [a for a in s.arcs if isclose(a.radius, 0.5 * 25.4)]
assert bulge, [a.radius for a in s.arcs]  # (a half circle from a bulge of 1)
bolt = s.circles[0]
assert near(bolt.center.pos, (8 * 25.4, 0)) and isclose(bolt.radius, 0.5 * 25.4), (bolt.center.pos, bolt.radius)
# Units given: as chosen.
doc.new()
s.import_file(dxf, units="mm")
assert isclose(max(l.length for l in s.lines), 4)

# --- what it records makes it again ------------------------------------------------------------------
doc.new()
s.import_file(svg, into="new_layer", placement="center", at=(10, 20), curves="smooth")
lines = doc.history
assert lines[-1].startswith('s.import_file(r"') and 'into="new_layer"' in lines[-1] and 'at=(10, 20)' in lines[-1], lines[-1]
count = len(s.entities)
doc.new()
exec(lines[-1])
assert len(s.entities) == count

# --- not a drawing -----------------------------------------------------------------------------------
for bad in (write("x.svg", "not xml at all"), write("y.dxf", "hello\nworld\n"), write("z.txt", "")):
    try:
        s.import_file(bad)
        raise AssertionError("imported " + bad)
    except ValueError:
        pass
print("ok")
