#pragma once

#include "Geometry.h"
#include "Sketch.h"

#include <map>
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
    static constexpr double kArcStep = 3.141592653589793 / 48;
    // The areas of one layer's geometry (fills don't see other layers'). Outlines follow arcs in
    // steps of at most `arcStep` radians (finer for export).
    Arrangement(const Sketch& sketch, Id layer, double arcStep = kArcStep);

    // The smallest closed area around p, or nothing if p isn't enclosed.
    std::optional<Region> regionAt(Vec2 p) const;
    // Every closed area (each smallest one: the faces).
    std::vector<Region> regions() const;
    // The area outlined by exactly these curves (sorted ids), if there is one. Several areas can
    // share the same curves (two overlapping circles make three), so the one around `near`, or
    // else the closest to it.
    std::optional<Region> regionBounded(const std::vector<Id>& boundary, Vec2 near) const;
    // Curve ends that don't meet anything: where an outline may be open.
    std::vector<Vec2> looseEnds() const;

private:
    struct Cycle
    {
        std::vector<Vec2> polygon;
        std::vector<Id> curves;  // sorted, without dead ends
        double area = 0.0;       // signed: > 0 counter-clockwise (an area's outline)
        int component = 0;
        double x0 = 1e300, y0 = 1e300, x1 = -1e300, y1 = -1e300;  // its polygon's box
    };
    Region regionFor(size_t cycle) const;
    std::optional<size_t> smallestAround(Vec2 p, int exceptComponent) const;

    std::vector<Cycle> m_cycles;
    std::vector<Vec2> m_looseEnds;
};

// Each layer's arrangement, made when first asked for.
class Arrangements
{
public:
    explicit Arrangements(const Sketch& sketch, double arcStep = Arrangement::kArcStep)
        : m_sketch(sketch)
        , m_arcStep(arcStep)
    {}
    const Arrangement& of(Id layer) const;

private:
    const Sketch& m_sketch;
    double m_arcStep;
    mutable std::map<Id, Arrangement> m_byLayer;
};

// Where a fill is now: its outline if that's still there, else the area around its seed.
// (`arrangement` must be of the fill's layer.)
std::optional<Region> resolve(const Arrangement& arrangement, const Fill& fill);
std::optional<Region> resolve(const Arrangements& arrangements, const Fill& fill);

// After an edit: points each fill at its area as it now is (boundary curves, a seed inside), so
// it can be found again whichever way the geometry changes next. Fills that can't be found are
// left as they are.
void refreshFills(Sketch& sketch);

}  // namespace core::regions
