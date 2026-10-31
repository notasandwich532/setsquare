#pragma once

#include "Sketch.h"

#include <set>
#include <vector>

// Geometric queries on a sketch: distances, picking and box selection. Pure functions in world
// units, so they can be used by the UI, scripts and tests alike.

namespace core
{

struct Vec2
{
    double x = 0.0;
    double y = 0.0;
};

// Resolved arc: angles in radians, counter-clockwise sweep in (0, 2*pi].
struct ArcShape
{
    Vec2 center;
    double radius = 0.0;
    double startAngle = 0.0;
    double sweep = 0.0;
};

ArcShape arcShape(const Sketch& sketch, Id arc);

// Circle through three points; returns false if they are (nearly) collinear.
bool circumcircle(Vec2 a, Vec2 b, Vec2 c, Vec2& center, double& radius);

// Shortest distance from p to the entity (to the curve itself for circles and arcs).
double distanceTo(const Sketch& sketch, Id entity, Vec2 p);

struct PickOptions
{
    double tolerance = 1.0;  // world units
    bool points = true;
    bool curves = true;
    std::set<Id> exclude;
};

// Nearest entity within tolerance; points win over curves so endpoints stay grabbable.
Id pick(const Sketch& sketch, Vec2 p, const PickOptions& options);

// Window selection (crossing = false) takes entities entirely inside the box; crossing
// selection also takes anything the box touches.
std::vector<Id> boxSelect(const Sketch& sketch, Vec2 corner1, Vec2 corner2, bool crossing);

// Polyline approximation of an entity (a single vertex for points).
std::vector<Vec2> sample(const Sketch& sketch, Id entity, int segments = 64);

}  // namespace core
