# Exported and imported again (SVG and DXF): every curve once (a fill's outline is the line along
# it, not a second copy), text as text, a hatch as a hatch, fills whole; and the DXF's structure.
import os
import re
import tempfile
from collections import Counter

RED, BLUE = "#ff0000", "#0000ff"


def build():
    doc.new()
    sides = s.rectangle((0, 0), (60, 30))
    s.stroke(sides, color="#000000", width=0.5)
    hole = s.circle((15, 15), 5)
    s.stroke(hole, color="#000000", width=0.5)
    blob = s.spline([(35, 5), (45, 12), (40, 22), (52, 25)])
    s.stroke(blob, color=BLUE, width=0.35)
    oval = s.ellipse((45, -20), (55, -20), 4)
    s.stroke(oval, color=RED, width=0.25)
    body = s.fill(at=(5, 25), color=RED)
    body.gradient = setsquare.linear([RED, BLUE], start=sides[0].p1, end=sides[0].p2)
    s.fill(at=(15, 15), color="#00aa00", hatch=setsquare.hatch("ANSI31", angle=15, scale=0.5))
    s.text("Hello there", at=(20, 2), height=4, font="Arial", color="#000000")
    s.text("Two\nlines", at=(2, -10), height=3, font="Arial")
    return [t.text for t in s.texts]


def curve_counts():
    out = Counter()
    for x in s.entities:
        kind = type(x).__name__
        if kind != "Point" and not getattr(x, "construction", False):
            out[kind] += 1
    return out


texts = build()
original = curve_counts()
assert original == Counter({"Line": 4, "Circle": 1, "Spline": 1, "Ellipse": 1}), original

with tempfile.TemporaryDirectory() as folder:
    svg_path, dxf_path = os.path.join(folder, "t.svg"), os.path.join(folder, "t.dxf")
    doc.export_svg(svg_path)
    doc.export_dxf(dxf_path)
    with open(svg_path, encoding="utf-8") as f:
        svg = f.read()
    with open(dxf_path, encoding="ascii", newline="") as f:
        dxf = f.read()
    # The SVG: text as text, the hatch as a pattern; the text-as-outlines option gives paths.
    assert svg.count("<text ") == 3 and "font-family=\"'Arial'\"" in svg, svg[:2000]  # (one per line)
    assert "<pattern " in svg and 'fill="url(#hatch1)"' in svg
    assert "<text " not in doc.to_svg(text_as_outlines=True)

    # The DXF: AutoCAD 2004, every handle unique and below the seed.
    lines = dxf.split("\r\n")[:-1]
    pairs = list(zip(lines[0::2], lines[1::2]))
    assert ("1", "AC1018") in pairs and pairs[-1] == ("0", "EOF")
    at_seed = pairs.index(("9", "$HANDSEED")) + 1
    seed = int(pairs[at_seed][1], 16)
    handles = [int(v, 16) for i, (c, v) in enumerate(pairs) if c in ("5", "105") and i != at_seed]
    assert len(handles) == len(set(handles)) and max(handles) < seed, (len(handles), len(set(handles)), seed)
    types = Counter(v for c, v in pairs if c == "0")
    for t in ("LINE", "CIRCLE", "SPLINE", "ELLIPSE", "HATCH", "TEXT"):
        assert types[t] >= 1, (t, types)
    assert types["TEXT"] == 3, types  # (a line each)
    assert types["HATCH"] == 2, types
    assert "TEXT" not in Counter(v for c, v in zip(*[iter(doc.to_dxf(text_as_outlines=True).split("\r\n"))] * 2) if c == "0")

    for path in (svg_path, dxf_path):
        doc.new()
        s.import_file(path, into="new_layer", placement="file")
        back = curve_counts()
        # Every curve once: the rectangle's sides (not twice, though fill and lines both have them).
        assert back["Line"] == 4, (path, back)
        assert sum(back.values()) <= sum(original.values()) + 2, (path, back)  # (a circle may come in pieces)
        got = sorted(t.text for t in s.texts)
        assert got == sorted(["Hello there", "Two", "lines"]), (path, got)
        hatched = [f for f in s.fills if f.hatch is not None]
        assert len(hatched) == 1, (path, [repr(f) for f in s.fills])
        assert len(s.fills) == 2, (path, len(s.fills))
        if path.endswith(".dxf"):
            assert hatched[0].hatch.pattern == "ANSI31", hatched[0].hatch  # (a built-in, as it was)
print("ok")
