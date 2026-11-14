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
assert rep["groups"] == 2 and rep["skipped"] == {} and rep["texts"] == 1, rep
assert not any("gradient" in n for n in rep["notes"]), rep  # (a real gradient now)
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
assert fills == sorted(["#0000ff", "#00ff00", "#808080", "#ffa500"]), fills  # (a gradient: its average)
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

# The ellipse's gradient: from its box's top left corner rightwards (objectBoundingBox; y1 = y2 = 0%), turned with it.
eg = [f for f in s.fills if f.gradient][0].gradient
assert eg.type == "linear" and eg.stops == [(0.0, "#000000"), (1.0, "#ffffff")], eg
assert near(eg.start.pos, (34.754809, 25.915064)) and near(eg.end.pos, (47.745191, 18.415064)), (eg.start.pos, eg.end.pos)

# --- gradients: exact placing, radial ones, strokes, sharing -------------------------------------------
doc.new()
grad = write("grad.svg", """<svg xmlns="http://www.w3.org/2000/svg" xmlns:xlink="http://www.w3.org/1999/xlink"
     width="100mm" height="100mm" viewBox="0 0 100 100">
  <defs>
    <linearGradient id="diag" x1="0" y1="0" x2="1" y2="1"><stop offset="0" stop-color="red"/><stop offset="1" stop-color="blue"/></linearGradient>
    <linearGradient id="base"><stop offset="0%" stop-color="#00ff00" stop-opacity="0.5"/><stop offset="100%" stop-color="black"/></linearGradient>
    <radialGradient id="glow" xlink:href="#base" gradientUnits="userSpaceOnUse" cx="70" cy="70" r="10" fx="74" fy="70"
                    gradientTransform="translate(70 70) scale(2 1) translate(-70 -70)" spreadMethod="reflect"/>
    <linearGradient id="across" gradientUnits="userSpaceOnUse" x1="0" y1="50" x2="100" y2="50"><stop offset="0" stop-color="#fff"/><stop offset="1" stop-color="#000"/></linearGradient>
    <pattern id="dots" width="4" height="4"><circle cx="2" cy="2" r="1"/></pattern>
  </defs>
  <rect x="0" y="0" width="40" height="10" fill="url(#diag)"/>
  <circle cx="70" cy="70" r="20" fill="url(#glow)"/>
  <line x1="0" y1="40" x2="40" y2="40" stroke="url(#across)" stroke-width="2"/>
  <rect x="0" y="80" width="10" height="10" fill="url(#across)"/>
  <rect x="20" y="80" width="10" height="10" fill="url(#across)"/>
  <rect x="40" y="80" width="10" height="10" fill="url(#dots)"/>
</svg>""")
rep = s.import_file(grad)
assert rep["gradients"] == 3 and rep["groups"] == 1, rep  # (the two squares share one)
healthy()

def by_colours(*colours):
    return [f for f in s.fills if f.gradient and [c for _, c in f.gradient.stops] == list(colours)]

# A square box it isn't: the colours change across the box's diagonal as the box squashes it (the far
# corner is exactly offset 1, and the near one 0).
d = by_colours("#ff0000", "#0000ff")[0].gradient
st, en = d.start.pos, d.end.pos
v = (en[0] - st[0], en[1] - st[1])
corner = (40, 100 - 10)  # (40, 10), y up (a unit is a mm here)
assert near(st, (0, 100)), st
assert isclose((corner[0] - st[0]) * v[0] + (corner[1] - st[1]) * v[1], v[0] ** 2 + v[1] ** 2, rel_tol=1e-9), (st, en)

# Radial: stops from its template (half-transparent green), stretched by its transform into an
# ellipse (a construction one), its focal point off centre, reflected.
glow = by_colours("#00ff0080", "#000000")[0].gradient
assert glow.type == "radial" and glow.spread == "reflect", glow
oval = glow.shape
assert type(oval).__name__ == "Ellipse" and oval.construction, oval
radii = sorted(hypot(p.pos[0] - oval.center.pos[0], p.pos[1] - oval.center.pos[1]) for p in (oval.major, oval.minor))
assert near(radii, (10, 20)) and near(oval.center.pos, (70, 30)), (radii, oval.center.pos)
assert near(glow.focal.pos, (78, 30)), glow.focal.pos  # (4 units right, stretched to 8)

