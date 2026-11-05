#include "Regions.h"

#include "Modify.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <map>
#include <numbers>
#include <numeric>

namespace core::regions
{

namespace
{
constexpr double kTwoPi = 2.0 * std::numbers::pi;

Vec2 sub(Vec2 a, Vec2 b) { return {a.x - b.x, a.y - b.y}; }
double norm(Vec2 a) { return std::hypot(a.x, a.y); }

// A solid curve, with a parameter along it: t in [0, 1] on a line; the angle from the start
// for an arc (0..sweep) and from +x for a circle (0..2pi).
struct Curve
{
    enum class Kind
    {
        Line,
        Arc,
        Circle,
    } kind = Kind::Line;
    Id id = kNoId;
    Vec2 a, b;  // line
    Vec2 c;     // arc / circle
    double r = 0.0, start = 0.0, sweep = kTwoPi;

    Vec2 at(double t) const
    {
        if (kind == Kind::Line) {
            return {a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t};
        }
        return {c.x + r * std::cos(start + t), c.y + r * std::sin(start + t)};
    }
    double param(Vec2 p) const
    {
        if (kind == Kind::Line) {
            const Vec2 d = sub(b, a);
            const double l2 = d.x * d.x + d.y * d.y;
            return l2 > 0 ? std::clamp(((p.x - a.x) * d.x + (p.y - a.y) * d.y) / l2, 0.0, 1.0) : 0.0;
        }
        double t = std::fmod(std::atan2(p.y - c.y, p.x - c.x) - start, kTwoPi);
        if (t < 0) {
            t += kTwoPi;
        }
        if (kind == Kind::Arc && t > sweep) {
            // Just past an end through rounding: the nearer end.
            t = (t - sweep < kTwoPi - t) ? sweep : 0.0;
        }
        return t;
    }
    double spanLength(double t0, double t1) const
    {
        return kind == Kind::Line ? norm(sub(b, a)) * (t1 - t0) : r * (t1 - t0);
    }
};

bool pointInPolygon(const std::vector<Vec2>& poly, Vec2 p)
{
    bool inside = false;
    for (size_t i = 0, j = poly.size() - 1; i < poly.size(); j = i++) {
        const Vec2 a = poly[i], b = poly[j];
        if ((a.y > p.y) != (b.y > p.y) && p.x < (b.x - a.x) * (p.y - a.y) / (b.y - a.y) + a.x) {
            inside = !inside;
        }
    }
    return inside;
}

double signedArea(const std::vector<Vec2>& poly)
{
    double sum = 0.0;
    for (size_t i = 0, j = poly.size() - 1; i < poly.size(); j = i++) {
        sum += (poly[j].x * poly[i].y - poly[i].x * poly[j].y);
    }
    return sum / 2.0;
}

double segmentDistance(Vec2 p, Vec2 a, Vec2 b)
{
    const Vec2 d = sub(b, a);
    const double l2 = d.x * d.x + d.y * d.y;
    const double t = l2 > 0 ? std::clamp(((p.x - a.x) * d.x + (p.y - a.y) * d.y) / l2, 0.0, 1.0) : 0.0;
    return norm(sub(p, {a.x + d.x * t, a.y + d.y * t}));
}

struct UnionFind
{
    std::vector<int> parent;
    int find(int i)
    {
        while (parent[static_cast<size_t>(i)] != i) {
            i = parent[static_cast<size_t>(i)] = parent[static_cast<size_t>(parent[static_cast<size_t>(i)])];
        }
        return i;
    }
    void unite(int a, int b) { parent[static_cast<size_t>(find(a))] = find(b); }
};
}  // namespace

// ---------------------------------------------------------------------------------------------

bool Region::contains(Vec2 p) const
{
    if (!pointInPolygon(outer, p)) {
        return false;
    }
    return std::ranges::none_of(holes, [&](const auto& hole) { return pointInPolygon(hole, p); });
}

Vec2 Region::interiorPoint() const
{
    double x0 = 1e300, y0 = 1e300, x1 = -1e300, y1 = -1e300;
    for (const Vec2& p : outer) {
        x0 = std::min(x0, p.x);
        y0 = std::min(y0, p.y);
        x1 = std::max(x1, p.x);
        y1 = std::max(y1, p.y);
    }
    auto clearance = [&](Vec2 p) {
        double best = 1e300;
        auto edges = [&](const std::vector<Vec2>& poly) {
            for (size_t i = 0, j = poly.size() - 1; i < poly.size(); j = i++) {
                best = std::min(best, segmentDistance(p, poly[j], poly[i]));
            }
        };
        edges(outer);
        for (const auto& hole : holes) {
            edges(hole);
        }
        return best;
    };
    // The point of a grid over the area that's furthest from any edge.
    Vec2 found = outer.front();
    double most = -1.0;
    constexpr int kSteps = 24;
    for (int i = 1; i < kSteps; ++i) {
        for (int j = 1; j < kSteps; ++j) {
            const Vec2 p{x0 + (x1 - x0) * i / kSteps, y0 + (y1 - y0) * j / kSteps};
            if (contains(p)) {
                if (const double c = clearance(p); c > most) {
                    most = c;
                    found = p;
                }
            }
        }
    }
    return found;
}

// ---------------------------------------------------------------------------------------------

Arrangement::Arrangement(const Sketch& s, Id layer, double arcStep)
{
    std::vector<Curve> curves;
    for (const auto& [id, e] : s.entities()) {
        if (e.kind() == EntityKind::Point || e.construction || s.isReference(id) || s.layerOf(id) != layer) {
            continue;
        }
        Curve c;
        c.id = id;
        if (const auto* l = std::get_if<Line>(&e.geom)) {
            c.a = {s.point(l->p1).x, s.point(l->p1).y};
            c.b = {s.point(l->p2).x, s.point(l->p2).y};
        }
        else if (const auto* circle = std::get_if<Circle>(&e.geom)) {
            c.kind = Curve::Kind::Circle;
            c.c = {s.point(circle->center).x, s.point(circle->center).y};
            c.r = circle->radius;
        }
        else {
            const ArcShape shape = arcShape(s, id);
            c.kind = Curve::Kind::Arc;
            c.c = shape.center;
            c.r = shape.radius;
            c.start = shape.startAngle;
            c.sweep = shape.sweep;
        }
        curves.push_back(c);
    }
    if (curves.empty()) {
        return;
    }

    // Where each curve is cut: its ends, and wherever it meets another curve.
    std::vector<std::vector<double>> cuts(curves.size());
    double extent = 1.0;
    for (size_t i = 0; i < curves.size(); ++i) {
        const Curve& c = curves[i];
        if (c.kind == Curve::Kind::Line) {
            cuts[i] = {0.0, 1.0};
            extent = std::max({extent, std::abs(c.a.x), std::abs(c.a.y), std::abs(c.b.x), std::abs(c.b.y)});
        }
        else {
            if (c.kind == Curve::Kind::Arc) {
                cuts[i] = {0.0, c.sweep};
            }
            extent = std::max({extent, std::abs(c.c.x) + c.r, std::abs(c.c.y) + c.r});
        }
    }
    for (size_t i = 0; i < curves.size(); ++i) {
        for (size_t j = i + 1; j < curves.size(); ++j) {
            for (const Vec2& p : modify::intersections(s, curves[i].id, curves[j].id)) {
                cuts[i].push_back(curves[i].param(p));
                cuts[j].push_back(curves[j].param(p));
            }
        }
    }
    const double tol = 1e-6 * extent;  // positions closer than this are one vertex

    // An end lying on another curve cuts it too. Intersections usually find these, but not a
    // line ending where it just touches a circle: a double root, which rounding can lose.
    for (size_t i = 0; i < curves.size(); ++i) {
        const Curve& c = curves[i];
        if (c.kind == Curve::Kind::Circle) {
            continue;
        }
        for (const Vec2 end : {c.at(0.0), c.at(c.kind == Curve::Kind::Line ? 1.0 : c.sweep)}) {
            for (size_t j = 0; j < curves.size(); ++j) {
                if (j != i) {
                    const double t = curves[j].param(end);
                    if (norm(sub(curves[j].at(t), end)) <= tol) {
                        cuts[j].push_back(t);
                    }
                }
            }
        }
    }

    // Vertices (merged by position) and the pieces between cuts, as half-edges.
    std::vector<Vec2> vertices;
    auto vertexAt = [&](Vec2 p) {
        for (size_t v = 0; v < vertices.size(); ++v) {
            if (norm(sub(vertices[v], p)) <= tol) {
                return static_cast<int>(v);
            }
        }
        vertices.push_back(p);
        return static_cast<int>(vertices.size() - 1);
    };
    struct HalfEdge
    {
        int from = 0, to = 0, twin = 0;
        size_t piece = 0;
        Id curve = kNoId;
        std::vector<Vec2> points;  // from -> to
        double angle = 0.0;        // leaving `from`
    };
    std::vector<HalfEdge> edges;
    size_t pieces = 0;
    std::map<std::pair<int, int>, std::vector<Vec2>> piecesBetween;  // (vertices) -> midpoints of pieces
    for (size_t i = 0; i < curves.size(); ++i) {
        const Curve& c = curves[i];
        std::vector<double> t = cuts[i];
        std::ranges::sort(t);
        std::vector<double> unique;
        for (double v : t) {
            if (unique.empty() || c.spanLength(unique.back(), v) > tol) {
                unique.push_back(v);
            }
        }
        std::vector<std::pair<double, double>> spans;
        if (c.kind == Curve::Kind::Circle) {
            if (unique.empty()) {
                unique.push_back(0.0);
            }
            if (unique.size() > 1 && c.spanLength(unique.back(), unique.front() + kTwoPi) <= tol) {
                unique.pop_back();  // the last cut is the first one again
            }
            for (size_t k = 0; k < unique.size(); ++k) {
                spans.push_back({unique[k], k + 1 < unique.size() ? unique[k + 1] : unique.front() + kTwoPi});
            }
        }
        else {
            for (size_t k = 0; k + 1 < unique.size(); ++k) {
                spans.push_back({unique[k], unique[k + 1]});
            }
        }
        for (const auto& [t0, t1] : spans) {
            if (c.spanLength(t0, t1) <= tol) {
                continue;
            }
            std::vector<Vec2> pts;
            const int n = c.kind == Curve::Kind::Line
                ? 1
                : std::max(2, static_cast<int>(std::ceil((t1 - t0) / arcStep)));
            for (int k = 0; k <= n; ++k) {
                pts.push_back(c.at(t0 + (t1 - t0) * k / n));
            }
            const int v0 = vertexAt(pts.front()), v1 = vertexAt(pts.back());
            pts.front() = vertices[static_cast<size_t>(v0)];
            pts.back() = vertices[static_cast<size_t>(v1)];
            // A piece lying on top of one already here (curves drawn twice over each other, e.g.
            // a side left doubled by an old fillet) is the same edge: kept twice, it would make
            // zero-width slivers that break the faces around it.
            const Vec2 middle = c.at(t0 + (t1 - t0) / 2);
            auto& alongside = piecesBetween[{std::min(v0, v1), std::max(v0, v1)}];
            if (std::ranges::any_of(alongside, [&](Vec2 m) { return norm(sub(m, middle)) <= 100 * tol; })) {
                continue;
            }
            alongside.push_back(middle);
            HalfEdge fwd{v0, v1, static_cast<int>(edges.size()) + 1, pieces, c.id, pts, 0.0};
            std::vector<Vec2> back(pts.rbegin(), pts.rend());
            HalfEdge bwd{v1, v0, static_cast<int>(edges.size()), pieces, c.id, back, 0.0};
            edges.push_back(std::move(fwd));
            edges.push_back(std::move(bwd));
            ++pieces;
        }
    }
    if (edges.empty()) {
        return;
    }

    // Around each vertex, the half-edges leaving it in counter-clockwise order.
    std::vector<std::vector<int>> leaving(vertices.size());
    UnionFind components{std::vector<int>(vertices.size())};
    std::iota(components.parent.begin(), components.parent.end(), 0);
    for (size_t h = 0; h < edges.size(); ++h) {
        HalfEdge& e = edges[h];
        const Vec2 d = sub(e.points[1], e.points[0]);
        e.angle = std::atan2(d.y, d.x);
        leaving[static_cast<size_t>(e.from)].push_back(static_cast<int>(h));
        components.unite(e.from, e.to);
    }
    for (auto& list : leaving) {
        std::ranges::sort(list, {}, [&](int h) { return edges[static_cast<size_t>(h)].angle; });
    }
    for (size_t v = 0; v < vertices.size(); ++v) {
        if (leaving[v].size() == 1) {
            m_looseEnds.push_back(vertices[v]);
        }
    }

    // Faces: from each half-edge, turn as far right as possible at every vertex; the face is
    // then on the left, so outlines of areas run counter-clockwise.
    auto next = [&](int h) {
        const HalfEdge& e = edges[static_cast<size_t>(h)];
        const std::vector<int>& around = leaving[static_cast<size_t>(e.to)];
        const auto it = std::ranges::find(around, e.twin);
        const size_t i = static_cast<size_t>(it - around.begin());
        return around[(i + around.size() - 1) % around.size()];
    };
    std::vector<bool> seen(edges.size(), false);
    for (size_t start = 0; start < edges.size(); ++start) {
        if (seen[start]) {
            continue;
        }
        Cycle cycle;
        std::map<size_t, int> pieceCount;
        int h = static_cast<int>(start);
        size_t guard = 0;
        do {
            seen[static_cast<size_t>(h)] = true;
            const HalfEdge& e = edges[static_cast<size_t>(h)];
            cycle.polygon.insert(cycle.polygon.end(), e.points.begin(), e.points.end() - 1);
            ++pieceCount[e.piece];
            h = next(h);
        } while (h != static_cast<int>(start) && ++guard <= edges.size());
        for (const auto& [piece, count] : pieceCount) {
            if (count == 1) {  // walked one way only: a real edge, not a dead end
                const Id curve = edges[2 * piece].curve;
                if (std::ranges::find(cycle.curves, curve) == cycle.curves.end()) {
                    cycle.curves.push_back(curve);
                }
            }
        }
        std::ranges::sort(cycle.curves);
        cycle.area = cycle.polygon.size() >= 3 ? signedArea(cycle.polygon) : 0.0;
        cycle.component = components.find(edges[start].from);
        m_cycles.push_back(std::move(cycle));
    }
}

std::optional<size_t> Arrangement::smallestAround(Vec2 p, int exceptComponent) const
{
    std::optional<size_t> best;
    for (size_t i = 0; i < m_cycles.size(); ++i) {
        const Cycle& c = m_cycles[i];
        if (c.area <= 1e-12 || c.component == exceptComponent || !pointInPolygon(c.polygon, p)) {
            continue;
        }
        if (!best || c.area < m_cycles[*best].area) {
            best = i;
        }
    }
    return best;
}

Region Arrangement::regionFor(size_t i) const
{
    const Cycle& face = m_cycles[i];
    Region r;
    r.outer = face.polygon;
    r.boundary = face.curves;
    r.area = face.area;
    // Islands: separate pieces of drawing whose nearest enclosing area is this one.
    for (const Cycle& c : m_cycles) {
        if (c.area >= -1e-12 || c.component == face.component || !pointInPolygon(face.polygon, c.polygon.front())) {
            continue;
        }
        if (smallestAround(c.polygon.front(), c.component) == i) {
            r.holes.push_back(c.polygon);
        }
    }
    return r;
}

std::optional<Region> Arrangement::regionAt(Vec2 p) const
{
    if (const auto i = smallestAround(p, -1)) {
        return regionFor(*i);
    }
    return std::nullopt;
}

std::optional<Region> Arrangement::regionBounded(const std::vector<Id>& boundary, Vec2 near) const
{
    // Among the areas with this outline: one around `near` (the smallest, should they nest), else
    // the one whose outline passes closest to it.
    auto distance = [&](const Cycle& c) {
        if (pointInPolygon(c.polygon, near)) {
            return 0.0;
        }
        double d = std::numeric_limits<double>::infinity();
        for (size_t k = 0; k < c.polygon.size(); ++k) {
            d = std::min(d, segmentDistance(near, c.polygon[k], c.polygon[(k + 1) % c.polygon.size()]));
        }
        return d;
    };
    std::optional<size_t> best;
    double bestDistance = 0.0;
    for (size_t i = 0; i < m_cycles.size(); ++i) {
        const Cycle& c = m_cycles[i];
        if (c.area <= 1e-12 || c.curves != boundary) {
            continue;
        }
        const double d = distance(c);
        if (!best || d < bestDistance || (d == bestDistance && c.area < m_cycles[*best].area)) {
            best = i;
            bestDistance = d;
        }
    }
    if (best) {
        return regionFor(*best);
    }
    return std::nullopt;
}

std::vector<Vec2> Arrangement::looseEnds() const
{
    return m_looseEnds;
}

std::optional<Region> resolve(const Arrangement& arrangement, const Fill& fill)
{
    if (!fill.boundary.empty()) {
        if (auto r = arrangement.regionBounded(fill.boundary, {fill.seed.x, fill.seed.y})) {
            return r;
        }
    }
    return arrangement.regionAt({fill.seed.x, fill.seed.y});
}

const Arrangement& Arrangements::of(Id layer) const
{
    auto it = m_byLayer.find(layer);
    if (it == m_byLayer.end()) {
        it = m_byLayer.emplace(layer, Arrangement(m_sketch, layer, m_arcStep)).first;
    }
    return it->second;
}

std::optional<Region> resolve(const Arrangements& arrangements, const Fill& fill)
{
    return resolve(arrangements.of(fill.layer), fill);
}

void refreshFills(Sketch& s)
{
    if (s.fills().empty()) {
        return;
    }
    const Arrangements arrangements(s);
    std::vector<Fill> updated;
    for (const auto& [id, fill] : s.fills()) {
        const auto region = resolve(arrangements, fill);
        if (!region) {
            continue;  // lost: keep looking where it was
        }
        Fill f = fill;
        f.boundary = region->boundary;
        if (!region->contains({f.seed.x, f.seed.y})) {
            const Vec2 inside = region->interiorPoint();
            f.seed = Point{inside.x, inside.y};
        }
        if (f.boundary != fill.boundary || f.seed.x != fill.seed.x || f.seed.y != fill.seed.y) {
            updated.push_back(std::move(f));
        }
    }
    for (const Fill& f : updated) {
        s.setFill(f);
    }
}

}  // namespace core::regions
