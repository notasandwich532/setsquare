#include "Geometry.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <numbers>

namespace core
{

namespace
{
constexpr double kTwoPi = 2.0 * std::numbers::pi;

Vec2 pos(const Sketch& s, Id point)
{
    const Point& p = s.point(point);
    return {p.x, p.y};
}

double len(Vec2 a, Vec2 b)
{
    return std::hypot(b.x - a.x, b.y - a.y);
}

double segmentDistance(Vec2 a, Vec2 b, Vec2 p)
{
    const double dx = b.x - a.x, dy = b.y - a.y;
    const double l2 = dx * dx + dy * dy;
    if (l2 == 0.0) {
        return len(a, p);
    }
    const double t = std::clamp(((p.x - a.x) * dx + (p.y - a.y) * dy) / l2, 0.0, 1.0);
    return len({a.x + t * dx, a.y + t * dy}, p);
}

// Angle of p measured from start, normalised to [0, 2*pi).
double angleFrom(double start, double angle)
{
    double a = std::fmod(angle - start, kTwoPi);
    return a < 0 ? a + kTwoPi : a;
}

struct Box
{
    double minX, minY, maxX, maxY;
    bool contains(Vec2 p) const { return p.x >= minX && p.x <= maxX && p.y >= minY && p.y <= maxY; }
};

// Liang-Barsky: does segment a-b touch the box?
bool segmentTouchesBox(Vec2 a, Vec2 b, const Box& box)
{
    double t0 = 0.0, t1 = 1.0;
    const double dx = b.x - a.x, dy = b.y - a.y;
    const double p[4] = {-dx, dx, -dy, dy};
    const double q[4] = {a.x - box.minX, box.maxX - a.x, a.y - box.minY, box.maxY - a.y};
    for (int i = 0; i < 4; ++i) {
        if (p[i] == 0.0) {
            if (q[i] < 0.0) {
                return false;
            }
            continue;
        }
        const double t = q[i] / p[i];
        if (p[i] < 0.0) {
            t0 = std::max(t0, t);
        }
        else {
            t1 = std::min(t1, t);
        }
        if (t0 > t1) {
            return false;
        }
    }
    return true;
}
}  // namespace

ArcShape arcShape(const Sketch& sketch, Id id)
{
    const Arc& a = sketch.arc(id);
    const Vec2 c = pos(sketch, a.center);
    const Vec2 s = pos(sketch, a.start);
    const Vec2 e = pos(sketch, a.end);
    ArcShape shape;
    shape.center = c;
    shape.radius = len(c, s);
    shape.startAngle = std::atan2(s.y - c.y, s.x - c.x);
    shape.sweep = angleFrom(shape.startAngle, std::atan2(e.y - c.y, e.x - c.x));
    if (shape.sweep == 0.0) {
        shape.sweep = kTwoPi;
    }
    return shape;
}

bool circumcircle(Vec2 a, Vec2 b, Vec2 c, Vec2& center, double& radius)
{
    const double d = 2.0 * (a.x * (b.y - c.y) + b.x * (c.y - a.y) + c.x * (a.y - b.y));
    const double scale = std::max({len(a, b), len(b, c), len(a, c)});
    if (std::abs(d) < 1e-9 * scale * scale) {
        return false;
    }
    const double a2 = a.x * a.x + a.y * a.y;
    const double b2 = b.x * b.x + b.y * b.y;
    const double c2 = c.x * c.x + c.y * c.y;
    center.x = (a2 * (b.y - c.y) + b2 * (c.y - a.y) + c2 * (a.y - b.y)) / d;
    center.y = (a2 * (c.x - b.x) + b2 * (a.x - c.x) + c2 * (b.x - a.x)) / d;
    radius = len(center, a);
    return true;
}

double distanceTo(const Sketch& sketch, Id id, Vec2 p)
{
    const Entity& e = sketch.entity(id);
    switch (e.kind()) {
        case EntityKind::Point: return len(pos(sketch, id), p);
        case EntityKind::Line: {
            const Line& l = std::get<Line>(e.geom);
            return segmentDistance(pos(sketch, l.p1), pos(sketch, l.p2), p);
        }
        case EntityKind::Circle: {
            const Circle& c = std::get<Circle>(e.geom);
            return std::abs(len(pos(sketch, c.center), p) - c.radius);
        }
        case EntityKind::Arc: {
            const ArcShape a = arcShape(sketch, id);
            const double angle = std::atan2(p.y - a.center.y, p.x - a.center.x);
            if (angleFrom(a.startAngle, angle) <= a.sweep) {
                return std::abs(len(a.center, p) - a.radius);
            }
            const Arc& arc = std::get<Arc>(e.geom);
            return std::min(len(pos(sketch, arc.start), p), len(pos(sketch, arc.end), p));
        }
    }
    return std::numeric_limits<double>::infinity();
}

Id pick(const Sketch& sketch, Vec2 p, const PickOptions& options)
{
    Id bestPoint = kNoId, bestCurve = kNoId;
    double pointDist = options.tolerance, curveDist = options.tolerance;
    for (const auto& [id, e] : sketch.entities()) {
        if (options.exclude.contains(id)) {
            continue;
        }
        const bool isPoint = e.kind() == EntityKind::Point;
        if ((isPoint && !options.points) || (!isPoint && !options.curves)) {
            continue;
        }
        const double d = distanceTo(sketch, id, p);
        if (isPoint && d <= pointDist) {
            pointDist = d;
            bestPoint = id;
        }
        else if (!isPoint && d <= curveDist) {
            curveDist = d;
            bestCurve = id;
        }
    }
    return bestPoint != kNoId ? bestPoint : bestCurve;
}

std::vector<Vec2> sample(const Sketch& sketch, Id id, int segments)
{
    const Entity& e = sketch.entity(id);
    std::vector<Vec2> out;
    switch (e.kind()) {
        case EntityKind::Point: out.push_back(pos(sketch, id)); break;
        case EntityKind::Line: {
            const Line& l = std::get<Line>(e.geom);
            out = {pos(sketch, l.p1), pos(sketch, l.p2)};
            break;
        }
        case EntityKind::Circle: {
            const Circle& c = std::get<Circle>(e.geom);
            const Vec2 centre = pos(sketch, c.center);
            for (int i = 0; i <= segments; ++i) {
                const double t = kTwoPi * i / segments;
                out.push_back({centre.x + c.radius * std::cos(t), centre.y + c.radius * std::sin(t)});
            }
            break;
        }
        case EntityKind::Arc: {
            const ArcShape a = arcShape(sketch, id);
            for (int i = 0; i <= segments; ++i) {
                const double t = a.startAngle + a.sweep * i / segments;
                out.push_back({a.center.x + a.radius * std::cos(t), a.center.y + a.radius * std::sin(t)});
            }
            break;
        }
    }
    return out;
}

std::vector<Id> boxSelect(const Sketch& sketch, Vec2 c1, Vec2 c2, bool crossing)
{
    const Box box{std::min(c1.x, c2.x), std::min(c1.y, c2.y), std::max(c1.x, c2.x), std::max(c1.y, c2.y)};
    std::vector<Id> out;
    for (const auto& [id, e] : sketch.entities()) {
        const std::vector<Vec2> pts = sample(sketch, id);
        bool hit = false;
        if (pts.size() == 1) {
            hit = box.contains(pts[0]);
        }
        else if (crossing) {
            for (size_t i = 0; i + 1 < pts.size() && !hit; ++i) {
                hit = segmentTouchesBox(pts[i], pts[i + 1], box);
            }
        }
        else {
            hit = std::ranges::all_of(pts, [&](Vec2 p) { return box.contains(p); });
        }
        if (hit) {
            out.push_back(id);
        }
    }
    return out;
}

}  // namespace core
