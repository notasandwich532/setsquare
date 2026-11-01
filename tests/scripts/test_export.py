# Exporting from Python.
import os
import tempfile

sides = s.rectangle((0, 0), (30, 20))
s.stroke(sides, color="#880015", width=0.7, dash="dash_dot")
s.fill(at=(5, 5), color="#99d9ea")
helper = s.line((0, 0), (30, 20))
helper.construction = True

text = doc.to_svg(margin=0)
assert text.startswith("<?xml") and "<svg" in text and text.rstrip().endswith("</svg>")
assert 'width="30.7mm"' in text, text[:300]
assert text.count("<line ") == 4 and "Construction" not in text
assert "fill-rule=\"evenodd\"" in text

with tempfile.TemporaryDirectory() as folder:
    path = os.path.join(folder, "out.svg")
    doc.export_svg(path, construction=True)
    with open(path, encoding="utf-8") as f:
        written = f.read()
    assert "Construction" in written and written.count("<line ") == 5

doc.new()
try:
    doc.to_svg()
    raise AssertionError("exported an empty drawing")
except ValueError:
    pass
print("ok")

# DXF: exact geometry in mm.
doc.new()
sides = s.rectangle((0, 0), (30, 20))
s.stroke(sides[0], color="#ff0000", dash="dashed")
hole = s.circle((15, 10), 4)
dxf = doc.to_dxf()
pairs = dxf.split("\r\n")[:-1]
assert len(pairs) % 2 == 0
codes = list(zip(pairs[0::2], pairs[1::2]))
assert ("0", "EOF") == codes[-1] and ("1", "AC1009") in codes
assert codes.count(("0", "LINE")) == 4 and codes.count(("0", "CIRCLE")) == 1
assert ("6", "DASHED") in codes and ("62", "1") in codes
with tempfile.TemporaryDirectory() as folder:
    path = os.path.join(folder, "out.dxf")
    doc.export_dxf(path, styles=False)
    with open(path, encoding="ascii", newline="") as f:
        written = f.read()
    assert "STROKES" not in written.split("ENTITIES")[1]
print("dxf ok")
