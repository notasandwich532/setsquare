#pragma once

#include "Geometry.h"
#include "Regions.h"
#include "Sketch.h"

#include <optional>
#include <vector>

// Measuring a drawing (the Measure tool, and scripts): curve lengths, how far apart two points
// are, and closed areas. Nothing here changes the sketch.

namespace core::measuring
{

// The total length of the curves (lines, arcs, circles, ellipses, splines); points add nothing.
double length(const Sketch& sketch, const std::vector<Id>& curves);

// From one point to another: the straight-line distance and the differences in x and y (b - a).
struct Separation
{
    double distance = 0.0;
    double dx = 0.0;
    double dy = 0.0;
};
Separation between(Vec2 a, Vec2 b);
Separation between(const Sketch& sketch, Id a, Id b);  // two points of the sketch

// The angle between two lines, as they're drawn: at the point where they meet (or would, made
// longer), from one towards the other, each taken from there out along it (to its far end). So
// a corner's inside angle, or a taper's narrow one. `other` is the rest of a half turn.
// Parallel lines: 0, with no vertex.
struct Angle
{
    double degrees = 0.0;  // 0 to 180
    double other = 180.0;  // 180 - degrees
    bool parallel = false;
    Vec2 vertex;           // where they meet (not when parallel)
    Vec2 dir1, dir2;       // unit directions from the vertex out along each line
};
Angle angle(const Sketch& sketch, Id line1, Id line2);

// The closed area around p, of a layer's geometry (what a fill there would cover), its outline
// following curves closely enough that its area is good to about a millionth.
std::optional<regions::Region> areaAt(const Sketch& sketch, Id layer, Vec2 p);
// An area's size: inside its outline, less its holes.
double area(const regions::Region& region);

}  // namespace core::measuring
