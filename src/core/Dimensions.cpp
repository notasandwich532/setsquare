#include "Dimensions.h"

#include <algorithm>
#include <cmath>
#include <numbers>

namespace core
{

namespace
{
constexpr double kPi = std::numbers::pi;

Vec2 pos(const Sketch& s, Id point)
{
    const Point& p = s.point(point);
    return {p.x, p.y};
}

Vec2 sub(Vec2 a, Vec2 b) { return {a.x - b.x, a.y - b.y}; }
Vec2 add(Vec2 a, Vec2 b) { return {a.x + b.x, a.y + b.y}; }
Vec2 mul(Vec2 a, double k) { return {a.x * k, a.y * k}; }
double dot(Vec2 a, Vec2 b) { return a.x * b.x + a.y * b.y; }
double norm(Vec2 a) { return std::hypot(a.x, a.y); }
Vec2 mid(Vec2 a, Vec2 b) { return {(a.x + b.x) / 2, (a.y + b.y) / 2}; }

Vec2 unit(Vec2 a, Vec2 fallback = {1, 0})
{
    const double n = norm(a);
    return n > 1e-12 ? Vec2{a.x / n, a.y / n} : fallback;
}

double wrapPi(double a)
{
    while (a > kPi) {
        a -= 2 * kPi;
    }
    while (a <= -kPi) {
        a += 2 * kPi;
    }
    return a;
}

void lineEnds(const Sketch& s, Id line, Vec2& a, Vec2& b)
{
    const Line& l = s.line(line);
    a = pos(s, l.p1);
    b = pos(s, l.p2);
}

// Endpoints of a two-point measurement: (point, point) or (line).
void measuredPoints(const Sketch& s, const std::vector<Id>& refs, Vec2& a, Vec2& b)
{
    if (refs.size() == 1) {
        lineEnds(s, refs[0], a, b);
    }
    else {
        a = pos(s, refs[0]);
        b = pos(s, refs[1]);
    }
}

// Foot of the perpendicular from p onto the infinite line through a-b.
Vec2 foot(Vec2 p, Vec2 a, Vec2 b)
{
    const Vec2 d = sub(b, a);
    const double l2 = dot(d, d);
    if (l2 == 0.0) {
        return a;
    }
    return add(a, mul(d, dot(sub(p, a), d) / l2));
}

double roundRadius(const Sketch& s, Id id)
{
    const Entity& e = s.entity(id);
    if (e.kind() == EntityKind::Circle) {
        return std::get<Circle>(e.geom).radius;
    }
    return s.arcRadius(id);
}

Vec2 roundCenter(const Sketch& s, Id id)
{
    const Entity& e = s.entity(id);
    if (e.kind() == EntityKind::Circle) {
        return pos(s, std::get<Circle>(e.geom).center);
    }
    return pos(s, std::get<Arc>(e.geom).center);
}

double lineAngle(const Sketch& s, Id line)
{
    Vec2 a, b;
    lineEnds(s, line, a, b);
    return std::atan2(b.y - a.y, b.x - a.x);
}
}  // namespace

double measure(const Sketch& s, ConstraintType type, const std::vector<Id>& refs)
{
    switch (type) {
        case ConstraintType::Distance: {
            if (refs.size() == 2 && s.entity(refs[1]).kind() == EntityKind::Line) {
                Vec2 a, b;
                lineEnds(s, refs[1], a, b);
                const Vec2 p = pos(s, refs[0]);
                return norm(sub(p, foot(p, a, b)));
            }
            Vec2 a, b;
            measuredPoints(s, refs, a, b);
            return norm(sub(b, a));
        }
        case ConstraintType::HorizontalDistance:
        case ConstraintType::VerticalDistance: {
            Vec2 a, b;
            measuredPoints(s, refs, a, b);
            return type == ConstraintType::HorizontalDistance ? std::abs(b.x - a.x) : std::abs(b.y - a.y);
        }
        case ConstraintType::Radius: return roundRadius(s, refs[0]);
        case ConstraintType::Diameter: return 2.0 * roundRadius(s, refs[0]);
        case ConstraintType::Angle:
            return std::abs(wrapPi(lineAngle(s, refs[1]) - lineAngle(s, refs[0]))) * 180.0 / kPi;
        default: return 0.0;
    }
}

DimensionLayout layoutDimension(const Sketch& s, const Constraint& c)
{
    DimensionLayout L;
    const Vec2 offset{c.labelX, c.labelY};
    const bool defaultLabel = c.labelX == 0.0 && c.labelY == 0.0;

    switch (c.type) {
        case ConstraintType::Distance:
        case ConstraintType::HorizontalDistance:
        case ConstraintType::VerticalDistance: {
            L.style = DimensionLayout::Style::Linear;
            if (c.type == ConstraintType::Distance && c.refs.size() == 2
                && s.entity(c.refs[1]).kind() == EntityKind::Line) {
                Vec2 la, lb;
                lineEnds(s, c.refs[1], la, lb);
                L.a = pos(s, c.refs[0]);
                L.b = foot(L.a, la, lb);
                const Vec2 lineDir = unit(sub(lb, la));
                L.dir = unit(sub(L.b, L.a), Vec2{-lineDir.y, lineDir.x});
            }
            else {
                measuredPoints(s, c.refs, L.a, L.b);
                if (c.type == ConstraintType::HorizontalDistance) {
                    L.dir = {1, 0};
                }
                else if (c.type == ConstraintType::VerticalDistance) {
                    L.dir = {0, 1};
                }
                else {
                    L.dir = unit(sub(L.b, L.a));
                }
            }
            L.anchor = mid(L.a, L.b);
            if (defaultLabel) {
                const Vec2 n{-L.dir.y, L.dir.x};
                const double away = std::max(norm(sub(L.b, L.a)) * 0.2, 5.0);
                L.label = add(L.anchor, mul(n, away));
            }
            else {
                L.label = add(L.anchor, offset);
            }
            L.valid = true;
            break;
        }

        case ConstraintType::Radius:
        case ConstraintType::Diameter: {
            L.style = c.type == ConstraintType::Radius ? DimensionLayout::Style::Radius : DimensionLayout::Style::Diameter;
            L.center = roundCenter(s, c.refs[0]);
            L.radius = roundRadius(s, c.refs[0]);
            L.anchor = L.center;
            if (defaultLabel) {
                const double k = L.radius * 1.3 / std::sqrt(2.0);
                L.label = add(L.center, {k, k});
            }
            else {
                L.label = add(L.anchor, offset);
            }
            L.valid = true;
            break;
        }

        case ConstraintType::Angle: {
            L.style = DimensionLayout::Style::Angle;
            Vec2 a1, b1, a2, b2;
            lineEnds(s, c.refs[0], a1, b1);
            lineEnds(s, c.refs[1], a2, b2);
            const Vec2 d1 = sub(b1, a1), d2 = sub(b2, a2);
            const double cross = d1.x * d2.y - d1.y * d2.x;
            if (std::abs(cross) < 1e-12 * norm(d1) * norm(d2)) {
                return L;  // parallel: no vertex to draw around
            }
            // Intersection of the two infinite lines.
            const double t = ((a2.x - a1.x) * d2.y - (a2.y - a1.y) * d2.x) / cross;
            L.center = add(a1, mul(d1, t));
            L.anchor = L.center;
            L.startAngle = std::atan2(d1.y, d1.x);
            L.sweep = wrapPi(std::atan2(d2.y, d2.x) - L.startAngle);
            if (defaultLabel) {
                const double r = std::max(std::min(norm(d1), norm(d2)) * 0.4, 5.0);
                const double bisector = L.startAngle + L.sweep / 2;
                L.label = add(L.center, {r * std::cos(bisector), r * std::sin(bisector)});
            }
            else {
                L.label = add(L.anchor, offset);
            }
            L.radius = norm(sub(L.label, L.center));
            L.valid = true;
            break;
        }

        default: break;
    }
    return L;
}

Vec2 labelOffsetFor(const Sketch& s, const Constraint& c, Vec2 label)
{
    Constraint probe = c;
    probe.labelX = 1.0;  // any non-default offset, so the anchor is all we read back
    const DimensionLayout L = layoutDimension(s, probe);
    Vec2 off = sub(label, L.anchor);
    if (off.x == 0.0 && off.y == 0.0) {
        off.x = 1e-9;  // keep it distinguishable from "default"
    }
    return off;
}

}  // namespace core
