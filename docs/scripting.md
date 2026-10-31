# Scripting with Python

Everything you can do in the UI can be done from Python, and every UI action is recorded as the
Python that reproduces it.

Python is bundled with the app: CPython 3.14, built by vcpkg and shipped as `python/` next to the
executables. It runs in isolated mode, so whatever Python is installed on the machine, and its
environment variables, never get involved.

## Where to run Python

| Where | How |
|---|---|
| **Console** | The *Python* dock (View → Python). Enter runs; Shift+Enter adds a line; Up/Down recall history. |
| **Macros** | Macros → Run Script… (Ctrl+Shift+R). Scripts in the macros folder are listed under Macros → Macros Folder. |
| **Startup** | `drawapp --script setup.py [part.dsk]` |
| **Headless** | `drawapp-cli script.py [args]`, `drawapp-cli --open part.dsk script.py`, or `drawapp-cli -c "print(s.dof)"`. The exit code is 0 on success and 1 on an uncaught exception, and a traceback goes to stderr. |

In all of these, `doc` (the document), `s` (its sketch), `app` (the window) and `drawapp` are
predefined. Elsewhere, start with `import drawapp; doc = drawapp.document(); s = doc.sketch`.

**Undo:**
- A script or macro run is **one undo step**.
- In the console, each call is its own step, just like the UI action it may have been recorded from.
- `with doc.transaction(): ...` groups any block into a single step.

## The recorder

With *Record UI actions* ticked, the console shows each action as Python while you work:

```python
s.rectangle(s.origin, p7)
p12 = s.point(50, 0)
p13 = s.point(70, 0)
l14 = s.line(p12, p13)
k15 = s.horizontal(l14)
d16 = s.distance(l4, value=30, label=(0, -8))
d16.value = 40
s.drag([p7], 0, 10)
```

Items are named by id: `p` point, `l` line, `c` circle, `a` arc, `k` constraint, `d` dimension.
In the console those names always resolve to the live item, so you can paste recorded lines back
in. Macros → Save Recording as Macro… writes the session out as a `.py` file.

## API

### Geometry
Wherever a point is expected, you can pass a `Point` or an `(x, y)` pair (which makes a new point).

```python
p = s.point(10, 5)
l = s.line(s.origin, (40, 0))            # returns a Line
c = s.circle((20, 20), 5)
a = s.arc(center, start, end)            # counter-clockwise from start to end
bottom, right, top, left = s.rectangle((0, 0), (30, 20))   # sides come H/V constrained
s.delete(l, c)                           # and whatever depends on them
```

Shapes come with their constraints, and each returns its outline curves:

```python
s.rectangle_3point(a, b, width_point)    # rotated
s.rectangle_center(center, corner)
s.circle_2point(a, b)                    s.circle_3point(a, b, c)
s.circle_tangent([l1, l2], near=(x, y), radius=5)     # or three curves, no radius
s.arc_tangent(line_or_arc, its_end_point, end)
s.polygon(center, vertex, sides=6)       s.polygon(center, side_mid, sides=6, circumscribed=True)
s.polygon_edge(a, b, sides=6, left=True)
s.slot(center1, center2, width)          s.slot_overall(end1, end2, width)
s.slot_center_point(center, cap_center, width)
s.arc_slot(center, start, end, width)    # counter-clockwise centreline
s.arc_slot_3point(start, end, through, width)
```

### Constraints
These are refused with `ValueError` when they'd over-constrain the sketch, the same as in the UI.

```python
s.coincident(p, q)        s.coincident(p, line_or_curve)
s.horizontal(l)           s.horizontal(p, q)        s.vertical(...)
s.parallel(l1, l2)        s.perpendicular(l1, l2)   s.equal(a, b)
s.tangent(line, circle)   s.tangent(circle1, arc2)  s.concentric(c1, c2)
s.midpoint(p, l)          s.symmetric(p, q, axis_line)     s.collinear(l1, l2)
p.fixed = True            l.construction = True
```

### Dimensions
`value=None` (the default) keeps the current size, as the Dimension tool does. `label=` is the
label's offset from the dimension's anchor.

```python
s.distance(l)                 # a line's length
s.distance(p, q, value=25)    s.distance(p, line)
s.horizontal_distance(p, q)   s.vertical_distance(l)
s.radius(arc)                 s.diameter(circle, value=12)
s.angle(l1, l2, value=30)     # degrees, from l1 to l2
d.value = 40                  # re-solves; ValueError if impossible
d.label = (0, -8)
```

### Trimming and corners
`near` is where you would click; it picks the part of the curve that's meant.

```python
s.trim(curve, near=(x, y))          # remove the piece around near, up to the crossings
s.extend(line_or_arc, near=(x, y))  # stretch the nearer end to the next curve
s.break_curve(curve, near=(x, y))   # split at the crossings around near; returns the pieces
arc = s.fillet(l1, l2, 5)           # lines/arcs sharing a corner; originals become construction
arc = s.fillet(a, b, 5, near=((x1, y1), (x2, y2)))   # crossing curves: say which sides to keep
cut = s.chamfer(l1, l2, 5)                 # equal distances
cut = s.chamfer(l1, l2, 5, distance2=8)    # two distances
cut = s.chamfer(l1, l2, 5, angle=30)       # distance + angle
```

