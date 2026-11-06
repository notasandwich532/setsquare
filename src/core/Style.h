#pragma once

#include <cstdint>
#include <string>
#include <vector>

// How geometry looks in the finished drawing: colours and line styles. Pure presentation; the
// solver never sees it.

namespace core
{

// Entities and constraints share one id space; 0 is never a valid id.
using Id = std::uint32_t;
inline constexpr Id kNoId = 0;

struct Color
{
    std::uint8_t r = 0, g = 0, b = 0, a = 255;

    bool operator==(const Color&) const = default;
    std::string hex() const;  // "#rrggbb", or "#rrggbbaa" when not opaque
    // Accepts "#rgb", "#rrggbb" and "#rrggbbaa" (the # is optional). Returns false if malformed.
    static bool fromHex(const std::string& text, Color& out);
};

enum class Dash
{
    Solid,
    Dashed,
    Dotted,
    DashDot,
    DashDotDot,
};

const char* toString(Dash dash);  // "solid", "dashed", "dotted", "dash_dot", "dash_dot_dot"
bool dashFromString(const std::string& name, Dash& out);

// Gradients: what SVG has (linear and radial, any number of stops each with its own opacity, and
// pad / reflect / repeat past the ends), so they export exactly.
enum class GradientKind
{
    Linear,
    Radial,
};

// What happens past the gradient's ends.
enum class GradientSpread
{
    Pad,      // the end colours carry on
    Reflect,  // back and forth
    Repeat,   // over and over
};

const char* toString(GradientKind kind);      // "linear", "radial"
const char* toString(GradientSpread spread);  // "pad", "reflect", "repeat"
bool gradientKindFromString(const std::string& name, GradientKind& out);
bool gradientSpreadFromString(const std::string& name, GradientSpread& out);

struct GradientStop
{
    double offset = 0.0;  // 0..1 along the gradient
    Color color;
    bool operator==(const GradientStop&) const = default;
};

// A gradient's colours, and the sketch geometry that places it: it follows that geometry as it
// moves (constraints, dimensions, drags). Linear: from one point (offset 0) to another (offset 1).
// Radial: offset 1 on a circle, arc, ellipse or elliptical arc (an ellipse's gradient is
// elliptical too), offset 0 at its centre or at a focal point. See core/Gradient.h for placing it.
struct Gradient
{
    GradientKind kind = GradientKind::Linear;
    GradientSpread spread = GradientSpread::Pad;
    std::vector<GradientStop> stops;  // in offset order; none: no gradient (a plain colour)

    Id start = kNoId;  // linear: points
    Id end = kNoId;
    Id shape = kNoId;  // radial: the curve
    Id focal = kNoId;  // radial: a point (optional; else the centre)

    bool active() const { return !stops.empty(); }
    bool complete() const;        // has the geometry its kind needs
    std::vector<Id> refs() const;  // the geometry it uses (of its kind)
    Color colorAt(double offset) const;  // between the stops (no spread applied)
    Color average() const;               // one colour standing for it (e.g. for DXF)
    void sortStops();
    void reverse();  // the colours run the other way
    bool operator==(const Gradient&) const = default;
};

// A curve's line in the drawing.
struct StrokeStyle
{
    // `width` for no line at all: the curve isn't drawn in the finished drawing (presentation view,
    // SVG), though it's still part of it (it bounds fills). E.g. a fill without an outline.
    static constexpr double kNone = -1.0;

    Color color;
    double width = 0.35;  // millimetres on the drawing (it zooms with it); 0 = hairline, 1 pixel
    Dash dash = Dash::Solid;
    Gradient gradient;  // when active: the line is coloured by this rather than `color`

    bool none() const { return width < 0.0; }
    bool operator==(const StrokeStyle&) const = default;
};

}  // namespace core
