#pragma once

#include <cstdint>
#include <string>

// How geometry looks in the finished drawing: colours and line styles. Pure presentation; the
// solver never sees it.

namespace core
{

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

// A curve's line in the drawing.
struct StrokeStyle
{
    // `width` for no line at all: the curve isn't drawn in the finished drawing (presentation view,
    // SVG), though it's still part of it (it bounds fills). E.g. a fill without an outline.
    static constexpr double kNone = -1.0;

    Color color;
    double width = 0.35;  // millimetres on the drawing (it zooms with it); 0 = hairline, 1 pixel
    Dash dash = Dash::Solid;

    bool none() const { return width < 0.0; }
    bool operator==(const StrokeStyle&) const = default;
};

}  // namespace core
