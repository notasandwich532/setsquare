#include "Annotations.h"

#include "Curves.h"
#include "Dimensions.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <optional>

namespace core::annotation
{

namespace
{
constexpr double kPi = 3.141592653589793;

Vec2 sub(Vec2 a, Vec2 b) { return {a.x - b.x, a.y - b.y}; }
Vec2 add(Vec2 a, Vec2 b) { return {a.x + b.x, a.y + b.y}; }
Vec2 mul(Vec2 a, double k) { return {a.x * k, a.y * k}; }
double dot(Vec2 a, Vec2 b) { return a.x * b.x + a.y * b.y; }
double norm(Vec2 a) { return std::hypot(a.x, a.y); }
Vec2 unit(Vec2 a, Vec2 fallback = {1, 0})
{
    const double n = norm(a);
    return n > 1e-12 ? Vec2{a.x / n, a.y / n} : fallback;
}
Vec2 perp(Vec2 a) { return {-a.y, a.x}; }
Vec2 polar(double angle) { return {std::cos(angle), std::sin(angle)}; }
double sign(double v) { return v < 0 ? -1.0 : 1.0; }

Vec2 pos(const Sketch& s, Id point)
{
    const Point& p = s.point(point);
    return {p.x, p.y};
}

std::optional<ConstraintType> constraintKind(DrawnDimensionKind kind)
{
    switch (kind) {
        case DrawnDimensionKind::Aligned: return ConstraintType::Distance;
        case DrawnDimensionKind::Horizontal: return ConstraintType::HorizontalDistance;
        case DrawnDimensionKind::Vertical: return ConstraintType::VerticalDistance;
        case DrawnDimensionKind::Radius: return ConstraintType::Radius;
        case DrawnDimensionKind::Diameter: return ConstraintType::Diameter;
        case DrawnDimensionKind::Angle: return ConstraintType::Angle;
        default: return std::nullopt;
    }
}

// The dimension constraint with the same references and label (to lay out as one would be).
Constraint asConstraint(const DrawnDimension& d, ConstraintType type)
{
    Constraint c;
    c.type = type;
    c.refs = d.refs;
    c.labelX = d.labelX;
    c.labelY = d.labelY;
    c.flip = d.flip;
    return c;
}

// Where a leader points.
Vec2 leaderTip(const Sketch& s, const DrawnDimension& d)
{
    if (d.refs.empty()) {
        return {d.tip.x, d.tip.y};
    }
    if (s.entity(d.refs[0]).kind() == EntityKind::Point) {
        return pos(s, d.refs[0]);
    }
    return curve::pathOf(s, d.refs[0]).at(d.along);
}

// The point its label's offset is from (for the kinds laid out here).
Vec2 anchorOf(const Sketch& s, const DrawnDimension& d)
{
    switch (d.kind) {
        case DrawnDimensionKind::ArcLength: return arcShape(s, d.refs[0]).center;
        case DrawnDimensionKind::Ordinate: return pos(s, d.refs[0]);
        default: return leaderTip(s, d);
    }
}

// The label's default offset (for the kinds laid out here).
Vec2 defaultOffset(const Sketch& s, const DrawnDimension& d)
{
    const double h = d.textHeight;
    switch (d.kind) {
        case DrawnDimensionKind::ArcLength: {
            const ArcShape a = arcShape(s, d.refs[0]);
            return mul(polar(a.startAngle + a.sweep / 2), a.radius + 2.5 * h);
        }
        default: return {4 * h, 3 * h};
    }
}

// An angle in degrees, turned to read left to right: in (-90, 90].
double readable(double degrees)
{
    double a = std::fmod(degrees, 360.0);
    if (a > 180) {
        a -= 360;
    }
    if (a <= -180) {
        a += 360;
    }
    if (a > 90) {
        a -= 180;
    }
    if (a <= -90) {
        a += 180;
    }
    return a;
}

// Sizes, from the text's: arrowheads, the gap left between the drawing and an extension line, and
// how far extension lines run past the dimension line.
struct Sizes
{
    explicit Sizes(double h)
        : h(h)
        , arrow(0.8 * h)
        , halfWidth(0.8 * h * 0.18)
        , gap(0.25 * h)
        , over(0.5 * h)
        , textGap(0.3 * h)
    {}
    double h, arrow, halfWidth, gap, over, textGap;
};

struct Builder
{
    Graphics& g;
    Sizes z;

