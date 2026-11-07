#pragma once

#include "Geometry.h"
#include "Sketch.h"
#include "Style.h"

#include <map>
#include <optional>

// Placing gradients on the drawing from the sketch geometry they refer to (see core::Gradient),
// for the canvas and the exports alike.

namespace core::gradient
{

// A gradient in drawing coordinates.
struct Placed
{
    GradientKind kind = GradientKind::Linear;
    GradientSpread spread = GradientSpread::Pad;
    std::vector<GradientStop> stops;
    Vec2 start, end;      // linear: offset 0 to offset 1
    Vec2 center;          // radial
    double radius = 0.0;  // radial: along its first axis
    double angle = 0.0;   // radial: the first axis's direction (radians from +x)
    double aspect = 1.0;  // radial: the second axis over the first (an ellipse's)
    Vec2 focal;           // radial: where offset 0 is (kept inside the shape, as SVG would)
    // The focal point in the radial's own frame: x along the first axis, y along the second
    // (both in drawing units, before the aspect squash: a circle of `radius`).
    Vec2 focalLocal;
};

// None while its geometry is incomplete or gone (deleted): the item is then painted in the
// gradient's average colour until it's given geometry again.
std::optional<Placed> place(const Gradient& g, const Sketch& sketch);

// Where along a placed gradient a point is (0..1, with its spread applied past the ends), and so
// the colour it's painted there.
double offsetAt(const Placed& p, Vec2 world);
Color colorAt(const Placed& p, Vec2 world);

// What each part of a gradient can be: a point (linear ends, focal point) or a round or
// elliptical curve (radial: circle, arc, ellipse, elliptical arc).
bool canBePoint(const Sketch& sketch, Id id);
bool canBeShape(const Sketch& sketch, Id id);

// Throws SketchError unless the gradient's geometry is of the right kinds and in `layer` (that
// of the item it paints; the origin belongs to every layer).
void check(const Sketch& sketch, const Gradient& g, Id layer);

// For copies: the gradient's geometry through `map` (old id -> new). What isn't in it stays as
// it is when copying within one sketch, or is dropped when it came from another (pasting).
void remap(Gradient& g, const std::map<Id, Id>& map, bool sameSketch);

// Curves with the same gradient stroke joined end to end, as groups (a stroked outline): the
// gradient tool changes such a group's gradient together.
std::vector<std::vector<Id>> strokeGroups(const Sketch& sketch);
std::vector<Id> strokeGroup(const Sketch& sketch, Id curve);  // just itself if it's in none

}  // namespace core::gradient
