#pragma once

#include "Geometry.h"
#include "Regions.h"
#include "Sketch.h"
#include "Style.h"

#include <map>

// Placing gradients on the drawing. A gradient's positions are fractions of the painted item's
// box (see core::Gradient); these find the boxes and turn the fractions into drawing
// coordinates, for the canvas and the exports alike.

namespace core::gradient
{

// An item's box: origin + fx*u + fy*v for fractions (fx, fy). Usually upright (u along x, v
// along y); a text's turns with it.
struct Frame
{
    Vec2 origin;
    Vec2 u{1, 0};
    Vec2 v{0, 1};

    Vec2 at(double fx, double fy) const { return {origin.x + fx * u.x + fy * v.x, origin.y + fx * u.y + fy * v.y}; }
    // The fractions of a drawing point (the inverse of at()).
    Vec2 fractions(Vec2 world) const;

    // An upright box round the points, at least `minSize` across each way (a straight line's
    // box has some height, so a gradient across it still has room).
    static Frame around(const std::vector<Vec2>& points, double pad = 0.0, double minSize = 1e-6);
};

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
    double aspect = 1.0;  // radial: the second axis over the first
    Vec2 focal;           // radial: where offset 0 is (kept inside the shape, as SVG would)
    // The focal point in the radial's own frame: x along the first axis, y along the second
    // (both in drawing units, before the aspect squash: a circle of `radius`).
    Vec2 focalLocal;
};

Placed place(const Gradient& g, const Frame& frame);

// The boxes of the items a gradient paints:
// a fill's area,
Frame fillFrame(const regions::Region& region);
// a text's letters (turning with it),
Frame textFrame(const Sketch& sketch, const Text& text);
// and each curve with a gradient stroke: the box round all the curves joined end to end with it
// that have the same stroke, so a gradient runs across a stroked outline as a whole rather than
// starting over on each side.
std::map<Id, Frame> strokeFrames(const Sketch& sketch);
// The same with the strokes as given (e.g. as they'd be after an edit).
std::map<Id, Frame> strokeFrames(const Sketch& sketch, const std::map<Id, StrokeStyle>& strokes);
// Those groups of curves (each sharing one gradient box).
std::vector<std::vector<Id>> strokeGroups(const Sketch& sketch, const std::map<Id, StrokeStyle>& strokes);
// The group a curve with a gradient stroke is in (just itself if it hasn't one).
std::vector<Id> strokeGroup(const Sketch& sketch, Id curve);

}  // namespace core::gradient
