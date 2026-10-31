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
    Color color;
    double width = 0.35;  // millimetres on the drawing (it zooms with it); 0 = hairline, 1 pixel
    Dash dash = Dash::Solid;

    bool operator==(const StrokeStyle&) const = default;
};

}  // namespace core
