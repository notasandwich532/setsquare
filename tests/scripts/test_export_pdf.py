# Exporting a PDF from Python: the page at true size (or fitted round the drawing).
import os
import tempfile

sides = s.rectangle((0, 0), (40, 20))
s.fill(at=(5, 5), color="#99d9ea")
s.text("Setsquare", at=(5, 8), height=4)

folder = tempfile.mkdtemp()
fitted = os.path.join(folder, "fitted.pdf")
doc.export_pdf(fitted, margin=3)
with open(fitted, "rb") as f:
    data = f.read()
assert data.startswith(b"%PDF-"), data[:20]
assert b"/FontFile" in data  # (the text as text)

s.set_page("A4")
paged = os.path.join(folder, "page.pdf")
doc.export_pdf(paged)
with open(paged, "rb") as f:
    assert b"/MediaBox [0 0 595.000000 842.000000]" in f.read()  # A4, to the point

outlined = os.path.join(folder, "outlined.pdf")
doc.export_pdf(outlined, text_as_outlines=True)
with open(outlined, "rb") as f:
    assert b"/FontFile" not in f.read()  # (the letters as shapes)
print("ok")