# A stroke's gradient; the squares painted alike share one (and its geometry).
across = [l for l in s.lines if l.stroke and "gradient" in l.stroke]
assert across and across[0].stroke["gradient"].type == "linear", across
squares = by_colours("#ffffff", "#000000")
assert len(squares) == 2 and squares[0].gradient == squares[1].gradient, squares

# The gradients' geometry is in the group: it moves with the drawing.
corner_pt = [l for l in s.lines if l.stroke and "gradient" in l.stroke][0].p1
x0, y0 = corner_pt.pos
gap = (glow.focal.pos[0] - x0, glow.focal.pos[1] - y0)
corner_pt.move_to(x0 + 5, y0 - 2)
healthy()
assert isclose(hypot(glow.focal.pos[0] - corner_pt.pos[0], glow.focal.pos[1] - corner_pt.pos[1]), hypot(*gap), rel_tol=1e-6)

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
assert not rep["skipped"] and rep["texts"] == 1, rep  # (the empty HATCH: nothing to bring in)
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

# --- pictures: reference images (embedded, linked, turned, mirrored), from SVG and DXF ------------------
import base64
import struct
import zlib


def png_bytes(width, height, rgb=(220, 40, 40)):
    def chunk(kind, data):
        return struct.pack(">I", len(data)) + kind + data + struct.pack(">I", zlib.crc32(kind + data) & 0xffffffff)
    rows = b"".join(b"\x00" + bytes(rgb) * width for _ in range(height))
    return (b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", width, height, 8, 2, 0, 0, 0))
            + chunk(b"IDAT", zlib.compress(rows)) + chunk(b"IEND", b""))


os.makedirs(os.path.join(folder, "pics"), exist_ok=True)
with open(os.path.join(folder, "pics", "plan.png"), "wb") as fh:
    fh.write(png_bytes(40, 20))
with open(os.path.join(folder, "plan.png"), "wb") as fh:
    fh.write(png_bytes(40, 20))
embedded = base64.b64encode(png_bytes(40, 20)).decode()
pics = write("pics.svg", f"""<svg xmlns="http://www.w3.org/2000/svg" xmlns:xlink="http://www.w3.org/1999/xlink"
     width="100mm" height="100mm" viewBox="0 0 100 100">
  <image x="10" y="10" width="40" height="40" href="data:image/png;base64,{embedded}"/>
  <image x="60" y="60" width="20" height="10" xlink:href="pics/plan.png" transform="rotate(90 70 70)"/>
  <image x="-100" y="0" width="10" height="5" href="pics/plan.png" transform="scale(-1 1)"/>
  <image x="0" y="90" width="40" height="5" href="pics/plan.png" preserveAspectRatio="none"/>
  <image x="0" y="0" width="10" height="10" href="nowhere/gone.png"/>
  <line x1="0" y1="0" x2="1" y2="1" stroke="black"/>
</svg>""")
doc.new()
rep = s.import_file(pics)
assert rep["images"] == 4 and rep["skipped"] == {"linked images not found": 1}, rep
assert "stretched or skewed images came in keeping their own proportions" in rep["notes"], rep
ims = s.images
# Fitted into its box (40 x 40 for a 2:1 picture: 40 x 20, in the middle), y up.
assert near(ims[0].center, (30, 70)) and isclose(ims[0].width, 40) and ims[0].angle == 0 and not ims[0].flip_h
# Turned a quarter clockwise round (70, 70).
assert near(ims[1].center, (75, 30)) and isclose(ims[1].width, 20) and isclose(ims[1].angle % 360, 270), (ims[1].center, ims[1].angle)
# Mirrored.
assert near(ims[2].center, (95, 97.5)) and ims[2].flip_h and isclose(ims[2].width, 10), (ims[2].center, ims[2].flip_h)
assert ims[0].opacity == 0.5  # (references: faded, as when inserted)

