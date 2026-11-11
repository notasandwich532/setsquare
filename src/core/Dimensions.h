#pragma once

#include "Geometry.h"
#include "Sketch.h"

#include <vector>

// Measuring and laying out dimensions. Shared by the UI (drawing, placing) and anything that
// needs "what is this dimension currently", e.g. creating a dimension at the current size.

namespace core
{

// Current value of a dimension as the geometry stands (same units as Constraint::value:
// positive lengths, angles in degrees in [0, 180]; a ratio constraint's first-over-second). The
// refs must be valid for the type.
double measure(const Sketch& sketch, ConstraintType type, const std::vector<Id>& refs, std::uint8_t flip = 0);

// For an angle between two lines: the Constraint::flip that selects the angle (of the four
// between them) whose sector contains `at`. Lines must not be parallel.
std::uint8_t angleFlipAt(const Sketch& sketch, Id line1, Id line2, Vec2 at);

struct DimensionLayout
{
    enum class Style
    {
        Linear,    // a, b measured along dir
        Radius,    // center + radius
        Diameter,  // center + radius
        Angle,     // vertex at center, arc from startAngle sweeping sweep (radians, signed)
    };

    bool valid = false;
    Style style = Style::Linear;
    Vec2 anchor;  // label = anchor + (labelX, labelY)
    Vec2 label;

    Vec2 a, b, dir;  // Linear
    // Linear, from a point to a line: b is the foot on the (infinite) line; this is the line's
    // own extent, so its extension line only reaches past the line's end, not along it.
    bool bOnLine = false;
    Vec2 lineA, lineB;
    // Likewise a, when the point is the end of a line parallel to that one (say a corner of an
    // outline measured to a side of a box inside it).
    bool aOnLine = false;
    Vec2 aLineA, aLineB;

    Vec2 center;  // Radius, Diameter, Angle
    double radius = 0.0;
    double startAngle = 0.0;
    double sweep = 0.0;
};

DimensionLayout layoutDimension(const Sketch& sketch, const Constraint& dimension);

// The label offset that puts a dimension's label at `label` (inverse of layoutDimension).
Vec2 labelOffsetFor(const Sketch& sketch, const Constraint& dimension, Vec2 label);

}  // namespace core