### Moving things
```python
s.drag([p, q], dx, dy)        # like dragging with the mouse; constraints still hold
p.move_to(x, y)
s.drag_edge(circle, (x, y), dx, dy)   # grab a rim: resizes a free circle
s.move(items, dx=10, dy=0)            # rigidly, dragging the real geometry (attached things follow)
s.move(items, angle=30, about=(x, y)) # turn (then shift by dx, dy if given)
s.scale(items, 2, about=(x, y))       # dimensions between the items scale too
```

### Copies, mirrors, patterns and offsets
Copies made with `copy`/`paste` are independent; `mirror`, the patterns and `offset` stay linked to
their originals: edit the original and the copies follow. A pattern or offset is one feature
constraint `k`; set `k.value` (spacing, total angle, or distance) to move the copies, and delete
`k` to unlink them.

```python
new = s.copy(items, dx=50, dy=0, angle=0, about=(0, 0))  # with the constraints among them
images = s.mirror(items, line)          # symmetric constraints; points on the line are shared
k = s.rectangular_pattern(items, count=4, spacing=10, angle=0, count2=2, spacing2=15)
k = s.rectangular_pattern(items, count=3, spacing=10, symmetric=True)   # 3 each way (5 in all)
k = s.rectangular_pattern(items, count=2, count2=3, spacing2=-8)        # rows below, not above
k = s.circular_pattern(items, s.origin, count=6, angle=360)   # a full turn spaces them evenly
k.value = 12                            # spacing (rectangular) or total angle (circular)
k = s.set_pattern(k, count=5)           # a new count remakes the copies: use the returned pattern
k.sources, k.copies                     # originals, and per copy the matching items
o = s.offset(curve, 2, toward=(x, y))   # one curve offsets its whole chain; or pass a list
o.value = 3                             # the distance; the originals stay put
s.chain(curve)                          # the curves joined end to end with it
data = s.extract(items)                 # JSON, as the clipboard holds it
pasted = s.paste(data, at=(x, y))       # centred at (x, y)
```

### Appearance (strokes)
How curves look in the finished drawing; the solver ignores it. Curves without a stroke draw as
plain sketch geometry.

```python
s.stroke(curves, color="#ed1c24", width=0.5, dash="dashed")  # any of these; the rest stay as they are
s.stroke(line, color=(0, 162, 232, 128))      # (r, g, b[, a]) works too
s.stroke(circle, width=0)                      # 0 = hairline (always 1 pixel)
s.clear_stroke(curves)
line.stroke                                    # {'color': '#ed1c24', 'width': 0.5, 'dash': 'dashed'} or None
app.presentation = True                        # see the drawing as it will look
```
Line styles: `solid`, `dashed`, `dotted`, `dash_dot`, `dash_dot_dot`. Widths are millimetres on
the drawing. Copies, mirrors, patterns, offsets and trimmed or filleted pieces keep their stroke.

### Origin and axes
`s.origin`, `s.x_axis` and `s.y_axis` are fixed and permanent. Use them like any point or line:
`s.angle(s.x_axis, l, value=30)`, `s.distance(p, s.y_axis, value=5)`, `s.coincident(p, s.x_axis)`.
They aren't included in `s.entities` and the other lists.

### Reading
```python
s.origin, s[12], s.points, s.lines, s.circles, s.arcs, s.entities, s.constraints, s.dimensions
p.x, p.y, p.pos     l.p1, l.p2, l.length     c.center, c.radius     a.start, a.end, a.radius
k.type, k.refs, k.value, k.is_dimension, k.is_feature      item.id, item.name, item.exists
s.dof                 r = s.solve()  ->  r.ok, r.dof, r.conflicting, r.redundant, r.fully_constrained
e.fully_constrained
```

### Document and window
```python
doc.save(path)  doc.open(path)  doc.new()  doc.undo()  doc.redo()  doc.path  doc.modified
doc.to_json()
app.fit()  app.selection = [l, c]  app.tool = "line"  app.message("hi")   # needs the UI
```

`app.tool` accepts the tool names, which follow the API names:
- Geometry: select, line, point, rectangle, rectangle_3point, rectangle_center, circle, circle_2point, circle_3point, circle_tangent2, circle_tangent3, arc, arc3, arc_tangent
- Polygons and slots: polygon_inscribed, polygon_circumscribed, polygon_edge, slot, slot_overall, slot_center_point, arc_slot, arc_slot_3point
- Dimensions and constraints: dimension, coincident, horizontal_vertical, parallel, perpendicular, tangent, equal, midpoint, concentric, symmetric, collinear, fix

## Tests
Each `tests/scripts/test_*.py` is a CTest test run through `drawapp-cli`. The GUI test
*"Replaying recorded UI actions reproduces the sketch"* drives the real tools, replays what they
recorded, and checks the result is identical. It's there to keep "the UI and Python can do the
same things" true.
