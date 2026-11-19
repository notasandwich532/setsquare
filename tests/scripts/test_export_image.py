# Exporting PNG and JPEG from Python: the page at the resolution asked.
import os
import struct
import tempfile

s.rectangle((0, 0), (40, 20))
s.fill(at=(5, 5), color="#99d9ea")
s.text("Setsquare", at=(5, 8), height=4)
s.set_page("A4")

folder = tempfile.mkdtemp()
png = os.path.join(folder, "page.png")
doc.export_image(png, dpi=50)
with open(png, "rb") as f:
    data = f.read()
assert data.startswith(b"\x89PNG"), data[:8]
width, height = struct.unpack(">II", data[16:24])  # (the IHDR's)
assert (width, height) == (413, 585), (width, height)  # 210 x 297 mm at 50 dpi

jpg = os.path.join(folder, "page.jpg")
doc.export_image(jpg, dpi=50, quality=80)
with open(jpg, "rb") as f:
    assert f.read(3) == b"\xff\xd8\xff"

assert not any("doc.export_image(" in line for line in doc.history)  # (scripted exports aren't recorded)
try:
    doc.export_image(os.path.join(folder, "page.gif"))
    assert False, "a .gif should be refused"
except RuntimeError as e:
    assert ".png" in str(e), e
print("ok")