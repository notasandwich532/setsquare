#include "Style.h"

#include <cctype>
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

}  // namespace core
