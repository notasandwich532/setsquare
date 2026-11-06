#include "Style.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>

namespace core
{

std::string Color::hex() const
{
    char buf[10];
    if (a == 255) {
        std::snprintf(buf, sizeof buf, "#%02x%02x%02x", r, g, b);
    }
    else {
        std::snprintf(buf, sizeof buf, "#%02x%02x%02x%02x", r, g, b, a);
    }
    return buf;
}

bool Color::fromHex(const std::string& text, Color& out)
{
    std::string h = !text.empty() && text[0] == '#' ? text.substr(1) : text;
    for (char c : h) {
        if (!std::isxdigit(static_cast<unsigned char>(c))) {
            return false;
        }
    }
    if (h.size() == 3) {  // #rgb -> #rrggbb
        h = {h[0], h[0], h[1], h[1], h[2], h[2]};
    }
    if (h.size() != 6 && h.size() != 8) {
        return false;
    }
    auto byte = [&](size_t i) { return static_cast<std::uint8_t>(std::stoi(h.substr(i, 2), nullptr, 16)); };
    out = Color{byte(0), byte(2), byte(4), h.size() == 8 ? byte(6) : std::uint8_t(255)};
    return true;
}

const char* toString(Dash dash)
{
    switch (dash) {
        case Dash::Solid: return "solid";
        case Dash::Dashed: return "dashed";
        case Dash::Dotted: return "dotted";
        case Dash::DashDot: return "dash_dot";
        case Dash::DashDotDot: return "dash_dot_dot";
    }
    return "solid";
}

bool dashFromString(const std::string& name, Dash& out)
{
    for (Dash d : {Dash::Solid, Dash::Dashed, Dash::Dotted, Dash::DashDot, Dash::DashDotDot}) {
        if (name == toString(d)) {
            out = d;
            return true;
        }
    }
    return false;
}

const char* toString(GradientKind kind)
{
    return kind == GradientKind::Radial ? "radial" : "linear";
}

const char* toString(GradientSpread spread)
{
    switch (spread) {
        case GradientSpread::Pad: return "pad";
        case GradientSpread::Reflect: return "reflect";
        case GradientSpread::Repeat: return "repeat";
    }
    return "pad";
}

bool gradientKindFromString(const std::string& name, GradientKind& out)
{
    for (GradientKind k : {GradientKind::Linear, GradientKind::Radial}) {
        if (name == toString(k)) {
            out = k;
            return true;
        }
    }
    return false;
}

bool gradientSpreadFromString(const std::string& name, GradientSpread& out)
{
    for (GradientSpread s : {GradientSpread::Pad, GradientSpread::Reflect, GradientSpread::Repeat}) {
        if (name == toString(s)) {
            out = s;
            return true;
        }
    }
    return false;
}

namespace
{
Color mix(const Color& a, const Color& b, double t)
{
    auto ch = [t](std::uint8_t x, std::uint8_t y) {
        return static_cast<std::uint8_t>(std::lround(x + (y - x) * t));
    };
    return {ch(a.r, b.r), ch(a.g, b.g), ch(a.b, b.b), ch(a.a, b.a)};
}
}  // namespace

bool Gradient::complete() const
{
    return kind == GradientKind::Linear ? start != kNoId && end != kNoId : shape != kNoId;
}

std::vector<Id> Gradient::refs() const
{
    std::vector<Id> out;
    for (Id id : kind == GradientKind::Linear ? std::vector<Id>{start, end} : std::vector<Id>{shape, focal}) {
        if (id != kNoId) {
            out.push_back(id);
        }
    }
    return out;
}

Color Gradient::colorAt(double offset) const
{
    if (stops.empty()) {
        return {};
    }
    if (offset <= stops.front().offset) {
        return stops.front().color;
    }
    for (size_t i = 1; i < stops.size(); ++i) {
        const GradientStop& a = stops[i - 1];
        const GradientStop& b = stops[i];
        if (offset <= b.offset) {
            const double span = b.offset - a.offset;
            return span <= 0.0 ? b.color : mix(a.color, b.color, (offset - a.offset) / span);
        }
    }
    return stops.back().color;
}

Color Gradient::average() const
{
    if (stops.empty()) {
        return {};
    }
    double r = 0, g = 0, b = 0, a = 0;
    constexpr int kSamples = 16;
    for (int i = 0; i < kSamples; ++i) {
        const Color c = colorAt((i + 0.5) / kSamples);
        r += c.r;
        g += c.g;
        b += c.b;
        a += c.a;
    }
    auto ch = [](double v) { return static_cast<std::uint8_t>(std::lround(v / kSamples)); };
    return {ch(r), ch(g), ch(b), ch(a)};
}

void Gradient::sortStops()
{
    for (GradientStop& s : stops) {
        s.offset = std::clamp(s.offset, 0.0, 1.0);
    }
    std::stable_sort(stops.begin(), stops.end(),
                     [](const GradientStop& a, const GradientStop& b) { return a.offset < b.offset; });
}

void Gradient::reverse()
{
    for (GradientStop& s : stops) {
        s.offset = 1.0 - s.offset;
    }
    std::reverse(stops.begin(), stops.end());
}

}  // namespace core