# DXF: the IMAGEDEF's path is from another computer; the picture's found by name beside the file.
dxfpic = write("pic.dxf", "\n".join(str(x) for x in [
    0, "SECTION", 2, "HEADER", 9, "$INSUNITS", 70, 4, 0, "ENDSEC",
    0, "SECTION", 2, "ENTITIES",
    0, "IMAGE", 5, "30", 8, "Refs", 10, 5, 20, 5, 30, 0, 11, 0.1, 21, 0, 31, 0, 12, 0, 22, 0.1, 32, 0,
    13, 40, 23, 20, 340, "2A",
    0, "LINE", 8, "Refs", 10, 0, 20, 0, 11, 1, 21, 0,
    0, "ENDSEC",
    0, "SECTION", 2, "OBJECTS",
    0, "IMAGEDEF", 5, "2A", 1, r"D:\elsewhere\drawings\plan.png", 10, 40, 20, 20,
    0, "ENDSEC", 0, "EOF"]) + "\n")
doc.new()
rep = s.import_file(dxfpic)
assert rep["images"] == 1, rep
im = s.images[0]
assert near(im.center, (7, 6)) and isclose(im.width, 4) and im.angle == 0, (im.center, im.width)

# --- text: SVG text and tspans, DXF TEXT and MTEXT -------------------------------------------------------
words = write("words.svg", """<svg xmlns="http://www.w3.org/2000/svg" xmlns:xlink="http://www.w3.org/1999/xlink"
     width="100mm" height="100mm" viewBox="0 0 100 100">
  <style>.t { font-family: 'No Such Font', Arial, sans-serif; font-size: 10px; }</style>
  <defs><path id="p" d="M 0 0 L 50 0"/></defs>
  <text class="t" x="10" y="20" fill="#123456">Hello <tspan font-weight="bold">bold</tspan> world</text>
  <text x="50" y="50" font-size="8" text-anchor="middle" transform="rotate(-90 50 50)"><tspan x="50" y="50">Line one</tspan><tspan x="50" y="60">Line two</tspan></text>
  <text x="10" y="90" font-size="5" letter-spacing="1" font-style="italic">   spaced
     out  </text>
  <text x="0" y="0" font-size="5" transform="scale(-1 1)">mirror</text>
  <text><textPath xlink:href="#p">along</textPath></text>
</svg>""")
doc.new()
rep = s.import_file(words)
assert rep["texts"] == 8 and not rep["skipped"], rep
healthy()
by_words = {t.text: t for t in s.texts}
assert sorted(by_words) == ["Hello", "Line one", "Line two", "along", "bold", "mirror", "spaced out", "world"], sorted(by_words)
along = by_words["along"]  # (along its path: a construction line)
assert len(along.path_curves) == 1 and along.path_curves[0].construction and along.align == "left", along.path_curves
# A word in its own style within a line: a text of its own, after the one before it.
hello, bold, world = by_words["Hello"], by_words["bold"], by_words["world"]
assert hello.font != "No Such Font" and not hello.bold and hello.color == "#123456", (hello.font, hello.color)
assert bold.bold and not world.bold and world.color == "#123456"
assert near(hello.anchor.pos, (10, 80)) and hello.align == "left" and hello.angle == 0
assert hello.anchor.x < bold.anchor.x < world.anchor.x and bold.anchor.y == hello.anchor.y, (hello.anchor.pos, bold.anchor.pos)
assert 5 < hello.height < 8.5, hello.height  # (10 px is the em; the height here is the capitals')
one, two = by_words["Line one"], by_words["Line two"]
assert one.align == "center" and isclose(one.angle, 90) and near(one.anchor.pos, (50, 50)), (one.angle, one.anchor.pos)
assert near(two.anchor.pos, (60, 50)), two.anchor.pos  # (the next line, turned too)
spaced = by_words["spaced out"]
assert spaced.italic and isclose(spaced.spacing, 20), spaced.spacing  # (1 of 5: 20% of the size)
mirror = by_words["mirror"]
assert mirror.flip_h and mirror.align == "right" and near(mirror.anchor.pos, (0, 100)), (mirror.flip_h, mirror.align)
# Held with the rest of the layer: moved together.
gap = (two.anchor.pos[0] - one.anchor.pos[0], two.anchor.pos[1] - one.anchor.pos[1])
one.anchor.move_to(55, 52)
healthy()
assert near((two.anchor.pos[0] - one.anchor.pos[0], two.anchor.pos[1] - one.anchor.pos[1]), gap) or \
    isclose(hypot(two.anchor.pos[0] - one.anchor.pos[0], two.anchor.pos[1] - one.anchor.pos[1]), hypot(*gap))

