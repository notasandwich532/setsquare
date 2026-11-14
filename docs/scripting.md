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
| **Startup** | `setsquare --script setup.py [part.setsq]` |
| **Headless** | `setsquare-cli script.py [args]`, `setsquare-cli --open part.setsq script.py`, or `setsquare-cli -c "print(s.dof)"`. The exit code is 0 on success and 1 on an uncaught exception, and a traceback goes to stderr. |

In all of these, `doc` (the document), `s` (its sketch), `app` (the window) and `setsquare` are
predefined. Elsewhere, start with `import setsquare; doc = setsquare.document(); s = doc.sketch`.

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

Items are named by id: `p` point, `l` line, `c` circle, `a` arc, `e` ellipse, `ea` elliptical arc,
`sp` spline, `k` constraint, `d` dimension, `f` fill, `t` text.
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
e = s.ellipse(center, (40, 0), 10)       # centre, one axis end, the other axis end (or its length)
e.center, e.major, e.minor               # points (the axes are kept at right angles)
e.major_radius, e.minor_radius, e.angle  # s.radius(e, value=30): the major semi-axis
sp = s.spline([(0, 0), (10, 10), (20, 0)])                    # through the points
sp = s.spline([p1, p2, p3, p4], kind="control", closed=True)  # pulled by them
sp.points, sp.kind, sp.closed, sp.lengthbottom, right, top, left = s.rectangle((0, 0), (30, 20))   # sides come H/V constrained
s.delete(l, c)                           # and whatever depends on them
```

Ellipses and splines work like the other curves: points can be kept on them (`s.coincident`),
they can be tangent to any curve (`s.tangent`, also where they join end to end), they close
areas for fills, cut and can be trimmed or broken (an ellipse becomes an elliptical arc,
`s.ellipse_arc`; a spline a piece of itself with `start` and `end` points), and copy, mirror
and pattern. Text can run along them. Fillets and chamfers work between any two open curves
(a whole ellipse or closed spline: trim it first); extend carries an elliptical arc round its
ellipse, a piece of a spline along it, and a whole spline on past its end (the point reached
becomes its new end point). An ellipse's or spline's offset is a spline through points at the
distance from it (placed from it, so they follow it exactly; only its ends join the solve).

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
k = s.ratio(a, b, value=1.5)   # a's length (or radius) is 1.5 times b's; k.value = 2 changes it
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
o = s.offset(curve, 2, both_sides=True, caps=True)  # both sides, ends closed with half circles
o.value = 3                             # the distance; the originals stay put
s.chain(curve)                          # the curves joined end to end with it
data = s.extract(items)                 # JSON, as the clipboard holds it
pasted = s.paste(data, at=(x, y))       # centred at (x, y)
```

### Dividing a line
Points along a line that stay at their places on it as it moves or stretches (the Divide tool).
Evenly, or by a ratio of the gaps from the line's start (`p1`) to its end: `3:2:1` makes two
points with the biggest gap first; `reversed=True` measures from the end instead.

```python
d = s.divide(line, count=3)                       # 3 points, 4 equal gaps
d = s.divide(line, ratio="3:2:1")                 # or ratio=[3, 2, 1]
d = s.divide(line, ratio="3:2:1", reversed=True)  # biggest gap at p2's end
d.copies[0]                                       # the points, in order from the start
d = s.set_divide(d, reversed=False)               # same number of points: the same divide
d = s.set_divide(d, count=5)                      # a new count remakes the points: use the returned one
```

