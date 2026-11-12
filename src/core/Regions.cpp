#include "Regions.h"

#include "Curves.h"
#include "Modify.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <map>
#include <numbers>
#include <numeric>
#include <unordered_map>

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
        Other,  // ellipse, elliptical arc, spline: by its path
    } kind = Kind::Line;
    Id id = kNoId;
    Vec2 a, b;  // line
    Vec2 c;     // arc / circle
    double r = 0.0, start = 0.0, sweep = kTwoPi;
    curve::Path path;  // other
    double x0 = 0, y0 = 0, x1 = 0, y1 = 0;  // its bounding box

    bool closed() const { return kind == Kind::Circle || (kind == Kind::Other && path.closed()); }
    double end() const { return kind == Kind::Line ? 1.0 : kind == Kind::Other ? path.end() : sweep; }

    Vec2 at(double t) const
    {
        if (kind == Kind::Line) {
            return {a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t};
        }
        if (kind == Kind::Other) {
            return path.at(t);
        }
        return {c.x + r * std::cos(start + t), c.y + r * std::sin(start + t)};
    }
    double param(Vec2 p) const
    {
        if (kind == Kind::Other) {
            return path.param(p);
        }
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
        if (kind == Kind::Other) {
            double total = 0;
            Vec2 prev = at(t0);
            for (int i = 1; i <= 8; ++i) {
                const Vec2 q = at(t0 + (t1 - t0) * i / 8);
                total += norm(sub(q, prev));
                prev = q;
            }
            return t1 >= t0 ? total : -total;
        }
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
    // Often the middle of the outline (its centroid) is well inside: then that.
    {
        double a = 0, cx = 0, cy = 0;
        for (size_t i = 0, j = outer.size() - 1; i < outer.size(); j = i++) {
            const double k = outer[j].x * outer[i].y - outer[i].x * outer[j].y;
            a += k;
            cx += (outer[j].x + outer[i].x) * k;
            cy += (outer[j].y + outer[i].y) * k;
        }
        if (std::abs(a) > 1e-300) {
            const Vec2 centroid{cx / (3 * a), cy / (3 * a)};
            if (contains(centroid) && clearance(centroid) >= 0.2 * std::min(x1 - x0, y1 - y0)) {
                return centroid;
            }
        }
    }
    // Else the point of a grid over the area that's furthest from any edge, refined around it.
    Vec2 found = outer.front();
    double most = -1.0;
    auto search = [&](double gx0, double gy0, double gx1, double gy1, int steps) {
        for (int i = 1; i < steps; ++i) {
            for (int j = 1; j < steps; ++j) {
                const Vec2 p{gx0 + (gx1 - gx0) * i / steps, gy0 + (gy1 - gy0) * j / steps};
                if (contains(p)) {
                    if (const double c = clearance(p); c > most) {
                        most = c;
                        found = p;
                    }
                }
            }
        }
    };
    constexpr int kSteps = 12;
    search(x0, y0, x1, y1, kSteps);
    if (most > 0) {
        const double w = (x1 - x0) / kSteps, h = (y1 - y0) / kSteps;
        search(found.x - w, found.y - h, found.x + w, found.y + h, 6);
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
        else if (e.kind() == EntityKind::Arc) {
            const ArcShape shape = arcShape(s, id);
            c.kind = Curve::Kind::Arc;
            c.c = shape.center;
            c.r = shape.radius;
            c.start = shape.startAngle;
            c.sweep = shape.sweep;
        }
        else {
            c.kind = Curve::Kind::Other;
            c.path = curve::pathOf(s, id);
        }
        // Its box (an arc's: its whole circle's; a path's: through its control points, a little
        // generous), to skip pairs of curves nowhere near each other.
        if (c.kind == Curve::Kind::Line) {
            c.x0 = std::min(c.a.x, c.b.x);
            c.x1 = std::max(c.a.x, c.b.x);
            c.y0 = std::min(c.a.y, c.b.y);
            c.y1 = std::max(c.a.y, c.b.y);
        }
        else if (c.kind == Curve::Kind::Other) {
            c.x0 = c.y0 = 1e300;
            c.x1 = c.y1 = -1e300;
            auto take = [&](Vec2 q) {
                c.x0 = std::min(c.x0, q.x);
                c.y0 = std::min(c.y0, q.y);
                c.x1 = std::max(c.x1, q.x);
                c.y1 = std::max(c.y1, q.y);
            };
            for (const curve::Bezier& b : c.path.beziers()) {  // (a spline: within its control points)
                for (const Vec2& q : b.p) {
                    take(q);
                }
            }
            for (const Vec2& q : c.path.polyline(16)) {  // (an ellipse: along it, then a little more)
                take(q);
            }
            const double pad = 0.02 * std::max(c.x1 - c.x0, c.y1 - c.y0);
            c.x0 -= pad;
            c.y0 -= pad;
            c.x1 += pad;
            c.y1 += pad;
        }
        else {
            c.x0 = c.c.x - c.r;
            c.x1 = c.c.x + c.r;
            c.y0 = c.c.y - c.r;
            c.y1 = c.c.y + c.r;
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
        else if (c.kind == Curve::Kind::Other) {
            if (!c.closed()) {
                cuts[i] = {0.0, c.end()};
            }
            for (const Vec2& q : c.path.polyline(8)) {
                extent = std::max({extent, std::abs(q.x), std::abs(q.y)});
            }
        }
        else {
            if (c.kind == Curve::Kind::Arc) {
                cuts[i] = {0.0, c.sweep};
            }
            extent = std::max({extent, std::abs(c.c.x) + c.r, std::abs(c.c.y) + c.r});
        }
    }
    const double tol = 1e-6 * extent;  // positions closer than this are one vertex

    // The pairs of curves whose boxes meet (a sweep along x): only they can cross or touch.
    std::vector<size_t> byLeft(curves.size());
    std::iota(byLeft.begin(), byLeft.end(), size_t{0});
    std::ranges::sort(byLeft, {}, [&](size_t i) { return curves[i].x0; });
    std::vector<std::pair<size_t, size_t>> near;
    for (size_t a = 0; a < byLeft.size(); ++a) {
        const Curve& ca = curves[byLeft[a]];
        for (size_t b = a + 1; b < byLeft.size() && curves[byLeft[b]].x0 <= ca.x1 + tol; ++b) {
            const Curve& cb = curves[byLeft[b]];
            if (cb.y0 <= ca.y1 + tol && ca.y0 <= cb.y1 + tol) {
                near.push_back({std::min(byLeft[a], byLeft[b]), std::max(byLeft[a], byLeft[b])});
            }
        }
    }
    // Crossings: lines, circles and arcs by formula; with an ellipse or spline, by their paths
    // (made once per curve, not once per pair), keeping only points on both curves.
    auto pathOf = [&](const Curve& c) {
        switch (c.kind) {
            case Curve::Kind::Line: return curve::Path::line(c.a, c.b);
            case Curve::Kind::Arc:
            case Curve::Kind::Circle: return curve::Path::circle(c.c, c.r);
            case Curve::Kind::Other: break;
        }
        return c.path;
    };
    std::vector<curve::Path> paths;
    paths.reserve(curves.size());
    for (const Curve& c : curves) {
        paths.push_back(c.kind == Curve::Kind::Other ? curve::Path{} : pathOf(c));
    }
    auto pathFor = [&](size_t i) -> const curve::Path& { return curves[i].kind == Curve::Kind::Other ? curves[i].path : paths[i]; };
    std::vector<std::vector<Vec2>> samples(curves.size());  // (made when first needed)
    auto samplesFor = [&](size_t i) -> const std::vector<Vec2>& {
        if (samples[i].empty()) {
            samples[i] = curve::crossingSamples(pathFor(i));
        }
        return samples[i];
    };
    auto on = [&](const Curve& c, Vec2 p) { return norm(sub(c.at(c.param(p)), p)) <= 100 * tol; };
    for (const auto& [i, j] : near) {
        const bool fancy = curves[i].kind == Curve::Kind::Other || curves[j].kind == Curve::Kind::Other;
        const std::vector<Vec2> xs = fancy ? curve::intersections(pathFor(i), samplesFor(i), pathFor(j), samplesFor(j))
                                           : modify::intersections(s, curves[i].id, curves[j].id);
        for (const Vec2& p : xs) {
            if (fancy && (!on(curves[i], p) || !on(curves[j], p))) {
                continue;  // (on an arc's circle, past its end)
            }
            cuts[i].push_back(curves[i].param(p));
            cuts[j].push_back(curves[j].param(p));
        }
    }

    // An end lying on another curve cuts it too. Intersections usually find these, but not a
    // line ending where it just touches a circle: a double root, which rounding can lose.
    auto endsOn = [&](size_t i, size_t j) {
        const Curve& c = curves[i];
        if (c.closed()) {
            return;
        }
        const Curve& other = curves[j];
        const Vec2 otherEnds[2] = {other.at(0.0), other.at(other.end())};
        for (const Vec2 end : {c.at(0.0), c.at(c.end())}) {
            if (end.x < other.x0 - tol || end.x > other.x1 + tol || end.y < other.y0 - tol || end.y > other.y1 + tol
                || (!other.closed() && (norm(sub(end, otherEnds[0])) <= tol || norm(sub(end, otherEnds[1])) <= tol))) {
                continue;  // (nowhere near it, or at its end: a cut already)
            }
            const double t = curves[j].param(end);
            if (norm(sub(curves[j].at(t), end)) <= tol) {
                cuts[j].push_back(t);
            }
        }
    };
    for (const auto& [i, j] : near) {
        endsOn(i, j);
        endsOn(j, i);
    }

    // Vertices (merged by position) and the pieces between cuts, as half-edges.
    std::vector<Vec2> vertices;
    std::unordered_map<long long, std::vector<int>> grid;  // (vertices by place, to find them quickly)
    auto cell = [&](double v) { return static_cast<long long>(std::floor(v / std::max(tol, 1e-300))); };
    auto vertexAt = [&](Vec2 p) {
        const long long cx = cell(p.x), cy = cell(p.y);
        for (long long dx = -1; dx <= 1; ++dx) {
            for (long long dy = -1; dy <= 1; ++dy) {
                const auto it = grid.find((cx + dx) * 1000003LL ^ (cy + dy));
                if (it == grid.end()) {
                    continue;
                }
                for (int v : it->second) {
                    if (norm(sub(vertices[static_cast<size_t>(v)], p)) <= tol) {
                        return v;
                    }
                }
            }
        }
        vertices.push_back(p);
        const int v = static_cast<int>(vertices.size() - 1);
        grid[cx * 1000003LL ^ cy].push_back(v);
        return v;
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
        if (c.closed()) {
            const double round = c.end();  // (2pi for a circle or ellipse)
            if (unique.empty()) {
                unique.push_back(0.0);
            }
            if (unique.size() > 1 && c.spanLength(unique.back(), unique.front() + round) <= tol) {
                unique.pop_back();  // the last cut is the first one again
            }
            for (size_t k = 0; k < unique.size(); ++k) {
                spans.push_back({unique[k], k + 1 < unique.size() ? unique[k + 1] : unique.front() + round});
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
            // (splines: a parameter unit is a whole Bezier piece, about a quarter turn's worth)
            const double perUnit = c.kind == Curve::Kind::Other && c.path.kind() == curve::Path::Kind::Spline
                ? (std::numbers::pi / 2) / arcStep
                : 1.0 / arcStep;
            const int n = c.kind == Curve::Kind::Line ? 1 : std::max(2, static_cast<int>(std::ceil((t1 - t0) * perUnit)));
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
        for (const Vec2& q : cycle.polygon) {
            cycle.x0 = std::min(cycle.x0, q.x);
            cycle.y0 = std::min(cycle.y0, q.y);
            cycle.x1 = std::max(cycle.x1, q.x);
            cycle.y1 = std::max(cycle.y1, q.y);
        }
        cycle.component = components.find(edges[start].from);
        m_cycles.push_back(std::move(cycle));
    }
}

std::optional<size_t> Arrangement::smallestAround(Vec2 p, int exceptComponent) const
{
    std::optional<size_t> best;
    for (size_t i = 0; i < m_cycles.size(); ++i) {
        const Cycle& c = m_cycles[i];
        if (c.area <= 1e-12 || c.component == exceptComponent || p.x < c.x0 || p.x > c.x1 || p.y < c.y0 || p.y > c.y1
            || !pointInPolygon(c.polygon, p)) {
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
        const Vec2 q = c.polygon.empty() ? Vec2{} : c.polygon.front();
        if (c.area >= -1e-12 || c.component == face.component || q.x < face.x0 || q.x > face.x1 || q.y < face.y0
            || q.y > face.y1 || !pointInPolygon(face.polygon, q)) {
            continue;
        }
        if (smallestAround(c.polygon.front(), c.component) == i) {
            r.holes.push_back(c.polygon);
        }
    }
    return r;
}

std::vector<Region> Arrangement::regions() const
{
    std::vector<Region> out;
    for (size_t i = 0; i < m_cycles.size(); ++i) {
        if (m_cycles[i].area > 1e-12) {  // (an area's outline: counter-clockwise)
            out.push_back(regionFor(i));
        }
    }
    return out;
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

void carryFills(Sketch& now, const Sketch& before)
{
    if (before.fills().empty()) {
        return;
    }
    // A curve moved as a whole: every point of it moved.
    auto movedWhole = [&](Id curve) {
        if (!before.hasEntity(curve) || !now.hasEntity(curve)) {
            return false;
        }
        const std::vector<Id> points = before.definingPoints(curve);
        return !points.empty() && std::ranges::all_of(points, [&](Id p) {
            if (!now.hasEntity(p)) {
                return false;
            }
            const Point& a = before.point(p);
            const Point& b = now.point(p);
            return std::hypot(b.x - a.x, b.y - a.y) > 1e-9;
        });
    };
    // Where only some of a fill's outline moved: whether what moved encloses it on its own (it's
    // the moving shape's: say one left half across a line by an earlier drag), found among the
    // curves that moved alone (the rest made construction, in a copy, made when first needed).
    std::optional<Sketch> movedOnly;
    std::optional<Arrangements> movedAreas;
    auto enclosedByMoved = [&](const Fill& f) {
        if (!movedOnly) {
            movedOnly = before;
            for (const auto& [id, e] : before.entities()) {
                if (e.kind() != EntityKind::Point && !e.construction && !movedWhole(id)) {
                    movedOnly->entity(id).construction = true;
                }
            }
            movedAreas.emplace(*movedOnly);
        }
        return movedAreas->of(f.layer).regionAt({f.seed.x, f.seed.y}).has_value();
    };

    std::vector<Fill> moved;
    for (const auto& [id, was] : before.fills()) {
        if (!now.hasFill(id) || was.boundary.empty()) {
            continue;
        }
        // The outline's curves that moved as a whole: where their points were and are.
        std::vector<Vec2> from, to;
        bool all = true;
        for (Id curve : was.boundary) {
            if (!movedWhole(curve)) {
                all = all && !(before.hasEntity(curve) && now.hasEntity(curve));  // (one that's gone doesn't count)
                continue;
            }
            for (Id p : before.definingPoints(curve)) {
                from.push_back({before.point(p).x, before.point(p).y});
                to.push_back({now.point(p).x, now.point(p).y});
            }
        }
        if (from.empty() || (!all && !enclosedByMoved(was))) {
            continue;  // (its outline stayed put, or it isn't the moving shape's)
        }
        // The turn and shift that best take them there (least squares), applied to the seed.
        Vec2 ca{}, cb{};
        for (size_t i = 0; i < from.size(); ++i) {
            ca = {ca.x + from[i].x, ca.y + from[i].y};
            cb = {cb.x + to[i].x, cb.y + to[i].y};
        }
        const double n = static_cast<double>(from.size());
        ca = {ca.x / n, ca.y / n};
        cb = {cb.x / n, cb.y / n};
        double sDot = 0, sCross = 0;
        for (size_t i = 0; i < from.size(); ++i) {
            const Vec2 a{from[i].x - ca.x, from[i].y - ca.y}, b{to[i].x - cb.x, to[i].y - cb.y};
            sDot += a.x * b.x + a.y * b.y;
            sCross += a.x * b.y - a.y * b.x;
        }
        const double turn = (std::abs(sDot) + std::abs(sCross) > 1e-12) ? std::atan2(sCross, sDot) : 0.0;
        const double c = std::cos(turn), sn = std::sin(turn);
        const Vec2 d{was.seed.x - ca.x, was.seed.y - ca.y};
        Fill f = now.fill(id);
        f.seed = Point{cb.x + c * d.x - sn * d.y, cb.y + sn * d.x + c * d.y};
        if (f.seed.x != now.fill(id).seed.x || f.seed.y != now.fill(id).seed.y) {
            moved.push_back(std::move(f));
        }
    }
    for (const Fill& f : moved) {
        now.setFill(f);
    }
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
