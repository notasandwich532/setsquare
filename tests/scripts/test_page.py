# The page: optional, a standard or custom size, turned, placed on the origin, coloured; saved,
# undone, recorded, and what the SVG export is the size of.
import os
import re
import tempfile

assert s.page is None

s.set_page("A4")
p = s.page
assert (p["size"], p["width"], p["height"], p["orientation"], p["origin"], p["color"]) == \
    ("A4", 210, 297, "portrait", "bottom_left", None), p
assert p["rect"] == (0, 0, 210, 297)

s.set_page("A3", orientation="landscape", origin="center", color="#fffdf5")
p = s.page
assert (p["width"], p["height"], p["orientation"], p["color"]) == (420, 297, "landscape", "#fffdf5"), p
assert p["rect"] == (-210, -148.5, 210, 148.5)
s.set_page("Letter", origin="top_left")
assert s.page["rect"] == (0, -279.4, 215.9, 0)

s.set_page(width=100, height=50)
assert s.page["size"] == "custom" and s.page["orientation"] == "landscape"
s.set_page(width=100, height=50, orientation="portrait")  # turned
assert (s.page["width"], s.page["height"]) == (50, 100)

for bad in (lambda: s.set_page("B7"), lambda: s.set_page(), lambda: s.set_page(width=0, height=10),
            lambda: s.set_page("A4", orientation="sideways"), lambda: s.set_page("A4", origin="middle")):
    try:
        bad()
        raise AssertionError("accepted a bad page")
    except ValueError:
        pass

# --- the SVG is the page --------------------------------------------------------------------
s.set_page("A4", color="#ffffff")
blank = doc.to_svg()  # (a blank page exports: it's a page)
assert 'width="210mm" height="297mm" viewBox="0 0 210 297"' in blank, blank[:400]
assert '<rect id="page" x="0" y="0" width="210" height="297" fill="#ffffff"' in blank

c = s.circle((20, 30), 10)
s.stroke(c, color="#ff0000", width=1)
svg = doc.to_svg()
m = re.search(r'<circle cx="([^"]+)" cy="([^"]+)" r="10"', svg)
assert m and (float(m.group(1)), float(m.group(2))) == (20, 297 - 30), svg  # y down from the page's top
far = s.line((300, 0), (400, 0))  # off the page: in the file, but outside its view (cut off)
assert 'width="210mm"' in doc.to_svg()

# Without the page: fitted round the drawing as before.
fitted = doc.to_svg(page=False, margin=0)
assert 'width="210mm"' not in fitted and '<rect id="page"' not in fitted
s.delete(far)

# No colour: no paper rect (a transparent background).
s.set_page("A4")
assert '<rect id="page"' not in doc.to_svg()

# Centre origin: the circle's place on the page moves with it.
s.set_page("A4", origin="center")
m = re.search(r'<circle cx="([^"]+)" cy="([^"]+)"', doc.to_svg())
assert (float(m.group(1)), float(m.group(2))) == (105 + 20, 148.5 - 30), m.groups()

# --- saving, recording (undo: see the app tests) ---------------------------------------
s.clear_page()
assert s.page is None
s.set_page("A4", origin="center")
with tempfile.TemporaryDirectory() as folder:
    path = os.path.join(folder, "page.dsk")
    doc.save(path)
    doc.new()
    assert s.page is None
    doc.open(path)
assert s.page["size"] == "A4" and s.page["origin"] == "center"

doc.new()
s.set_page("A5", orientation="landscape", origin="top_left", color="#ffeedd")
lines = doc.history
assert lines[-1] == 's.set_page("A5", orientation="landscape", origin="top_left", color="#ffeedd")', lines
s.set_page(width=120.5, height=80)
assert doc.history[-1] == "s.set_page(width=120.5, height=80)", doc.history[-1]
s.clear_page()
assert doc.history[-1] == "s.clear_page()"
print("ok")
