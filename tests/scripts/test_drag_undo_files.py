# Dragging, transactions/undo, and saving + reopening through the API.
import os
import tempfile
from math import isclose, hypot

# Drag: a pinned, fixed-length arm swings about its pivot.
arm = s.line(s.origin, (10, 0))
s.distance(arm, value=10)
s.drag([arm.p2], -10, 10)
assert isclose(arm.length, 10, rel_tol=1e-9)
assert isclose(arm.p2.x, 0, abs_tol=1e-5) and isclose(arm.p2.y, 10, abs_tol=1e-5), arm.p2

# move_to drags towards an absolute position.
free = s.point(50, 50)
free.move_to(55, 45)
assert free.pos == (55, 45) or (isclose(free.x, 55) and isclose(free.y, 45))

# Rim drag resizes a free circle.
c = s.circle((100, 0), 5)
s.drag_edge(c, (105, 0), 3, 0)
assert isclose(c.radius, 8, abs_tol=1e-4), c.radius

# Files: save, start over, reopen.
path = os.path.join(tempfile.mkdtemp(), "api.dsk")
count = len(s.entities)
doc.save(path)
assert doc.path == path and not doc.modified
doc.new()
assert len(s.entities) == 1  # just the origin
doc.open(path)
assert len(s.entities) == count
assert isclose(s[arm.id].length, 10)
print("drag/files ok")
