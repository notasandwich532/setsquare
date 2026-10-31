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

## M7: Trim family
- **Curve-curve intersections** for lines, circles and arcs. Ellipse and spline intersections are
  added in M9.
- **Trim, Extend, Break.** New ends get coincident/point-on-curve constraints where they meet other
  geometry.
- **Fillet** (radius): the original curves stay full-length but become construction geometry, so
  the sharp corner and anything dimensioned to it survive. New solid segments run to the tangent
  points, sharing the far endpoints, and are joined by a tangent arc with a radius dimension.
- **Chamfer:** equal distance, distance-distance, and distance-angle, kept as construction the same
  way.

## M8: Transform family
- **Solver support:** custom PlaneGCS constraints (translation, rotation, scale relationships)
  whose parameters can be shared between many constraints and driven by one dimension.
- **Move/Copy** tool: translate or rotate a selection, with a "create copy" option.
- **Clipboard** copy and paste (Ctrl+C/V). The paste follows the cursor; internal constraints come
  along with the copy.
- **Scale** about a point. Dimensions inside the selection scale with it.
- **Mirror** about a line, linked by symmetric constraints.
- **Rectangular and circular patterns,** linked: one spacing/angle parameter drives every copy.
  Changing the count regenerates the copies.
- **Offset,** linked: one offset dimension drives the whole chain (lines go parallel, arcs
  concentric, corners meet).

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