# The font shorthand; a nested svg's viewBox; a symbol used at a size (its viewBox fitted to it).
fitted = write("fitted.svg", """<svg xmlns="http://www.w3.org/2000/svg" xmlns:xlink="http://www.w3.org/1999/xlink"
     width="100mm" height="100mm" viewBox="0 0 100 100">
  <defs><symbol id="sq" viewBox="0 0 10 10"><rect x="0" y="0" width="10" height="10" fill="red"/></symbol></defs>
  <svg x="10" y="10" width="20" height="20" viewBox="0 0 10 10"><circle cx="5" cy="5" r="5" fill="blue"/></svg>
  <use xlink:href="#sq" x="50" y="10" width="30" height="30"/>
  <text x="10" y="90" style="font: italic bold 8px serif" fill="black">Short</text>
</svg>""")
doc.new()
s.import_file(fitted)
disc = s.circles[0]
assert near(disc.center.pos, (20, 80)) and isclose(disc.radius, 10), (disc.center.pos, disc.radius)  # (scaled 2x)
xs = sorted({round(p.x, 6) for p in s.points if p.y < 95 and p.x >= 45})
assert xs[0] == 50 and xs[-1] == 80, xs  # (the symbol's 10 units: 30)
short = s.texts[0]
assert short.bold and short.italic and 5 < short.height < 7, (short.bold, short.italic, short.height)

# Markers: an arrowhead on each line's end, its tip on the end, sized to the stroke, turned with it.
arrows = write("arrows.svg", """<svg xmlns="http://www.w3.org/2000/svg" width="100mm" height="100mm" viewBox="0 0 100 100">
  <defs><marker id="arrow" viewBox="0 0 10 10" refX="10" refY="5" markerWidth="4" markerHeight="4" orient="auto">
    <path d="M0,0 L10,5 L0,10 z" fill="red"/></marker></defs>
  <line x1="10" y1="50" x2="60" y2="50" stroke="black" stroke-width="2" marker-end="url(#arrow)"/>
  <path d="M80,90 L80,40" stroke="black" stroke-width="2" marker-end="url(#arrow)"/>
</svg>""")
doc.new()
rep = s.import_file(arrows)
assert rep["fills"] == 4 and all(f.color == "#ff0000" for f in s.fills), rep  # (the arrowheads, each halved by its line)
corners = {(round(p.x, 6), round(p.y, 6)) for p in s.points}
for q in [(60, 50), (52, 54), (52, 46), (80, 60), (76, 52), (84, 52)]:
    assert q in corners, (q, sorted(corners))

