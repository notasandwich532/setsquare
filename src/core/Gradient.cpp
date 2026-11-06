#include "Gradient.h"

#include "Curves.h"

#include <algorithm>
#include <cmath>
#include <numeric>

namespace core::gradient
{

bool canBePoint(const Sketch& s, Id id)
{
    return s.hasEntity(id) && s.entity(id).kind() == EntityKind::Point && !s.isHiddenReference(id);
}

bool canBeShape(const Sketch& s, Id id)
{
    if (!s.hasEntity(id)) {
        return false;
    }
    const EntityKind k = s.entity(id).kind();
    return k == EntityKind::Circle || k == EntityKind::Arc || k == EntityKind::Ellipse || k == EntityKind::EllipseArc;
}

std::optional<Placed> place(const Gradient& g, const Sketch& s)
{
    if (!g.active() || !g.complete()) {
        return std::nullopt;
    }
    Placed p;
    p.kind = g.kind;
    p.spread = g.spread;
    p.stops = g.stops;
    auto at = [&](Id id) {
        const Point& q = s.point(id);
        return Vec2{q.x, q.y};
    };
    if (g.kind == GradientKind::Linear) {
        if (!canBePoint(s, g.start) || !canBePoint(s, g.end)) {
            return std::nullopt;
        }
        p.start = at(g.start);
        p.end = at(g.end);
        return p;
    }
    if (!canBeShape(s, g.shape) || (g.focal != kNoId && !canBePoint(s, g.focal))) {
        return std::nullopt;
    }
    switch (s.entity(g.shape).kind()) {
        case EntityKind::Circle: {
            const Circle& c = s.circle(g.shape);
            p.center = at(c.center);
            p.radius = c.radius;
            break;
        }
        case EntityKind::Arc: {
            const ArcShape a = arcShape(s, g.shape);
            p.center = a.center;
            p.radius = a.radius;
            break;
        }
        default: {
            const curve::EllipseFrame f = curve::ellipseFrame(s, g.shape);
            p.center = f.c;
            p.radius = f.a;
            p.angle = std::atan2(f.u.y, f.u.x);
            p.aspect = f.a > 0.0 ? f.b / f.a : 1.0;
            break;
        }
    }
    p.aspect = std::max(p.aspect, 1e-3);
    p.end = {p.center.x + p.radius * std::cos(p.angle), p.center.y + p.radius * std::sin(p.angle)};
    // The focal point (the centre unless given), held inside the shape.
    const Vec2 f = g.focal != kNoId ? at(g.focal) : p.center;
    const double c = std::cos(p.angle), sn = std::sin(p.angle);
    const Vec2 rel{f.x - p.center.x, f.y - p.center.y};
    Vec2 local{rel.x * c + rel.y * sn, (-rel.x * sn + rel.y * c) / p.aspect};
    const double r = std::hypot(local.x, local.y);
    const double limit = p.radius * 0.99;
    if (r > limit && r > 0.0) {
        local = {local.x * limit / r, local.y * limit / r};
    }
    p.focalLocal = local;
    const double ly = local.y * p.aspect;
    p.focal = {p.center.x + local.x * c - ly * sn, p.center.y + local.x * sn + ly * c};
    return p;
}

void check(const Sketch& s, const Gradient& g, Id layer)
{
    if (!g.active()) {
        return;
    }
    if (g.kind == GradientKind::Linear) {
        if (!canBePoint(s, g.start) || !canBePoint(s, g.end)) {
            throw SketchError("a linear gradient runs between two points of the sketch (start and end)");
        }
        if (g.start == g.end) {
            throw SketchError("a linear gradient's start and end must be different points");
        }
    }
    else {
        if (!canBeShape(s, g.shape)) {
            throw SketchError("a radial gradient is placed by a circle, arc, ellipse or elliptical arc of the sketch");
        }
        if (g.focal != kNoId && !canBePoint(s, g.focal)) {
            throw SketchError("a radial gradient's focal point must be a point of the sketch");
        }
    }
    for (Id id : g.refs()) {
        const Id l = s.layerOf(id);
        if (layer != kNoId && l != kNoId && l != layer) {
            throw SketchError("a gradient's geometry must be in the same layer as what it paints "
                              "(project it into this layer first)");
        }
    }
}

void remap(Gradient& g, const std::map<Id, Id>& map, bool sameSketch)
{
    for (Id* id : {&g.start, &g.end, &g.shape, &g.focal}) {
        if (*id == kNoId) {
            continue;
        }
        if (const auto it = map.find(*id); it != map.end()) {
            *id = it->second;
        }
        else if (!sameSketch) {
            *id = kNoId;
        }
    }
}

std::vector<std::vector<Id>> strokeGroups(const Sketch& sketch)
{
    struct Curve
    {
        Id id;
        const StrokeStyle* style;
        Id layer;
        Vec2 ends[2];
    };
    std::vector<Curve> curves;
    for (const auto& [id, style] : sketch.strokes()) {
        if (!style.gradient.active() || !sketch.hasEntity(id) || sketch.entity(id).construction) {
            continue;
        }
        const std::vector<Vec2> pts = sample(sketch, id, 8);
        if (pts.empty()) {
            continue;
        }
        curves.push_back({id, &style, sketch.layerOf(id), {pts.front(), pts.back()}});
    }
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
            const Curve& a = curves[i];
            const Curve& b = curves[j];
            if (a.layer != b.layer || !(*a.style == *b.style)) {
                continue;
            }
            bool touch = false;
            for (const Vec2& p : a.ends) {
                for (const Vec2& q : b.ends) {
                    touch = touch || near(p, q);
                }
            }
            if (touch) {
                parent[root(i)] = root(j);
            }
        }
    }
    std::map<size_t, std::vector<Id>> byRoot;
    for (size_t i = 0; i < curves.size(); ++i) {
        byRoot[root(i)].push_back(curves[i].id);
    }
    std::vector<std::vector<Id>> out;
    for (auto& [r, ids] : byRoot) {
        out.push_back(std::move(ids));
    }
    return out;
}

std::vector<Id> strokeGroup(const Sketch& sketch, Id curve)
{
    for (std::vector<Id>& g : strokeGroups(sketch)) {
        if (std::ranges::find(g, curve) != g.end()) {
            return std::move(g);
        }
    }
    return {curve};
}

}  // namespace core::gradient
