# Sublayers through the API: new items go to the active one, items in different sublayers depend on
# each other and fill together, and sublayers can be renamed, hidden, reordered, emptied into each
# other, deleted, and saved.
import os, tempfile
from math import isclose

assert [l.name for l in s.sublayers] == ["Sublayer 1"]
square = s.rectangle((0, 0), (40, 40))
width = s.distance(square[0], value=40)
s.distance(square[1], value=40)
walls = s.sublayer
walls.name = "Square"
assert walls.name == "Square" and walls.active

diag = s.add_sublayer("Diagonal")
assert s.sublayer == diag and not walls.active
d = s.line(square[0].p1, square[1].p2)  # its ends are the square's corners
assert d.sublayer == diag and d.p1.sublayer == walls

# A fill sees the curves of every sublayer.
f = s.fill(at=(30, 10), color="#ff0000")
assert isclose(f.area, 800, rel_tol=1e-6), f.area

# Dragging the square drags the diagonal along (one solve).
width.value = 50
assert isclose(d.p2.x, square[1].p2.x) and isclose(d.p2.y, square[1].p2.y)

s.show_sublayer("Square", False)
assert not walls.visible
s.show_sublayer(walls)
assert walls.visible

# Moving an item takes its points and the constraints wholly on it.
extra = s.add_sublayer("Extra")
c = s.circle((100, 0), 5)
s.radius(c, value=5)
s.move_to_sublayer([c], "Square")
assert c.sublayer == walls
assert all(k.sublayer == walls for k in s.constraints if k.type == "radius")
assert len(extra.items) == 0

s.move_sublayer("Extra", 0)
assert [l.name for l in s.sublayers] == ["Extra", "Square", "Diagonal"]
s.delete_sublayer("Extra")
assert [l.name for l in s.sublayers] == ["Square", "Diagonal"]

# Deleting a sublayer deletes what's in it.
s.sublayer = "Diagonal"
s.delete_sublayer(diag)
assert not d.exists and s.sublayer == walls
assert len(s.fills) == 1 and isclose(s.fills[0].area, 50 * 40)  # the fill, now the whole square

# Names stay unique.
again = s.add_sublayer("Square")
assert again.name != "Square"

path = os.path.join(tempfile.gettempdir(), "setsquare_sublayers.dsk")
doc.save(path)
doc.open(path)
assert [l.name for l in s.sublayers] == ["Square", again.name]
assert s.sublayer.name == again.name
assert len(s.sublayers[0].items) > 0
os.remove(path)
