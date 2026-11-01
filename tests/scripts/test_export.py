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