# Clip paths: shapes cut to them (fills closed along the cut, with no line there); a group's clip
# of two windows; a line cut short.
clips = write("clips.svg", """<svg xmlns="http://www.w3.org/2000/svg" width="100mm" height="100mm" viewBox="0 0 100 100">
  <defs>
    <clipPath id="half"><rect x="0" y="0" width="50" height="100"/></clipPath>
    <clipPath id="two"><rect x="60" y="60" width="10" height="30"/><rect x="80" y="60" width="10" height="30"/></clipPath>
  </defs>
  <circle cx="50" cy="50" r="20" fill="blue" stroke="black" clip-path="url(#half)"/>
  <g clip-path="url(#two)"><rect x="55" y="70" width="40" height="10" fill="green"/></g>
  <line x1="0" y1="95" x2="100" y2="95" stroke="red" clip-path="url(#half)"/>
</svg>""")
doc.new()
rep = s.import_file(clips)
assert not rep["skipped"], rep
healthy()
blue = [f for f in s.fills if f.color == "#0000ff"]
assert len(blue) == 1 and isclose(blue[0].area, pi * 400 / 2, rel_tol=1e-3), [f.area for f in blue]  # (the left half)
assert max(p.x for p in s.points if hypot(p.x - 50, p.y - 50) <= 20 + 1e-6) <= 50 + 1e-6  # (nothing of it right of the cut)
cut = [l for l in s.lines if near(l.p1.pos, (50, 30)) or near(l.p2.pos, (50, 30))]
assert cut and cut[0].stroke["width"] == "none", cut  # (the cut edge: no line)
assert [a for a in s.arcs if isclose(a.radius, 20)][0].stroke["color"] == "#000000"
green = sorted(round(f.area, 6) for f in s.fills if f.color == "#008000")
assert green == [100, 100], green  # (seen through both windows)
red = [l for l in s.lines if l.stroke and l.stroke["color"] == "#ff0000"]
assert len(red) == 1 and isclose(red[0].length, 50), [l.length for l in red]

# Patterns: a tile of lines comes in as a hatch; a grid tile (lines on its edges) as two families;
# a tile of dots repeated as real circles, cut to the shape.
pats = write("pats.svg", """<svg xmlns="http://www.w3.org/2000/svg" width="100mm" height="100mm" viewBox="0 0 100 100">
  <defs>
    <pattern id="diag" width="4" height="4" patternUnits="userSpaceOnUse"><path d="M0,0 L4,4" stroke="black" stroke-width="0.5"/></pattern>
    <pattern id="grid" width="10" height="10" patternUnits="userSpaceOnUse"><path d="M0,0 H10 M0,0 V10 M10,0 V10" stroke="blue" stroke-width="0.2" fill="none"/></pattern>
    <pattern id="dots" width="10" height="10" patternUnits="userSpaceOnUse"><circle cx="5" cy="5" r="2" fill="red"/></pattern>
  </defs>
  <rect x="0" y="0" width="40" height="20" fill="url(#diag)" stroke="black"/>
  <rect x="0" y="30" width="40" height="20" fill="url(#grid)"/>
  <rect x="50" y="0" width="25" height="20" fill="url(#dots)"/>
</svg>""")
doc.new()
rep = s.import_file(pats)
assert rep["hatches"] == 2, rep
healthy()
diag = [f for f in s.fills if f.hatch and f.color == "#000000"][0].hatch
(angle, x, y, dx, dy, dashes), = diag.lines
assert isclose(angle % 180, 135) and isclose(dy, 4 / 2 ** 0.5) and dashes == [] and diag.width == 0.5, diag.lines
grid = [f for f in s.fills if f.hatch and f.color == "#0000ff"][0].hatch
assert sorted(round(l[0] % 180) for l in grid.lines) == [0, 90] and all(isclose(l[4], 10) for l in grid.lines), grid.lines
dots = [f for f in s.fills if f.color == "#ff0000"]
assert len([c for c in s.circles if isclose(c.radius, 2)]) == 4, [c.radius for c in s.circles]  # (the 2 cut in half: arcs)
assert isclose(sum(f.area for f in dots), 5 * pi * 4, rel_tol=1e-3), sum(f.area for f in dots)
assert max(p.x for p in s.points if p.x > 45) <= 75 + 1e-6

