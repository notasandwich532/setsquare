#include "Measure.h"

#include "Curves.h"

#include <algorithm>
#include <cmath>

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