### Text
Text sits in a box (a construction rectangle you can dimension), along a curve, or at a point, so it
can be constrained and dragged like other geometry. Its letters come from the font and are kept with the sketch (files open, and
export, where the font isn't installed).

```python
app.fonts("script")                       # installed families containing "script"
t = s.text("Hello", (10, 20), height=8, font="Arial")   # height: capital letters, mm
t = s.text("Hello", p, angle=30, bold=True, italic=False, color="#202020")
t = s.text("Scribble", p, font="Segoe Script", outline="merged")      # overlapping strokes joined
t = s.text("Scribble", p, font="Segoe Script", outline="silhouette")  # only the outside edge
t = s.text("Thin", p, font="Ink Free", min_thickness=0.8)  # parts narrower than 0.8 mm thickened
s.edit_text(t, text="Hi", height=10)      # any of the settings; the letters are remade
t = s.text("In a box", box=((0, 0), (60, 30)), align="center", valign="middle")
                                          # a construction rectangle the text sits in (top left by
                                          # default) and follows; t.box: its corners
t = s.text("Two\nlines", p, align="center", valign="top")  # which part sits on p
t = s.text("Spaced", p, spacing=20, flip_h=True)   # % of the size between letters; mirrored
t = s.text("Round we go", path=circle, align="center")  # along a line/arc/circle, following it
                                          # (align: its start/middle/end; a circle: its top)
t.anchor, t.contours, t.bounds            # its point; its outline as loops of (x, y); extent
```
### Appearance (strokes)
How curves look in the finished drawing; the solver ignores it. Curves without a stroke draw as
plain sketch geometry.

```python
s.stroke(curves, color="#ed1c24", width=0.5, dash="dashed")  # any of these; the rest stay as they are
s.stroke(line, color=(0, 162, 232, 128))      # (r, g, b[, a]) works too
s.stroke(circle, width=0)                      # 0 (or "hairline") = always 1 pixel
s.stroke(outline, width="none")               # no line in the finished drawing (presentation, SVG):
                                               # e.g. a fill without an outline; still bounds fills
s.clear_stroke(curves)
line.stroke                                    # {'color': '#ed1c24', 'width': 0.5, 'dash': 'dashed'} or None
app.presentation = True                        # see the drawing as it will look
```
Line styles: `solid`, `dashed`, `dotted`, `dash_dot`, `dash_dot_dot`. Widths are millimetres on
the drawing. Copies, mirrors, patterns, offsets and trimmed or filleted pieces keep their stroke.

### Appearance (fills)
A fill colours the closed area around a point, like a paint bucket; islands inside it (a hole in
a plate) stay unfilled, and construction geometry doesn't count as an edge. The area is found
from the geometry each time, so a fill follows its outline, and one whose outline is opened waits
(`broken`) until it's closed again.

```python
f = s.fill(at=(5, 5), color="#ffc90e")   # ValueError if (5, 5) isn't enclosed
f.color = "#22b14c"                      # (filling the same area again recolours it too)
f.area, f.boundary, f.broken, f.at
s.fills                                  # bottom to top
s.loose_ends()                           # curve ends that meet nothing: where an outline is open
s.delete(f)
```

### Origin and axes
`s.origin`, `s.x_axis` and `s.y_axis` are fixed and permanent. Use them like any point or line:
`s.angle(s.x_axis, l, value=30)`, `s.distance(p, s.y_axis, value=5)`, `s.coincident(p, s.x_axis)`.
They aren't included in `s.entities` and the other lists.

### Sublayers
Everything (geometry, constraints, dimensions, texts) belongs to a sublayer; new items go to the
active one. All sublayers are solved together, so an item in one can be constrained to items in
another, and fills see the curves of every sublayer. In the app only the active sublayer is
selectable and editable; the others show faded, for snapping and constraining to (the Browser dock
on the left switches, hides, renames, reorders and deletes them).
```python
s.sublayers                       # in order: Sublayer handles
s.sublayer                        # the active one; s.sublayer = "Walls" (or a handle) switches
l = s.add_sublayer("Walls")       # becomes active; names are kept unique
l.name = "Outer"  l.visible = False  l.active  l.items
s.rename_sublayer("Outer", "Walls")   s.show_sublayer("Walls", False)
s.move_sublayer("Walls", 0)           # to that place in the order
s.move_to_sublayer([c, l2], "Walls")  # curves take their points; constraints wholly on them come too
s.delete_sublayer("Walls")            # and everything in it (not the last one)
item.sublayer
```
### Layers and projections
Layers sit above sublayers: each is a sketch of its own, drawn over the layers after it in
`s.layers` (the first is on top). Nothing in a layer may refer to another layer's geometry (lines,
constraints and texts on it are refused), except through **projections**: linked copies made with
`s.project`, which stay where their originals are (fixed), follow them when they change, and start
as construction (make them solid to bound fills). If an original is deleted its copies stay put,
unlinked. Fills are found per layer. The origin and axes are in every layer.
```python
s.layers                          # top first
s.layer                           # the active one; s.layer = "Top" (or a handle) switches
top = s.add_layer("Top")          # on top, with one sublayer; becomes active
top.name = "Detail"  top.visible = False  top.active  top.sublayers  top.fills
s.rename_layer("Detail", "Top")   s.show_layer("Top", False)   s.move_layer("Top", 1)
s.delete_layer("Top")             # and everything in it (not the last one)
copies = s.project([l13, c14])    # into the active layer; points they share are projected once
k.projected  k.linked  k.source   # a copy, whether it still follows, its original
s.break_link([copy])              # ordinary geometry again
item.layer
```
`s.sublayers` is the active layer's. Sublayer names are unique within a layer; where two layers
have one of the same name, `"Layer/Sublayer"` says which.
### Reading
```python
s.origin, s[12], s.points, s.lines, s.circles, s.arcs, s.entities, s.constraints, s.dimensions
p.x, p.y, p.pos     l.p1, l.p2, l.length     c.center, c.radius     a.start, a.end, a.radius
k.type, k.refs, k.value, k.is_dimension, k.is_feature      item.id, item.name, item.exists
s.dof                 r = s.solve()  ->  r.ok, r.dof, r.conflicting, r.redundant, r.fully_constrained
e.fully_constrained
```