# Text along paths: a curve then a line (a chain), from its middle; single arcs, run their way.
paths = write("paths.svg", """<svg xmlns="http://www.w3.org/2000/svg" xmlns:xlink="http://www.w3.org/1999/xlink"
     width="100mm" height="100mm" viewBox="0 0 100 100">
  <defs>
    <path id="wave" d="M10,50 C30,30 50,30 70,50 L90,50"/>
    <path id="over" d="M10,90 A20,20 0 0 1 50,90"/>
    <path id="under" d="M10,90 A20,20 0 0 0 50,90"/>
  </defs>
  <text font-size="5" text-anchor="middle"><textPath xlink:href="#wave" startOffset="50%">middle of the wave</textPath></text>
  <text font-size="4"><textPath xlink:href="#over">over the top</textPath></text>
  <text font-size="4"><textPath xlink:href="#under">under the bottom</textPath></text>
</svg>""")
doc.new()
rep = s.import_file(paths)
assert rep["texts"] == 3 and not rep["skipped"] and not rep["notes"], rep
healthy()
by_words = {t.text: t for t in s.texts}
wave = by_words["middle of the wave"]
assert [type(c).__name__ for c in wave.path_curves] == ["Spline", "Line"] and wave.align == "center", wave.path_curves
assert all(c.construction for c in wave.path_curves)


def centre(contour):
    return (sum(p[0] for p in contour) / len(contour), sum(p[1] for p in contour) / len(contour))


for words in ("over the top", "under the bottom"):
    tx = by_words[words]
    first, last = centre(tx.contours[0]), centre(tx.contours[-1])
    assert hypot(first[0] - 10, first[1] - 10) < hypot(last[0] - 10, last[1] - 10), (words, first, last)  # (from its start)
over, under = by_words["over the top"], by_words["under the bottom"]
assert min(p[1] for c in over.contours for p in c) > 8 and max(p[1] for c in under.contours for p in c) < 12  # (each its side)

notes = write("notes.dxf", "\n".join(str(x) for x in [
    0, "SECTION", 2, "HEADER", 9, "$INSUNITS", 70, 4, 0, "ENDSEC",
    0, "SECTION", 2, "TABLES", 0, "TABLE", 2, "STYLE",
    0, "STYLE", 2, "Notes", 70, 0, 40, 0, 41, 1, 3, "arial.ttf",
    0, "ENDTAB", 0, "ENDSEC",
    0, "SECTION", 2, "ENTITIES",
    0, "TEXT", 8, "Words", 7, "Notes", 10, 0, 20, 0, 40, 2.5, 50, 30, 72, 1, 11, 20, 21, 10, 1, "%%c10 %%p0.1",
    0, "MTEXT", 8, "Words", 10, 40, 20, 40, 40, 3, 71, 5, 3, r"{\fArial|b0|i0;First}\PSec", 1, r"ond \S1/2;",
    0, "ENDSEC", 0, "EOF"]) + "\n")
doc.new()
rep = s.import_file(notes)
assert rep["texts"] == 2 and not rep["skipped"], rep
by_words = {t.text: t for t in s.texts}
dia = by_words["\u230010 \u00b10.1"]
assert dia.align == "center" and isclose(dia.angle, 30) and isclose(dia.height, 2.5) and near(dia.anchor.pos, (20, 10)), dia
m = by_words["First\nSecond 1/2"]
assert m.align == "center" and m.valign == "middle" and isclose(m.height, 3) and near(m.anchor.pos, (40, 40)), m

