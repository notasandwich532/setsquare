# Reference images: inserted (kept in the drawing), placed, calibrated, saved, recorded, and never
# part of the drawing's exports.
import os
import re
import struct
import tempfile
import zlib
from math import isclose


def png(path, width, height, rgb=(220, 40, 40)):
    """A plain PNG of one colour (no imaging library needed)."""
    def chunk(kind, data):
        return struct.pack(">I", len(data)) + kind + data + struct.pack(">I", zlib.crc32(kind + data) & 0xffffffff)
    rows = b"".join(b"\x00" + bytes(rgb) * width for _ in range(height))
    with open(path, "wb") as f:
        f.write(b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", width, height, 8, 2, 0, 0, 0))
                + chunk(b"IDAT", zlib.compress(rows)) + chunk(b"IEND", b""))


def near(a, b, tol=1e-6):
    return all(isclose(x, y, abs_tol=tol) for x, y in zip(a, b))


folder = tempfile.mkdtemp()
path = os.path.join(folder, "plan.png")
png(path, 40, 20)

# --- inserting ------------------------------------------------------------------------------
natural = s.image(path)
assert natural.pixels == (40, 20)
assert isclose(natural.width, 40 * 25.4 / 96, rel_tol=1e-3), natural.width  # (no resolution given: 96 per inch)
assert natural.center == (0, 0) and natural.opacity == 0.5
s.delete(natural)
assert not s.images

img = s.image(path, at=(10, 5), width=80, opacity=0.7)
assert img.source == os.path.abspath(path)
assert (img.width, img.height) == (80, 40)
assert near(img.corners[0], (-30, -15)) and near(img.corners[2], (50, 25)), img.corners
assert s[img.id] == img and img in s and "plan.png" in repr(img)
assert s.image(path, height=10).width == 20  # by its height
s.delete(s.images[-1])

for bad in (lambda: s.image(os.path.join(folder, "missing.png")),
            lambda: s.image(path, width=0)):
    try:
        bad()
        raise AssertionError("accepted a bad image")
    except ValueError:
        pass
text = os.path.join(folder, "notes.txt")
with open(text, "w") as f:
    f.write("not a picture")
try:
    s.image(text)
    raise AssertionError("read a text file as an image")
except ValueError as e:
    assert "image" in str(e)

# --- placing ----------------------------------------------------------------------------------
s.edit_image(img, angle=90, flip_h=True)
assert img.angle == 90 and img.flip_h
assert near(img.at(0, 0), (10 + 20, 5 - 40)), img.at(0, 0)  # turned a quarter: bottom-left goes to the lower right
s.edit_image(img, angle=0, flip_h=False, at=(0, 0), opacity=2)
assert img.opacity == 1 and not img.flip_h
s.edit_image(img, height=20)
assert img.width == 40

# --- calibrate (as in Fusion) -------------------------------------------------------------------
# Two marks on it 30 mm apart as it is now are really 75 mm apart: everything scales 2.5x about the first.
a, b = img.at(0.25, 0.5), img.at(1.0, 0.5)
assert isclose(b[0] - a[0], 30)
img.calibrate(a, b, 75)
assert isclose(img.width, 100) and near(img.at(0.25, 0.5), a), (img.width, img.at(0.25, 0.5), a)
assert isclose(img.at(1.0, 0.5)[0] - a[0], 75)
for bad in (lambda: img.calibrate(a, a, 10), lambda: img.calibrate(a, b, 0)):
    try:
        bad()
        raise AssertionError("calibrated without a length")
    except ValueError:
        pass

# --- not part of the drawing ----------------------------------------------------------------------
s.line((0, 0), (10, 0))
svg = doc.to_svg()
assert "<image" not in svg and "plan.png" not in svg
assert "plan" not in doc.to_dxf()
s.edit_image(img, visible=False)
assert not img.visible
s.edit_image(img, visible=True)

# --- saved in the drawing ---------------------------------------------------------------------------
saved = os.path.join(folder, "with image.setsq")
before = (img.center, img.width, img.angle, img.opacity, img.pixels)
doc.save(saved)
assert os.path.getsize(saved) > 100
os.remove(path)  # (the file isn't needed again: the drawing has it)
doc.open(saved)
img = s.images[0]
assert (img.center, img.width, img.angle, img.opacity, img.pixels) == before

# --- layers ---------------------------------------------------------------------------------------
base = s.layer
top = s.add_layer("Tracing")
png(os.path.join(folder, "plan2.png"), 10, 10, (0, 0, 255))
on_top = s.image(os.path.join(folder, "plan2.png"), width=5)
s.delete_layer(top)
assert not on_top.exists and len(s.images) == 1
s.layer = base

# --- recorded ---------------------------------------------------------------------------------------
doc.new()
png(path, 40, 20)
img = s.image(path, at=(3, 4), width=60, angle=15, opacity=0.4, flip_v=True)
s.edit_image(img, width=50)
img.calibrate((0, 0), (10, 0), 20)
lines = doc.history
assert lines[0].startswith(f'i{img.id} = s.image(r"{os.path.abspath(path)}", at=(3, 4), width=60, angle=15'), lines[0]
assert all(l.startswith("s.edit_image(") for l in lines[1:]), lines
expected = (img.center, img.width, img.angle, img.opacity, img.flip_v)


class Names(dict):  # as the console has them
    def __missing__(self, key):
        m = re.match(r"^(sp|ea|[plcakdftei])(\d+)$", key)
        if not m:
            raise KeyError(key)
        return s[int(m.group(2))]


doc.new()
exec("\n".join(lines), globals(), Names())
again = s.images[0]
assert (again.center, again.width, again.angle, again.opacity, again.flip_v) == expected
print("ok")
