#pragma once

#include "Geometry.h"
#include "Sketch.h"

#include <array>
#include <vector>

// Composite shapes: each builder adds the geometry and the constraints that make it behave as
// one shape, using a minimal independent set, so a freshly built shape is never reported
// redundant. Degrees of freedom of each (with its input points free) are noted below and
// covered by tests. Builders throw SketchError for degenerate input.

namespace core::build
{

struct Rectangle
{
    std::array<Id, 4> corners{};  // in order around the rectangle
    std::array<Id, 4> sides{};    // side i joins corner i and i+1
    Id center = kNoId;            // centre rectangle only
    std::array<Id, 2> diagonals{};  // centre rectangle only (construction)
};

// Axis-aligned from two opposite corners. DOF 4.
Rectangle rectangle(Sketch& s, Id corner, Id opposite);
// Rotated: first side a->b, the other side reaching to widthPoint. DOF 5.
Rectangle rectangle3(Sketch& s, Id a, Id b, Vec2 widthPoint);
// Axis-aligned about a centre point, with construction diagonals. DOF 4.
Rectangle rectangleCenter(Sketch& s, Id center, Vec2 corner);

// Circle with a and b at the ends of a diameter. DOF 3.
Id circle2(Sketch& s, Vec2 a, Vec2 b);
// Circle through three points. DOF 3.
Id circle3(Sketch& s, Vec2 a, Vec2 b, Vec2 c);
// Circle tangent to 2 or 3 curves, on the side nearest `near`. With 2 curves, `radius` sets
// the size (ignored between parallel lines). DOF 1 (two curves) or 0 (three).
Id circleTangent(Sketch& s, const std::vector<Id>& curves, Vec2 near, double radius = 0.0);

// Arc continuing tangentially from `start`, an end of `curve` (a line or arc), to `end`.
Id arcTangent(Sketch& s, Id curve, Id start, Id end);

enum class PolygonKind
{
    Inscribed,      // vertices on the circle; `point` is a vertex
    Circumscribed,  // sides tangent to the circle; `point` is a side's midpoint
};

struct Polygon
{
    Id center = kNoId;
    Id circle = kNoId;       // construction, through the corners
    Id innerCircle = kNoId;  // construction, touching the sides (circumscribed only)
    std::vector<Id> vertices;
    std::vector<Id> sides;
};

// Regular polygon about a centre point. DOF 4 (centre, size, rotation).
Polygon polygon(Sketch& s, Id center, Vec2 point, int sides, PolygonKind kind);
// Regular polygon built on the edge a->b, on its left (or right). DOF 4.
Polygon polygonEdge(Sketch& s, Id a, Id b, int sides, bool left = true);

struct Slot
{
    std::vector<Id> curves;   // the outline: caps and sides
    Id centerline = kNoId;    // construction line or arc
    std::array<Id, 2> centers{};
    Id midpoint = kNoId;      // centre-point slot only
};

// Straight slots. DOF 5 (two centres, width).
Slot slot(Sketch& s, Id center1, Id center2, double width);
Slot slotOverall(Sketch& s, Vec2 end1, Vec2 end2, double width);
Slot slotCenterPoint(Sketch& s, Id center, Vec2 capCenter, double width);  // DOF 5

// Arc slots along a counter-clockwise centreline arc. DOF 6 (centreline arc, width).
Slot arcSlot(Sketch& s, Id center, Vec2 start, Vec2 end, double width);
Slot arcSlot3(Sketch& s, Vec2 start, Vec2 end, Vec2 through, double width);

}  // namespace core::build