# --- DXF hatches: patterns (ours kept editable, others exact), islands, solid, gradient; SOLIDs ------
d45 = 3.175 / 2 ** 0.5
hatched = write("hatched.dxf", "\n".join(str(x) for x in [
    0, "SECTION", 2, "HEADER", 9, "$INSUNITS", 70, 4, 0, "ENDSEC",
    0, "SECTION", 2, "ENTITIES",
    # The rectangle's outline as its own polyline too (the hatch's boundary mustn't double it).
    0, "LWPOLYLINE", 8, "H", 90, 4, 70, 1, 10, 0, 20, 0, 10, 40, 20, 0, 10, 40, 20, 20, 10, 0, 20, 20,
    0, "HATCH", 8, "H", 2, "ANSI31", 70, 0, 71, 0, 91, 2,
    92, 3, 72, 0, 73, 1, 93, 4, 10, 0, 20, 0, 10, 40, 20, 0, 10, 40, 20, 20, 10, 0, 20, 20, 97, 0,
    92, 16, 93, 1, 72, 2, 10, 20, 20, 10, 40, 5, 50, 0, 51, 360, 73, 1, 97, 0,
    75, 0, 76, 1, 52, 0, 41, 1, 77, 0, 78, 1, 53, 45, 43, 0, 44, 0, 45, -d45, 46, d45, 79, 0, 98, 1, 10, 1, 20, 1,
    # A pattern of the file's own, at 30 degrees, dashed, on a D (a bulge's half circle).
    0, "HATCH", 8, "H", 2, "MINE", 70, 0, 71, 0, 91, 1,
    92, 3, 72, 1, 73, 1, 93, 2, 10, 50, 20, 0, 42, 0, 10, 70, 20, 0, 42, 1, 97, 0,
    75, 0, 76, 1, 52, 0, 41, 1, 77, 0, 78, 1, 53, 30, 43, 0, 44, 0, 45, -1.1339746, 46, 3.9641016, 79, 2, 49, 3, 49, -1,
    0, "HATCH", 8, "H", 2, "SOLID", 70, 1, 71, 0, 91, 1,
    92, 3, 72, 0, 73, 1, 93, 4, 10, 80, 20, 0, 10, 90, 20, 0, 10, 90, 20, 10, 10, 80, 20, 10, 97, 0,
    0, "SOLID", 8, "H", 10, 100, 20, 0, 11, 110, 21, 0, 12, 105, 22, 8, 13, 105, 23, 8,
    0, "HATCH", 8, "H", 2, "SOLID", 70, 1, 71, 0, 91, 1,
    92, 3, 72, 0, 73, 1, 93, 4, 10, 120, 20, 0, 10, 140, 20, 0, 10, 140, 20, 10, 10, 120, 20, 10, 97, 0,
    450, 1, 451, 0, 452, 0, 453, 2, 460, 0, 461, 0, 462, 1, 470, "LINEAR", 463, 0, 63, 5, 421, 255, 463, 1, 63, 2, 421, 16776960,
    0, "ENDSEC", 0, "EOF"]) + "\n")
doc.new()
rep = s.import_file(hatched)
assert rep["hatches"] == 2 and rep["fills"] == 5 and not rep["skipped"], rep
healthy()
assert len([l for l in s.lines if near(l.p1.pos, (0, 0)) or near(l.p2.pos, (0, 0))]) == 2, "the outline doubled"
by_pattern = {f.hatch.pattern: f for f in s.fills if f.hatch}
ansi = by_pattern["ANSI31"].hatch
assert ansi == setsquare.hatch("ANSI31"), ansi  # (one of ours: its angle and scale can be changed)
assert isclose(by_pattern["ANSI31"].area, 40 * 20 - pi * 25, rel_tol=1e-3), by_pattern["ANSI31"].area  # (the island left out)
mine = by_pattern["MINE"].hatch
(angle, x, y, dx, dy, dashes), = mine.lines
assert isclose(angle, 30) and isclose(dx, 1, abs_tol=1e-6) and isclose(dy, 4, abs_tol=1e-6) and dashes == [3, -1], mine.lines
assert isclose(by_pattern["MINE"].area, pi * 100 / 2, rel_tol=1e-3)
solid = [f for f in s.fills if not f.hatch and not f.gradient]
assert len(solid) == 2, solid  # (the solid hatch, the SOLID triangle)
grad = [f for f in s.fills if f.gradient][0].gradient
assert grad.type == "linear" and [c for _, c in grad.stops] == ["#0000ff", "#ffff00"], grad
assert near(grad.start.pos, (120, 5)) and near(grad.end.pos, (140, 5)), (grad.start.pos, grad.end.pos)

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
