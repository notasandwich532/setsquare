# Layers through the API: each is its own sketch; geometry from another layer can only be used by
# projecting it (a linked, fixed, construction copy that follows the original); fills are per layer.
import os, tempfile
from math import isclose

base = s.layer
assert [l.name for l in s.layers] == ["Layer 1"]
sq = s.rectangle((0, 0), (40, 30))
sq[0].p1.fixed = True
width = s.distance(sq[0], value=40)
s.distance(sq[1], value=30)
s.fill(at=(5, 5), color="#ff0000")

top = s.add_layer("Top")
assert s.layer == top and [l.name for l in s.layers] == ["Top", "Layer 1"]  # new layers go on top
assert [l.name for l in s.sublayers] == ["Sublayer 1"]  # names are per layer

# Nothing here may refer to the other layer's geometry...
for attempt in (lambda: s.line(sq[0].p1, (50, 50)), lambda: s.horizontal(sq[0]), lambda: s.coincident(sq[0].p2, sq[1])):
    try:
        attempt()
        raise AssertionError("a reference to another layer was allowed")
    except ValueError as e:
        assert "another layer" in str(e)
# ...but the origin and axes are in every layer.
s.line(s.origin, (5, 5))

# Projecting: linked copies, fixed and construction, sharing their projected corner.
bottom, right = s.project([sq[0], sq[1]])
assert bottom.projected and bottom.linked and bottom.source == sq[0]
assert bottom.construction and bottom.fixed and bottom.layer == top
assert bottom.p2 == right.p1  # the shared corner is projected once
assert s.project([sq[0]]) == [bottom]  # again: the same copy

# Geometry here can be tied to the copies, and follows when the original changes.
post = s.line(bottom.p2, (40, 50))
s.vertical(post)
width.value = 60
assert isclose(bottom.p2.x, 60) and isclose(post.p2.x, 60)

# Copies can't be unfixed; Break Link makes them ordinary geometry.
try:
    bottom.fixed = False
    raise AssertionError("a projection was unfixed")
except ValueError:
    pass

# Layers can't depend on each other in a circle.
ring = s.circle((20, 15), 4)
s.layer = base
try:
    s.project([ring])
    raise AssertionError("a circular dependency was allowed")
except ValueError as e:
    assert "depend on each other" in str(e)

# Fills are per layer: Top's fill sees only Top's curves (the projections are construction).
s.layer = top
f = s.fill(at=(20, 15), color="#00ff00")
assert f.layer == top and isclose(f.area, 3.14159 * 16, rel_tol=0.01)
right.construction = False
bottom.construction = False
assert [round(x.area) for x in s.layer.fills] == [round(3.14159 * 16)]
assert len(base.fills) == 1 and isclose(base.fills[0].area, 60 * 30)

s.break_link([right])
assert not right.projected and not right.fixed

# Saving keeps it all.
path = os.path.join(tempfile.gettempdir(), "drawapp_layers.dsk")
doc.save(path)
doc.open(path)
assert [l.name for l in s.layers] == ["Top", "Layer 1"] and s.layer.name == "Top"
assert [l.name for l in s.layers[1].sublayers] == ["Sublayer 1"]
copies = [e for e in s.entities if e.projected]
assert len(copies) >= 3 and all(c.linked for c in copies)
assert s.solve().ok
os.remove(path)

# Hiding and reordering; deleting a layer leaves projections of it behind, unlinked.
s.show_layer("Layer 1", False)
assert not s.layers[1].visible
s.move_layer("Top", 1)
assert [l.name for l in s.layers] == ["Layer 1", "Top"]
s.delete_layer("Layer 1")
assert [l.name for l in s.layers] == ["Top"]
assert all(not c.linked for c in s.entities if c.projected)
assert s.solve().ok