    void line(Vec2 a, Vec2 b) { g.lines.push_back({a, b}); }
    // An arrowhead with its tip at `tip`, its body back along `back` (a unit vector).
    void arrow(Vec2 tip, Vec2 back)
    {
        const Vec2 base = add(tip, mul(back, z.arrow));
        const Vec2 side = mul(perp(back), z.halfWidth);
        g.arrows.push_back({tip, add(base, side), sub(base, side)});
    }
    // A line of text centred on `at`, along `direction` (radians), kept readable, on the side of
    // `at` that `outward` points to (a unit vector; the text's middle is textGap + h/2 out).
    void textBeside(Vec2 at, double direction, Vec2 outward, const std::string& text)
    {
        const double angle = readable(direction * 180.0 / kPi);
        const Vec2 up = perp(polar(angle * kPi / 180.0));
        const Vec2 middle = add(at, mul(outward, z.textGap + z.h / 2));
        g.text = {text, sub(middle, mul(up, z.h / 2)), angle, z.h, 1};
    }
    // A line of text level, starting (or ending, `side` < 0) at the end of a leader's landing.
    void textLevel(Vec2 at, double side, const std::string& text)
    {
        g.text = {text, Vec2{at.x + side * 0.4 * z.h, at.y - z.h / 2}, 0.0, z.h, side > 0 ? 0 : 2};
    }
    // A level landing from `at` (the end of a leader), then the text.
    void landing(Vec2 at, double side, const std::string& text)
    {
        const Vec2 end{at.x + side * z.h, at.y};
        line(at, end);
        textLevel(end, side, text);
    }
};

// Radial positions: the angle-like dimensions (angles, arc lengths) draw an arc with arrowheads at
// its ends, and their text outside its middle.
void arcWithArrows(Builder& b, Vec2 c, double r, double start, double sweep, const std::string& text)
{
    b.g.arcs.push_back({c, r, start, sweep});
    const double end = start + sweep;
    const double turn = sign(sweep);
    const Vec2 p1 = add(c, mul(polar(start), r)), p2 = add(c, mul(polar(end), r));
    b.arrow(p1, mul(perp(polar(start)), turn));    // (back along the arc)
    b.arrow(p2, mul(perp(polar(end)), -turn));
    const double middle = start + sweep / 2;
    b.textBeside(add(c, mul(polar(middle), r)), middle + kPi / 2, polar(middle), text);
}

// An extension line along a ray from `c`, from past `reach` (the drawing's extent along it) to
// past `r` (the dimension's arc).
void radialExtension(Builder& b, Vec2 c, Vec2 ray, double reach, double r)
{
    if (r > reach + b.z.gap) {
        b.line(add(c, mul(ray, reach + b.z.gap)), add(c, mul(ray, r + b.z.over)));
    }
    else if (r < reach - b.z.gap) {
        b.line(add(c, mul(ray, reach - b.z.gap)), add(c, mul(ray, r - b.z.over)));
    }
}

void linear(Builder& b, const Sketch& s, const DrawnDimension& d, ConstraintType type, const std::string& text)
{
    const DimensionLayout L = layoutDimension(s, asConstraint(d, type));
    if (!L.valid) {
        return;
    }
    const Vec2 n = perp(L.dir);
    const Vec2 a2 = add(L.a, mul(n, dot(sub(L.label, L.a), n)));
    const Vec2 b2 = add(L.b, mul(n, dot(sub(L.label, L.b), n)));
    // Extension lines: from the drawing (with a gap) to past the dimension line; for an end on a
    // line running that way, from the line's nearer end only.
    auto extension = [&](Vec2 from, Vec2 to, bool onLine, Vec2 la, Vec2 lb) {
        if (onLine) {
            const Vec2 dl = sub(lb, la);
            const double l2 = dot(dl, dl);
            const double t = l2 > 0 ? std::clamp(dot(sub(to, la), dl) / l2, 0.0, 1.0) : 0.0;
            from = add(la, mul(dl, t));
        }
        const Vec2 v = sub(to, from);
        if (norm(v) > b.z.gap) {
            const Vec2 e = unit(v);
            b.line(add(from, mul(e, b.z.gap)), add(to, mul(e, b.z.over)));
        }
    };
    extension(L.a, a2, L.aOnLine, L.aLineA, L.aLineB);
    extension(L.b, b2, L.bOnLine, L.lineA, L.lineB);

    const double length = norm(sub(b2, a2));
    const Vec2 e = unit(sub(b2, a2), L.dir);
    if (length >= 2.5 * b.z.arrow) {
        b.line(a2, b2);
        b.arrow(a2, e);
        b.arrow(b2, mul(e, -1));
    }
    else {
        // Too short for the arrowheads inside: outside, pointing in.
        b.line(sub(a2, mul(e, 1.8 * b.z.arrow)), add(b2, mul(e, 1.8 * b.z.arrow)));
        b.arrow(a2, mul(e, -1));
        b.arrow(b2, e);
    }
    // The text where the label is along the line (the line reaching it, if that's past an end).
    const double t = dot(sub(L.label, a2), e);
    const Vec2 at = add(a2, mul(e, t));
    if (t < 0) {
        b.line(a2, at);
    }
    else if (t > length) {
        b.line(b2, at);
    }
    const double angle = std::atan2(e.y, e.x);
    const double reading = readable(angle * 180 / kPi) * kPi / 180;
    b.textBeside(at, angle, perp(polar(reading)), text);
    b.g.label = L.label;
}

void round(Builder& b, const Sketch& s, const DrawnDimension& d, ConstraintType type, const std::string& text)
{
    const DimensionLayout L = layoutDimension(s, asConstraint(d, type));
    if (!L.valid) {
        return;
    }
    const Vec2 c = L.center;
    const double r = L.radius;
    const Vec2 e = unit(sub(L.label, c));
    const bool outside = norm(sub(L.label, c)) > r;
    const Vec2 p = add(c, mul(e, r));
    const double side = sign(e.x);
    if (type == ConstraintType::Radius) {
        if (outside) {
            b.line(p, L.label);
            b.arrow(p, e);  // (from outside, pointing in at the curve)
            b.landing(L.label, side, text);
        }
        else {
            b.line(c, p);
            b.arrow(p, mul(e, -1));
            b.textBeside(L.label, std::atan2(e.y, e.x), perp(polar(readable(std::atan2(e.y, e.x) * 180 / kPi) * kPi / 180)), text);
        }
    }
    else {
        const Vec2 q = sub(c, mul(e, r));
        if (outside) {
            b.line(q, L.label);
            b.arrow(p, e);
            b.arrow(q, e);
            b.landing(L.label, side, text);
        }
        else {
            b.line(q, p);
            b.arrow(p, mul(e, -1));
            b.arrow(q, e);
            b.textBeside(L.label, std::atan2(e.y, e.x), perp(polar(readable(std::atan2(e.y, e.x) * 180 / kPi) * kPi / 180)), text);
        }
    }
    b.g.label = L.label;
}

void angle(Builder& b, const Sketch& s, const DrawnDimension& d, const std::string& text)
{
    const DimensionLayout L = layoutDimension(s, asConstraint(d, ConstraintType::Angle));
    if (!L.valid || L.radius <= 0) {
        return;
    }
    // Extension lines where the arc is beyond (or short of) the lines it measures.
    const double rays[2] = {L.startAngle, L.startAngle + L.sweep};
    for (int i = 0; i < 2; ++i) {
        const Line& l = s.line(d.refs[static_cast<size_t>(i)]);
        const Vec2 ray = polar(rays[i]);
        const double reach =
            std::max(dot(sub(pos(s, l.p1), L.center), ray), dot(sub(pos(s, l.p2), L.center), ray));
        const double nearest =
            std::max(0.0, std::min(dot(sub(pos(s, l.p1), L.center), ray), dot(sub(pos(s, l.p2), L.center), ray)));
        if (L.radius > reach) {
            radialExtension(b, L.center, ray, reach, L.radius);
        }
        else if (L.radius < nearest) {
            radialExtension(b, L.center, ray, nearest, L.radius);
        }
    }
    arcWithArrows(b, L.center, L.radius, L.startAngle, L.sweep, text);
    b.g.label = L.label;
}

void arcLength(Builder& b, const Sketch& s, const DrawnDimension& d, const std::string& text)
{
    const ArcShape a = arcShape(s, d.refs[0]);
    const Vec2 label = add(a.center, d.labelX == 0 && d.labelY == 0 ? defaultOffset(s, d) : Vec2{d.labelX, d.labelY});
    const double r = norm(sub(label, a.center));
    if (r <= 0) {
        return;
    }
    radialExtension(b, a.center, polar(a.startAngle), a.radius, r);
    radialExtension(b, a.center, polar(a.startAngle + a.sweep), a.radius, r);
    arcWithArrows(b, a.center, r, a.startAngle, a.sweep, text);
    b.g.label = label;
}

void leader(Builder& b, const Sketch& s, const DrawnDimension& d, const std::string& text)
{
    // (An ordinate: to its point, saying where it is.)
    const Vec2 tip = d.kind == DrawnDimensionKind::Ordinate ? pos(s, d.refs[0]) : leaderTip(s, d);
    const Vec2 label = add(tip, d.labelX == 0 && d.labelY == 0 ? defaultOffset(s, d) : Vec2{d.labelX, d.labelY});
    const double side = sign(label.x - tip.x);
    b.line(tip, label);
    b.arrow(tip, unit(sub(label, tip)));
    b.landing(label, side, text);
    b.g.label = label;
}
}  // namespace

double value(const Sketch& s, const DrawnDimension& d)
{
    if (const auto type = constraintKind(d.kind)) {
        return measure(s, *type, d.refs, d.flip);
    }
    switch (d.kind) {
        case DrawnDimensionKind::ArcLength: {
            const ArcShape a = arcShape(s, d.refs[0]);
            return a.radius * std::abs(a.sweep);
        }
        case DrawnDimensionKind::Ordinate: return coordinates(s, d).x;
        default: return 0.0;
    }
}

Vec2 coordinates(const Sketch& s, const DrawnDimension& d)
{
    if (d.kind != DrawnDimensionKind::Ordinate || d.refs.size() != 2) {
        return {};
    }
    return sub(pos(s, d.refs[0]), pos(s, d.refs[1]));
}

std::string number(const Sketch& s, const DrawnDimension& d, double v)
{
    const int places = d.decimals < 0 ? s.dimensionDecimals() : d.decimals;
    char buffer[64];
    std::snprintf(buffer, sizeof buffer, "%.*f", places, v);
    std::string out = buffer;
    // (No "-0.00": a value that rounds to nothing has no sign.)
    if (out.front() == '-' && out.find_first_not_of("-0.") == std::string::npos) {
        out.erase(0, 1);
    }
    return out;
}

std::string number(const Sketch& s, const DrawnDimension& d)
{
    return number(s, d, value(s, d));
}

std::string label(const Sketch& s, const DrawnDimension& d)
{
    std::string out = d.text;
    auto replaceAll = [&](const std::string& token, const std::string& with) {
        for (size_t at = out.find(token); at != std::string::npos; at = out.find(token, at + with.size())) {
            out.replace(at, token.size(), with);
        }
    };
    if (d.kind == DrawnDimensionKind::Ordinate) {
        const Vec2 c = coordinates(s, d);
        const std::string x = number(s, d, c.x), y = number(s, d, c.y);
        replaceAll("<x>", x);
        replaceAll("<y>", y);
        replaceAll("<>", "X" + x + "  Y" + y);
        return out;
    }
    replaceAll("<>", d.kind == DrawnDimensionKind::Leader ? std::string() : number(s, d));
    return out;
}

std::string defaultText(DrawnDimensionKind kind)
{
    switch (kind) {
        case DrawnDimensionKind::Radius: return "R<>";
        case DrawnDimensionKind::Diameter: return "\xC3\x98<>";        // Ø
        case DrawnDimensionKind::Angle: return "<>\xC2\xB0";           // °
        case DrawnDimensionKind::ArcLength: return "\xE2\x8C\x92<>";   // ⌒
        case DrawnDimensionKind::Leader: return "Note";
        default: return "<>";
    }
}

Graphics graphics(const Sketch& s, const DrawnDimension& d)
{
    Graphics g;
    for (Id r : d.refs) {
        if (!s.hasEntity(r)) {
            return g;
        }
    }
    Builder b{g, Sizes(d.textHeight)};
    const std::string text = label(s, d);
    switch (d.kind) {
        case DrawnDimensionKind::Aligned: linear(b, s, d, ConstraintType::Distance, text); break;
        case DrawnDimensionKind::Horizontal: linear(b, s, d, ConstraintType::HorizontalDistance, text); break;
        case DrawnDimensionKind::Vertical: linear(b, s, d, ConstraintType::VerticalDistance, text); break;
        case DrawnDimensionKind::Radius: round(b, s, d, ConstraintType::Radius, text); break;
        case DrawnDimensionKind::Diameter: round(b, s, d, ConstraintType::Diameter, text); break;
        case DrawnDimensionKind::Angle: angle(b, s, d, text); break;
        case DrawnDimensionKind::ArcLength: arcLength(b, s, d, text); break;
        case DrawnDimensionKind::Ordinate: leader(b, s, d, text); break;
        case DrawnDimensionKind::Leader: leader(b, s, d, text); break;
    }
    g.valid = !g.lines.empty() || !g.arcs.empty();
    return g;
}

Vec2 labelOffsetFor(const Sketch& s, const DrawnDimension& d, Vec2 at)
{
    if (const auto type = constraintKind(d.kind)) {
        return core::labelOffsetFor(s, asConstraint(d, *type), at);
    }
    Vec2 off = sub(at, anchorOf(s, d));
    if (off.x == 0.0 && off.y == 0.0) {
        off.x = 1e-9;  // (zero is "the default place")
    }
    return off;
}

}  // namespace core::annotation
