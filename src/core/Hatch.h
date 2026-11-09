#pragma once

#include "Geometry.h"
#include "Regions.h"
#include "Style.h"

#include <optional>
#include <string>
#include <vector>

// Hatches (see core::Hatch): the built-in patterns, and laying a hatch's lines out over an area,
// for the canvas and the exports alike.

namespace core::hatch
{

struct Pattern
{
    std::string name;  // as AutoCAD names it, where it has one (ANSI31, NET, ...)
    std::string description;
    std::vector<HatchLine> lines;  // in mm, at scale 1
};

// The built-in patterns, in the order they're offered.
const std::vector<Pattern>& builtins();
// A built-in by name (any case), turned and scaled; none if there's no such pattern.
std::optional<Hatch> builtin(const std::string& name, double angle = 0.0, double scale = 1.0, double width = 0.0);

// Its families of lines as drawn: turned and scaled.
std::vector<HatchLine> placed(const Hatch& hatch);

// The smallest distance between its lines (mm), to tell when they're too close to draw one by one.
double spacing(const Hatch& hatch);

// A piece of a hatch line; a == b: a dot.
struct Segment
{
    Vec2 a, b;
};

// Its lines across the box x0, y0 to x1, y1 (not cut to any area: for drawing with a clip).
// False, with `out` cleared, if there'd be more than `limit` pieces (too fine to draw so).
bool across(const Hatch& hatch, double x0, double y0, double x1, double y1, size_t limit, std::vector<Segment>& out);

// Its lines cut exactly to an area (its holes left out), e.g. for exporting. Same limit.
bool within(const Hatch& hatch, const regions::Region& area, size_t limit, std::vector<Segment>& out);

}  // namespace core::hatch
