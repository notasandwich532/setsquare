#include "Gradient.h"

#include <algorithm>
#include <cmath>
#include <numeric>

namespace core::gradient
{

Vec2 Frame::fractions(Vec2 world) const
{
    const double det = u.x * v.y - u.y * v.x;
    if (std::abs(det) < 1e-300) {
        return {0, 0};
    }
    const double dx = world.x - origin.x, dy = world.y - origin.y;
    return {(dx * v.y - dy * v.x) / det, (u.x * dy - u.y * dx) / det};
}

Frame Frame::around(const std::vector<Vec2>& points, double pad, double minSize)
{
    if (points.empty()) {
        return {};
    }
    double x0 = 1e300, y0 = 1e300, x1 = -1e300, y1 = -1e300;
    for (const Vec2& p : points) {
        x0 = std::min(x0, p.x);
        y0 = std::min(y0, p.y);
        x1 = std::max(x1, p.x);
        y1 = std::max(y1, p.y);
    }
    x0 -= pad;
    y0 -= pad;
    x1 += pad;
    y1 += pad;
    auto widen = [minSize](double& a, double& b) {
        if (b - a < minSize) {
            const double mid = (a + b) / 2;
            a = mid - minSize / 2;
            b = mid + minSize / 2;
        }
    };
    widen(x0, x1);
    widen(y0, y1);
    return {{x0, y0}, {x1 - x0, 0}, {0, y1 - y0}};
}

Placed place(const Gradient& g, const Frame& frame)
{
    Placed p;
    p.kind = g.kind;
    p.spread = g.spread;
    p.stops = g.stops;
    p.start = frame.at(g.x1, g.y1);
    p.end = frame.at(g.x2, g.y2);
    p.center = p.start;
    const Vec2 d{p.end.x - p.start.x, p.end.y - p.start.y};
    p.radius = std::hypot(d.x, d.y);
    p.angle = std::atan2(d.y, d.x);
    p.aspect = std::max(g.aspect, 1e-3);
    // The focal point: from the centre by the fractions, then held inside the ellipse.
    const Vec2 f = frame.at(g.x1 + g.fx, g.y1 + g.fy);
    const double c = std::cos(p.angle), s = std::sin(p.angle);
    const Vec2 rel{f.x - p.center.x, f.y - p.center.y};
    Vec2 local{rel.x * c + rel.y * s, (-rel.x * s + rel.y * c) / p.aspect};
    const double r = std::hypot(local.x, local.y);
    const double limit = p.radius * 0.99;
    if (r > limit && r > 0.0) {
        local = {local.x * limit / r, local.y * limit / r};
    }
    p.focalLocal = local;
    const double ly = local.y * p.aspect;
    p.focal = {p.center.x + local.x * c - ly * s, p.center.y + local.x * s + ly * c};
    return p;
}

Frame fillFrame(const regions::Region& region)
{
    return Frame::around(region.outer);
}

Frame textFrame(const Sketch& sketch, const Text& t)
{
    double x0 = 1e300, y0 = 1e300, x1 = -1e300, y1 = -1e300;
    for (const auto& contour : t.shape) {
        for (const Point& p : contour) {
            x0 = std::min(x0, p.x);
            y0 = std::min(y0, p.y);
            x1 = std::max(x1, p.x);
            y1 = std::max(y1, p.y);
        }
    }
    if (x0 > x1 || !sketch.hasEntity(t.anchor)) {
        return {};
    }
    x1 = std::max(x1, x0 + 1e-6);
    y1 = std::max(y1, y0 + 1e-6);
    const Point& anchor = sketch.point(t.anchor);
    const Point o = textToWorld(anchor, t.angle, {x0, y0});
    const Point a = textToWorld(anchor, t.angle, {x1, y0});
    const Point b = textToWorld(anchor, t.angle, {x0, y1});
    return {{o.x, o.y}, {a.x - o.x, a.y - o.y}, {b.x - o.x, b.y - o.y}};
}

namespace
{
struct StrokedCurve
{
    Id id;
    const StrokeStyle* style;
    Id layer;
    std::vector<Vec2> points;
};

// The curves with a gradient stroke, and which group each is in (an index into the curves of
// the group's first member).
std::vector<StrokedCurve> strokedCurves(const Sketch& sketch, const std::map<Id, StrokeStyle>& strokes,
                                        std::vector<size_t>& group)
{
    std::vector<StrokedCurve> curves;
    for (const auto& [id, style] : strokes) {
        if (!style.gradient.active() || style.none() || !sketch.hasEntity(id) || sketch.entity(id).construction) {
            continue;
        }
        curves.push_back({id, &style, sketch.layerOf(id), sample(sketch, id, 96)});
    }
    // Join the curves that meet end to end with the same stroke.
    std::vector<size_t> parent(curves.size());
    std::iota(parent.begin(), parent.end(), size_t{0});
    auto root = [&](size_t i) {
        while (parent[i] != i) {
            i = parent[i] = parent[parent[i]];
        }
        return i;
    };
    auto near = [](Vec2 a, Vec2 b) {
        return std::hypot(a.x - b.x, a.y - b.y) <= 1e-6 * (1 + std::abs(a.x) + std::abs(a.y));
    };
    for (size_t i = 0; i < curves.size(); ++i) {
        for (size_t j = i + 1; j < curves.size(); ++j) {
            const StrokedCurve& a = curves[i];
            const StrokedCurve& b = curves[j];
            if (a.layer != b.layer || !(*a.style == *b.style) || a.points.empty() || b.points.empty()) {
                continue;
            }
            const Vec2 ae[2] = {a.points.front(), a.points.back()};
            const Vec2 be[2] = {b.points.front(), b.points.back()};
            bool touch = false;
            for (const Vec2& p : ae) {
                for (const Vec2& q : be) {
                    touch = touch || near(p, q);
                }
            }
            if (touch) {
                parent[root(i)] = root(j);
            }
        }
    }
    group.resize(curves.size());
    for (size_t i = 0; i < curves.size(); ++i) {
        group[i] = root(i);
    }
    return curves;
}
}  // namespace

std::map<Id, Frame> strokeFrames(const Sketch& sketch)
{
    return strokeFrames(sketch, sketch.strokes());
}

std::map<Id, Frame> strokeFrames(const Sketch& sketch, const std::map<Id, StrokeStyle>& strokes)
{
    std::vector<size_t> group;
    const std::vector<StrokedCurve> curves = strokedCurves(sketch, strokes, group);
    std::map<size_t, std::vector<Vec2>> points;
    for (size_t i = 0; i < curves.size(); ++i) {
        auto& pts = points[group[i]];
        pts.insert(pts.end(), curves[i].points.begin(), curves[i].points.end());
    }
    std::map<Id, Frame> out;
    for (size_t i = 0; i < curves.size(); ++i) {
        const double pad = std::max(curves[i].style->width, 0.0) / 2;
        out[curves[i].id] = Frame::around(points[group[i]], pad, std::max(2 * pad, 1e-3));
    }
    return out;
}

std::vector<std::vector<Id>> strokeGroups(const Sketch& sketch, const std::map<Id, StrokeStyle>& strokes)
{
    std::vector<size_t> group;
    const std::vector<StrokedCurve> curves = strokedCurves(sketch, strokes, group);
    std::map<size_t, std::vector<Id>> byRoot;
    for (size_t i = 0; i < curves.size(); ++i) {
        byRoot[group[i]].push_back(curves[i].id);
    }
    std::vector<std::vector<Id>> out;
    for (auto& [root, ids] : byRoot) {
        out.push_back(std::move(ids));
    }
    return out;
}

std::vector<Id> strokeGroup(const Sketch& sketch, Id curve)
{
    for (std::vector<Id>& g : strokeGroups(sketch, sketch.strokes())) {
        if (std::ranges::find(g, curve) != g.end()) {
            return std::move(g);
        }
    }
    return {curve};
}

}  // namespace core::gradient