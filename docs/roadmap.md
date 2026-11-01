# Roadmap: remaining sketch features

Goal: Fusion's full sketch toolbox. Every tool ships with the matching Python API, recorder output
(`Editor` composite commands, so a tool records as one readable line) and tests: core DOF and
geometry tests, GUI tool tests, and Python scenario scripts.

## M6: Creation variants (done)
- **Point** tool.
- **Rectangles:** 3-point (rotated: perpendicular + parallel sides) and centre (construction
  diagonal, midpoint on the centre).
- **Circles:** 2-point, 3-point, tangent to 2 curves, tangent to 3 curves.
- **Tangent arc:** continues off a line or arc end.
- **Polygons:** inscribed, circumscribed, edge. A regular N-gon on a construction circle with equal
  sides. Side count is set in a tool-options box.
- **Slots:** centre-to-centre, overall, centre-point, 3-point arc, centre-point arc.
- **Collinear** constraint.
- **Toolbar** regrouped into Fusion-style dropdowns (Rectangle ▾, Circle ▾, Arc ▾, Polygon ▾,
  Slot ▾). Each button remembers the last variant used.

## M7: Trim family (done)
- **Curve-curve intersections** for lines, circles and arcs. Ellipse and spline intersections are
  added in M9.
- **Trim, Extend, Break.** New ends get coincident/point-on-curve constraints where they meet other
  geometry.
- **Fillet** (radius): the original curves stay full-length but become construction geometry, so
  the sharp corner and anything dimensioned to it survive. New solid segments run to the tangent
  points, sharing the far endpoints, and are joined by a tangent arc with a radius dimension.
- **Chamfer:** equal distance, distance-distance, and distance-angle, kept as construction the same
  way.

## M8: Transform family (done)
- **Feature constraints:** a pattern or offset is one constraint (with a value and settings) that
  the solver expands into per-point equations, including custom PlaneGCS constraints for rotated
  copies. Arc ends and smooth joins get exactly as many equations as they need, so linked copies
  add no freedom and no redundancy. Changing the value keeps the original in place.
- **Command panels** (Fusion-style) float over the canvas for Move/Copy, Scale, Mirror, the
  patterns and Offset: selection inputs ("4 selected"), values, OK/Cancel, with a live preview and
  draggable handles (distance arrows, rotation knobs). Fillet, Chamfer and Polygon settings are in
  small panels too; nothing is set from the toolbar.
- **Move/Copy** (M): X/Y distances and an angle, typed or dragged on the triad. The move drags the
  real geometry live; "Create copy" copies instead.
- **Clipboard** copy, cut and paste (Ctrl+C/X/V). The paste follows the cursor; internal
  constraints come along with the copy.
- **Scale** about a clicked point. Dimensions inside the selection scale with it.
- **Mirror** about a line (or axis), linked by symmetric constraints (equal for circles).
- **Rectangular and circular patterns,** linked: one spacing/angle value drives every copy.
  Rectangular: extent or spacing distribution, one-way or symmetric per direction, any direction
  line. Double-click a pattern in the constraints panel to edit it in its command; a new count
  regenerates the copies.
- **Offset,** linked: one distance drives the whole chain (lines go parallel, arcs concentric,
  corners meet).

## Appearance
Presentation is kept apart from the geometry: fills and strokes reference curves, the solver never
sees them, and per item styles live alongside the sketch (they go into layers later).

1. **Strokes (done).** Colour, width (mm on the drawing; 0 = hairline) and line style per curve.
   - Sketch view fades strokes under the geometry; Presentation view (V) shows the drawing (strokes
     at full strength, undecorated geometry as is, no constraints, dimensions or points), and
     drawing tools switch back to Sketch view.
   - Stroke brush (B): click a curve, double-click a chain; Ctrl+click removes, Alt+click picks up.
   - MS Paint-style colour bar: Colour 1 (strokes) / Colour 2 (fills), palette (left/right-click),
     custom colours, Edit colours, width and line style. Changes apply to the selected curves.
   - Strokes carry over to copies, mirrors, patterns, offsets and trimmed/filleted pieces.
2. **Fills (done).** Fill tool (G, a paint bucket): click a closed area to fill it with Colour 2
   (again to recolour; Ctrl+click removes, Alt+click picks up), with a hover preview. Areas come
   from a planar arrangement of the solid curves (cut where they cross; construction ignored),
   with islands as holes and stubs ignored. A fill stores its outline's curves and a point inside:
   found by its outline, else by the point; re-pointed after every edit; kept (broken) while its
   outline is open. Where nothing is enclosed, the loose ends are marked.
3. **Spill animation.** Optional animated raster flood fill that shows where a fill escapes through
   a gap, highlights the nearest loose ends and offers to close the gap.
4. **Layers panel.** Layers, each a sketch with ordered sublayers (show / fade / hide, lock).

## Export (done)
- **SVG** (File > Export): the drawing as presentation view shows it, in millimetres (y up), with
  fills (even-odd, holes kept), strokes (colour, opacity, width, dashes), exact lines, circles and
  arcs, plain curves in thin black, and optional construction; Inkscape layers.
- **DXF R12**: exact LINE / CIRCLE / ARC in millimetres for CAD and CAM; layers GEOMETRY, STROKES
  (nearest AutoCAD colour + linetype) and CONSTRUCTION (off). No fills, true colour or line weights
  (R12 has none); an R2000+ writer could add them later.
- Import (SVG/DXF into a sketch) is a later, bigger job.

## M9: New curve types
- **Ellipse** and elliptical arc, with major/minor radius dimensions.
- **Splines:** control-point and fit-point. Fit points are constrained onto the curve.
- **Conic curve:** a rational quadratic with a rho dimension.
- **Curvature (G2)** constraint at spline ends.
- Intersections, trim and picking for the new curves. Offset of ellipses and splines is out of
  scope; it would be approximate.

## M10: Text and parameters
- **Text:** anchored to a point, with height, angle and font. It can be constrained through its anchor.
- **Parameters table:** named values. Dimensions take expressions (`width/2 + 3`) that can refer to
  parameters and other dimensions (`d12`). Changes propagate; cycles are reported.
