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
    if (sketch.isAxis(id)) {
        return id == sketch.xAxis() ? std::abs(p.y) : std::abs(p.x);  // infinite lines
    }
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

Vec2 closestPoint(const Sketch& sketch, Id id, Vec2 p)
{
    if (sketch.isAxis(id)) {
        return id == sketch.xAxis() ? Vec2{p.x, 0.0} : Vec2{0.0, p.y};  // infinite lines
    }
    const Entity& e = sketch.entity(id);
    switch (e.kind()) {
        case EntityKind::Point: return pos(sketch, id);
        case EntityKind::Line: {
            const Line& l = std::get<Line>(e.geom);
            const Vec2 a = pos(sketch, l.p1), b = pos(sketch, l.p2);
            const double dx = b.x - a.x, dy = b.y - a.y;
            const double l2 = dx * dx + dy * dy;
            const double t = l2 == 0.0 ? 0.0 : std::clamp(((p.x - a.x) * dx + (p.y - a.y) * dy) / l2, 0.0, 1.0);
            return {a.x + t * dx, a.y + t * dy};
        }
        case EntityKind::Circle: {
            const Circle& c = std::get<Circle>(e.geom);
            const Vec2 centre = pos(sketch, c.center);
            const double d = len(centre, p);
            if (d == 0.0) {
                return {centre.x + c.radius, centre.y};
            }
            return {centre.x + (p.x - centre.x) * c.radius / d, centre.y + (p.y - centre.y) * c.radius / d};
        }
        case EntityKind::Arc: {
            const ArcShape a = arcShape(sketch, id);
            const double angle = std::atan2(p.y - a.center.y, p.x - a.center.x);
            if (angleFrom(a.startAngle, angle) <= a.sweep) {
                return {a.center.x + a.radius * std::cos(angle), a.center.y + a.radius * std::sin(angle)};
            }
            const Arc& arc = std::get<Arc>(e.geom);
            const Vec2 s = pos(sketch, arc.start), en = pos(sketch, arc.end);
            return len(s, p) <= len(en, p) ? s : en;
        }
    }
    return p;
}

Id pick(const Sketch& sketch, Vec2 p, const PickOptions& options)
{
    Id bestPoint = kNoId, bestCurve = kNoId;
    double pointDist = options.tolerance, curveDist = options.tolerance;
    // Curves lying on top of each other (a fillet's solid piece over its now-construction
    // original) are equally near: then the solid one is meant.
    const double same = 1e-9 + 1e-6 * options.tolerance;
    for (const auto& [id, e] : sketch.entities()) {
        if (options.exclude.contains(id) || sketch.isHiddenReference(id) || sketch.isAxis(id)) {
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
        else if (!isPoint && d <= curveDist + same) {
            const bool tie = bestCurve != kNoId && std::abs(d - curveDist) <= same;
            if (tie && e.construction && !sketch.entity(bestCurve).construction) {
                continue;  // keep the solid one
            }
            if (!tie && d > curveDist) {
                continue;  // (only within `same` beyond the best so far: not nearer)
            }
            curveDist = std::min(d, curveDist);
            bestCurve = id;
        }
    }
    if (bestPoint != kNoId) {
        return bestPoint;
    }
    if (bestCurve != kNoId || !options.curves) {
        return bestCurve;
    }
    // The axes are infinite and come last, so geometry drawn along them stays pickable.
    Id bestAxis = kNoId;
    double axisDist = options.tolerance;
    for (Id axis : {sketch.xAxis(), sketch.yAxis()}) {
        if (axis == kNoId || options.exclude.contains(axis)) {
            continue;
        }
        const double d = axis == sketch.xAxis() ? std::abs(p.y) : std::abs(p.x);
        if (d <= axisDist) {
            axisDist = d;
            bestAxis = axis;
        }
    }
    return bestAxis;
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
        if (sketch.isReference(id)) {
            continue;  // origin and axes aren't box-selected (they'd only get in the way)
        }
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
