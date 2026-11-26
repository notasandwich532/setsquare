#include "Measure.h"

#include "Curves.h"

#include <algorithm>
#include <cmath>
#include <utility>

namespace core::measuring
{

namespace
{
// Arcs followed in steps this small: a circle's area comes out within about 2e-6 of itself.
constexpr double kFineArcStep = 3.141592653589793 / 1024;

// Exact for lines, circles and arcs; ellipses and splines integrated (Gauss-Legendre, 5 points on
// each of many small steps: to ~1e-12 of the length).
double curveLength(const curve::Path& path)
{
    using Kind = curve::Path::Kind;
    switch (path.kind()) {
        case Kind::Line: return std::hypot(path.b.x - path.a.x, path.b.y - path.a.y);
        case Kind::Circle: return 2.0 * 3.141592653589793 * path.r;
        case Kind::Arc: return path.r * std::abs(path.sweep);
        default: break;
    }
    static constexpr double kNodes[] = {0.0, -0.5384693101056831, 0.5384693101056831, -0.9061798459386640,
                                        0.9061798459386640};
    static constexpr double kWeights[] = {0.5688888888888889, 0.4786286704993665, 0.4786286704993665,
                                          0.2369268850561891, 0.2369268850561891};
    const double end = path.end();
    const int steps = std::max(64, static_cast<int>(std::ceil(std::abs(end) * 64)));
    const double h = end / steps;
    double total = 0.0;
    for (int i = 0; i < steps; ++i) {
        const double mid = (i + 0.5) * h;
        for (int k = 0; k < 5; ++k) {
            const Vec2 d = path.derivative(mid + kNodes[k] * h / 2);
            total += kWeights[k] * std::hypot(d.x, d.y);
        }
    }
    return std::abs(total * h / 2);
}

double polygonArea(const std::vector<Vec2>& polygon)
{
    double twice = 0.0;
    for (size_t i = 0, n = polygon.size(); i < n; ++i) {
        const Vec2& a = polygon[i];
        const Vec2& b = polygon[(i + 1) % n];
        twice += a.x * b.y - b.x * a.y;
    }
    return std::abs(twice) / 2.0;
}
}  // namespace

double length(const Sketch& sketch, const std::vector<Id>& curves)
{
    double total = 0.0;
    for (Id id : curves) {
        if (sketch.hasEntity(id) && sketch.entity(id).kind() != EntityKind::Point) {
            total += curveLength(curve::pathOf(sketch, id));
        }
    }
    return total;
}

Separation between(Vec2 a, Vec2 b)
{
    const double dx = b.x - a.x;
    const double dy = b.y - a.y;
    return {std::hypot(dx, dy), dx, dy};
}

Separation between(const Sketch& sketch, Id a, Id b)
{
    const Point& p = sketch.point(a);
    const Point& q = sketch.point(b);
    return between(Vec2{p.x, p.y}, Vec2{q.x, q.y});
}

Angle angle(const Sketch& sketch, Id line1, Id line2)
{
    auto ends = [&](Id id) {
        const Line& l = sketch.line(id);
        const Point& p = sketch.point(l.p1);
        const Point& q = sketch.point(l.p2);
        return std::pair{Vec2{p.x, p.y}, Vec2{q.x, q.y}};
    };
    const auto [a1, a2] = ends(line1);
    const auto [b1, b2] = ends(line2);
    const Vec2 d1{a2.x - a1.x, a2.y - a1.y}, d2{b2.x - b1.x, b2.y - b1.y};
    const double l1 = std::hypot(d1.x, d1.y), l2 = std::hypot(d2.x, d2.y);
    Angle out;
    const double cross = d1.x * d2.y - d1.y * d2.x;
    if (l1 < 1e-12 || l2 < 1e-12 || std::abs(cross) <= 1e-9 * l1 * l2) {
        out.parallel = true;
        out.degrees = 0.0;
        out.other = 180.0;
        return out;
    }
    // Where they meet: a1 + t d1 on line 2.
    const double t = ((b1.x - a1.x) * d2.y - (b1.y - a1.y) * d2.x) / cross;
    out.vertex = {a1.x + t * d1.x, a1.y + t * d1.y};
    // Out along each line: towards its end farther from the vertex.
    auto outward = [&](Vec2 p, Vec2 q) {
        const Vec2 far = std::hypot(p.x - out.vertex.x, p.y - out.vertex.y)
                >= std::hypot(q.x - out.vertex.x, q.y - out.vertex.y) ? p : q;
        const Vec2 d{far.x - out.vertex.x, far.y - out.vertex.y};
        const double n = std::hypot(d.x, d.y);
        return Vec2{d.x / n, d.y / n};
    };
    out.dir1 = outward(a1, a2);
    out.dir2 = outward(b1, b2);
    const double c = std::clamp(out.dir1.x * out.dir2.x + out.dir1.y * out.dir2.y, -1.0, 1.0);
    out.degrees = std::acos(c) * 180.0 / 3.141592653589793;
    out.other = 180.0 - out.degrees;
    return out;
}

std::optional<regions::Region> areaAt(const Sketch& sketch, Id layer, Vec2 p)
{
    return regions::Arrangement(sketch, layer, kFineArcStep).regionAt(p);
}

double area(const regions::Region& region)
{
    double a = polygonArea(region.outer);
    for (const std::vector<Vec2>& hole : region.holes) {
        a -= polygonArea(hole);
    }
    return a;
}

}  // namespace core::measuring
