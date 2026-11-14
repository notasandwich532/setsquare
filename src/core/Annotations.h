#pragma once

#include "Geometry.h"
#include "Sketch.h"

#include <array>
#include <string>
#include <vector>

// Drawn dimensions (see DrawnDimension in Sketch.h): what they measure, what they say, and how
// they're drawn, as plain lines, arcs, arrowheads and a line of text in millimetres. The canvas
// and every export draw them from this, so they all look the same.

namespace core::annotation
{

// Text heights are capital letter heights; fonts are sized by their em, about this much taller.
constexpr double kCapHeight = 0.7;  // capital height / em (Arial's is 0.716)

// The value it shows: lengths in mm, angles in degrees (ordinates signed). 0 for a leader.
double value(const Sketch& sketch, const DrawnDimension& d);
// The value as written: `decimals` places (the drawing's, unless the dimension says).
std::string number(const Sketch& sketch, const DrawnDimension& d);
// What it says: its text with <> replaced by the number.
std::string label(const Sketch& sketch, const DrawnDimension& d);
// The text a new one of a kind starts with: "<>", "R<>", "Ø<>", "<>°", "⌒<>"; a leader's "Note".
std::string defaultText(DrawnDimensionKind kind);

struct Graphics
{
    bool valid = false;
    std::vector<std::array<Vec2, 2>> lines;
    struct Arc
    {
        Vec2 center;
        double radius = 0.0;
        double start = 0.0;  // radians
        double sweep = 0.0;  // radians, signed (counter-clockwise positive)
    };
    std::vector<Arc> arcs;
    std::vector<std::array<Vec2, 3>> arrows;  // filled triangles, the tip first
    struct Text
    {
        std::string text;
        Vec2 at;             // on its baseline, at its left end, middle or right end (`align`)
        double angle = 0.0;  // degrees, its baseline's direction (kept readable: -90 < angle <= 90)
        double height = 0.0; // capital height, mm
        int align = 1;       // 0 left, 1 centre, 2 right
    } text;
    Vec2 label;  // where its label is placed (what dragging it moves)
};
Graphics graphics(const Sketch& sketch, const DrawnDimension& d);

// The label offset (DrawnDimension::labelX, labelY) that puts its label at `at`.
Vec2 labelOffsetFor(const Sketch& sketch, const DrawnDimension& d, Vec2 at);

}  // namespace core::annotation
