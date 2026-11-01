#pragma once

#include "Geometry.h"
#include "Sketch.h"

#include <optional>
#include <vector>

// Closed areas of a drawing, for fills: what a paint bucket would fill. The solid curves
// (construction geometry and the axes are ignored) are cut where they cross into a planar
// graph, whose faces are the areas. An area can have holes (a circle inside a rectangle);
// curves sticking into it (loose ends) don't break it.

namespace core::regions
{

struct Region
{
    std::vector<Vec2> outer;               // the outline, counter-clockwise
    std::vector<std::vector<Vec2>> holes;  // islands inside it, clockwise
    std::vector<Id> boundary;              // curves along the outline (sorted)
    double area = 0.0;                     // inside the outline (holes not taken off)

    bool contains(Vec2 p) const;  // inside the outline and not in a hole
    Vec2 interiorPoint() const;   // a point well inside (away from every edge)
};

class Arrangement
{
public:
    // Outlines follow arcs in steps of at most `arcStep` radians (finer for export).
    explicit Arrangement(const Sketch& sketch, double arcStep = 3.141592653589793 / 48);

    // The smallest closed area around p, or nothing if p isn't enclosed.
    std::optional<Region> regionAt(Vec2 p) const;
    // The area outlined by exactly these curves (sorted ids), if there is one.
    std::optional<Region> regionBounded(const std::vector<Id>& boundary) const;
    // Curve ends that don't meet anything: where an outline may be open.
    std::vector<Vec2> looseEnds() const;

private:
    struct Cycle
    {
        std::vector<Vec2> polygon;
        std::vector<Id> curves;  // sorted, without dead ends
        double area = 0.0;       // signed: > 0 counter-clockwise (an area's outline)
        int component = 0;
    };
    Region regionFor(size_t cycle) const;
    std::optional<size_t> smallestAround(Vec2 p, int exceptComponent) const;

    std::vector<Cycle> m_cycles;
    std::vector<Vec2> m_looseEnds;
};

// Where a fill is now: its outline if that's still there, else the area around its seed.
std::optional<Region> resolve(const Arrangement& arrangement, const Fill& fill);

// After an edit: points each fill at its area as it now is (boundary curves, a seed inside), so
// it can be found again whichever way the geometry changes next. Fills that can't be found are
// left as they are.
void refreshFills(Sketch& sketch);

}  // namespace core::regions