### Annotations (dimensions and notes on the drawing)
Driven: they measure the geometry and never change it. They're shown in presentation view and
exported (SVG, PDF, DXF; `dimensions=False` on an export leaves them out).
```python
n = s.annotate("horizontal", line, at=(20, -8))       # also "aligned", "vertical" (two points, a point and a line, or a line)
s.annotate("diameter", circle, text="2× Ø<> THRU")     # <> is the value; "radius", "angle" ([l1, l2], flip=0-3)
s.annotate("arc_length", arc)
s.annotate("ordinate", point, along_y=True)            # from the origin (or [point, origin_point])
s.annotate("leader", curve, along=0.5, text="Polish")  # or a point; or tip=(x, y) with no refs
#   decimals= (None: the drawing's), text_height= (3.5 mm), width= (0.25 mm), color= ("#000000"), font=
s.edit_annotation(n, at=(20, -12), decimals=3)         # any of annotate's settings
n.kind, n.refs, n.value, n.label, n.at, n.text, n.decimals, n.text_height, n.width, n.color
s.annotations                                          # all of them
s.dimension_decimals = 1                               # the drawing's (2 to start with)
```

### Measuring
What the Measure tool shows. Nothing is changed.
```python
s.measure_length(curve)            # mm; a list of curves: their total (exact for arcs, ellipses, splines)
s.measure_between(a, b)            # (distance, dx, dy) from a to b: points, or (x, y)
s.measure_area(at=(x, y))          # mm^2 of the closed area there, its holes taken off; None if not enclosed
```

### Document and window
```python
doc.save(path)  doc.open(path)  doc.new()  doc.undo()  doc.redo()  doc.path  doc.modified
doc.export_svg(path, fills=True, undecorated=True, construction=False, margin=2)  # as presentation view, in mm
doc.export_dxf(path, styles=True, construction=False, fills=True)  # exact geometry, DXF 2004, mm
doc.export_pdf(path, fills=True, undecorated=True, construction=False, margin=2)  # the page, true size; text selectable
#   (text_as_outlines=True on any of them: text as its letters' shapes)
doc.to_svg(...), doc.to_dxf(...)                       # the same, as strings
doc.history      # the commands that built the sketch, one per edit (saved in the .setsq; undo
                 # takes commands off); run them in a new sketch to rebuild it
doc.to_json()
app.fit()  app.selection = [l, c]  app.tool = "line"  app.message("hi")   # needs the UI
```

`app.tool` accepts the tool names, which follow the API names:
- Geometry: select, line, point, project, rectangle, rectangle_3point, rectangle_center, circle, circle_2point, circle_3point, circle_tangent2, circle_tangent3, arc, arc3, arc_tangent
- Polygons and slots: polygon_inscribed, polygon_circumscribed, polygon_edge, slot, slot_overall, slot_center_point, arc_slot, arc_slot_3point
- Dimensions and constraints: dimension, coincident, horizontal_vertical, parallel, perpendicular, tangent, equal, midpoint, concentric, symmetric, collinear, fix

## Tests
Each `tests/scripts/test_*.py` is a CTest test run through `setsquare-cli`. The GUI test
*"Replaying recorded UI actions reproduces the sketch"* drives the real tools, replays what they
recorded, and checks the result is identical. It's there to keep "the UI and Python can do the
same things" true.
