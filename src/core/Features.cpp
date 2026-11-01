#include "Features.h"

#include <algorithm>
#include <cmath>
#include <functional>
#include <map>
#include <numbers>
#include <optional>
#include <set>
#include <tuple>

namespace core::feature
{

namespace
{
constexpr double kDegToRad = std::numbers::pi / 180.0;

Vec2 at(const Sketch& s, Id point)
{
    const Point& p = s.point(point);
    return {p.x, p.y};
}

Vec2 operator+(Vec2 a, Vec2 b) { return {a.x + b.x, a.y + b.y}; }
Vec2 operator-(Vec2 a, Vec2 b) { return {a.x - b.x, a.y - b.y}; }
Vec2 operator*(double k, Vec2 v) { return {k * v.x, k * v.y}; }
double dot(Vec2 a, Vec2 b) { return a.x * b.x + a.y * b.y; }
double cross(Vec2 a, Vec2 b) { return a.x * b.y - a.y * b.x; }
double norm(Vec2 v) { return std::hypot(v.x, v.y); }
Vec2 unit(Vec2 v)
{
    const double n = norm(v);
    return n > 0.0 ? Vec2{v.x / n, v.y / n} : Vec2{1.0, 0.0};
}
Vec2 left(Vec2 v) { return {-v.y, v.x}; }

int countParam(double v)
{
    return static_cast<int>(std::lround(v));
}

// Instances along one direction: `count`, or with symmetric, `count` each way sharing the original.
size_t along(int count, bool symmetric)
{
    return symmetric ? static_cast<size_t>(2 * count - 1) : static_cast<size_t>(count);
}

// Step number of the n-th instance along a direction: 0, 1, 2, ... then (symmetric) -1, -2, ...
double stepOf(size_t n, int count)
{
    return n < static_cast<size_t>(count) ? double(n) : -double(n - static_cast<size_t>(count) + 1);
}

Affine linearAt(const LinearPatternParams& p, size_t k)
{
    const size_t across = along(p.count, p.symmetric);
    const double i = stepOf(k % across, p.count), j = stepOf(k / across, p.count2);
    const double a = p.angle * kDegToRad;
    const Vec2 u{std::cos(a), std::sin(a)}, v{-std::sin(a), std::cos(a)};
    const Vec2 d = (i * p.spacing) * u + (j * p.spacing2) * v;
    return Affine::translation(d.x, d.y);
}

std::vector<double> linearParamVector(const LinearPatternParams& p)
{
    return {double(p.count), p.angle, double(p.count2), p.spacing2, p.symmetric ? 1.0 : 0.0, p.symmetric2 ? 1.0 : 0.0};
}

Affine circularAt(const CircularPatternParams& p, Vec2 center, size_t k)
{
    const bool fullTurn = p.angle >= 360.0 - 1e-9;
    const double step = fullTurn ? 360.0 / p.count : p.angle / (p.count - 1);
    return Affine::rotation(center, double(k) * step);
}

// The selection's sources plus one copy of them per further instance.
std::vector<Id> makeCopies(Sketch& s, const std::vector<Id>& sources, size_t instances,
                           const std::function<Affine(size_t)>& transformFor)
{
    std::vector<Id> refs = sources;
    for (size_t k = 1; k < instances; ++k) {
        const std::map<Id, Id> map = xform::copy(s, sources, transformFor(k), false);
        for (Id src : sources) {
            refs.push_back(map.at(src));
        }
    }
    return refs;
}

void removeCopies(Sketch& s, Id id)
{
    const Pattern p = pattern(s.constraint(id));
    for (const auto& instance : p.copies) {
        for (Id e : instance) {
            if (s.hasEntity(e)) {
                s.remove(e);
            }
        }
    }
    if (s.hasConstraint(id)) {
        s.remove(id);
    }
}

// --- chains ---

// A line's or arc's ends, in their own order (p1/p2, start/end).
std::optional<std::pair<Id, Id>> endsOf(const Sketch& s, Id curve)
{
    const Entity& e = s.entity(curve);
    if (const auto* l = std::get_if<Line>(&e.geom)) {
        return std::pair{l->p1, l->p2};
    }
    if (const auto* a = std::get_if<Arc>(&e.geom)) {
        return std::pair{a->start, a->end};
    }
    return std::nullopt;
}

struct Step
{
    Id curve = kNoId;
    bool forward = true;  // walked from its first end to its second
    Id entry = kNoId, exit = kNoId;
};

// Orders curves into one chain; throws if they branch or aren't connected.
std::vector<Step> orderChain(const Sketch& s, const std::vector<Id>& curves, bool& closed)
{
    std::map<Id, std::vector<Id>> at;  // point -> curves ending there
    for (Id c : curves) {
        const auto ends = endsOf(s, c);
        if (!ends) {
            throw SketchError("offset: only lines and arcs can be chained");
        }
        at[ends->first].push_back(c);
        at[ends->second].push_back(c);
    }
    Id start = curves.front();
    Id entry = endsOf(s, start)->first;
    closed = true;
    for (const auto& [p, list] : at) {
        if (list.size() > 2) {
            throw SketchError("offset: the curves branch");
        }
        if (list.size() == 1 && closed) {
            closed = false;
            start = list.front();
            entry = p;
        }
    }

    std::vector<Step> steps;
    std::set<Id> visited;
    Id current = start;
    while (current != kNoId && !visited.contains(current)) {
        visited.insert(current);
        const auto ends = *endsOf(s, current);
        const bool forward = ends.first == entry;
        const Id exit = forward ? ends.second : ends.first;
        steps.push_back({current, forward, entry, exit});
        Id next = kNoId;
        for (Id c : at[exit]) {
            if (c != current && !visited.contains(c)) {
                next = c;
            }
        }
        current = next;
        entry = exit;
    }
    if (steps.size() != curves.size()) {
        throw SketchError("offset: the curves aren't one connected chain");
    }
    return steps;
}

// Direction of travel along a step at one of its points.
Vec2 tangentAt(const Sketch& s, const Step& step, Vec2 x)
{
    const Entity& e = s.entity(step.curve);
    Vec2 t;
    if (const auto* l = std::get_if<Line>(&e.geom)) {
        t = at(s, l->p2) - at(s, l->p1);
    }
    else {
        t = left(x - at(s, std::get<Arc>(e.geom).center));  // counter-clockwise
    }
    return unit(step.forward ? t : -1.0 * t);
}

// An offset curve as a line (through a, b) or a circle.
struct Prim
{
    bool line = true;
    Vec2 a, b;
    Vec2 c;
    double r = 0.0;
};

std::vector<Vec2> intersect(const Prim& p, const Prim& q)
{
    if (p.line && q.line) {
        const Vec2 d1 = p.b - p.a, d2 = q.b - q.a;
        const double den = cross(d1, d2);
        if (std::abs(den) < 1e-12 * norm(d1) * norm(d2)) {
            return {};
        }
        const double t = cross(q.a - p.a, d2) / den;
        return {p.a + t * d1};
    }
    if (!p.line && q.line) {
        return intersect(q, p);
    }
    if (p.line) {  // line and circle
        const Vec2 d = p.b - p.a;
        const Vec2 f = p.a - q.c;
        const double A = dot(d, d), B = 2 * dot(f, d), C = dot(f, f) - q.r * q.r;
        const double disc = B * B - 4 * A * C;
        if (disc < 0 || A == 0.0) {
            return {};
        }
        const double sq = std::sqrt(disc);
        return {p.a + ((-B - sq) / (2 * A)) * d, p.a + ((-B + sq) / (2 * A)) * d};
    }
    const double dist = norm(q.c - p.c);  // two circles
    if (dist == 0.0 || dist > p.r + q.r || dist < std::abs(p.r - q.r)) {
        return {};
    }
    const double a = (p.r * p.r - q.r * q.r + dist * dist) / (2 * dist);
    const double h = std::sqrt(std::max(0.0, p.r * p.r - a * a));
    const Vec2 u = unit(q.c - p.c);
    const Vec2 mid = p.c + a * u;
    return {mid + h * left(u), mid - h * left(u)};
}
}  // namespace

// ---------------------------------------------------------------------------------------------
// Patterns
// ---------------------------------------------------------------------------------------------

Pattern pattern(const Constraint& c)
{
    Pattern p;
    size_t first = 0, groups = 0;
    if (c.type == ConstraintType::CircularPattern) {
        p.center = c.refs.at(0);
        first = 1;
        groups = static_cast<size_t>(countParam(c.params.at(0)));
    }
    else if (c.type == ConstraintType::LinearPattern) {
        const LinearPatternParams lp = linearParams(c);
        groups = along(lp.count, lp.symmetric) * along(lp.count2, lp.symmetric2);
    }
    else {
        throw SketchError("not a pattern");
    }
    const size_t n = (c.refs.size() - first) / groups;
    p.sources.assign(c.refs.begin() + first, c.refs.begin() + first + n);
    for (size_t g = 1; g < groups; ++g) {
        const auto from = c.refs.begin() + first + g * n;
        p.copies.emplace_back(from, from + n);
    }
    return p;
}

LinearPatternParams linearParams(const Constraint& c)
{
    const bool sym = c.params.size() > 4 && c.params[4] != 0.0, sym2 = c.params.size() > 5 && c.params[5] != 0.0;
    return {countParam(c.params.at(0)), c.value, c.params.at(1), countParam(c.params.at(2)), c.params.at(3), sym, sym2};
}

CircularPatternParams circularParams(const Constraint& c)
{
    return {countParam(c.params.at(0)), c.value};
}

Affine instanceTransform(const Sketch& s, const Constraint& c, size_t k)
{
    if (c.type == ConstraintType::CircularPattern) {
        return circularAt(circularParams(c), at(s, c.refs.at(0)), k);
    }
    return linearAt(linearParams(c), k);
}

std::vector<Id> patternSources(const Sketch& s, const std::vector<Id>& selection)
{
    std::set<Id> curvePoints;
    for (Id id : selection) {
        if (s.hasEntity(id) && s.entity(id).kind() != EntityKind::Point) {
            for (Id p : s.definingPoints(id)) {
                curvePoints.insert(p);
            }
        }
    }
    std::set<Id> out;
    for (Id id : selection) {
        if (!s.hasEntity(id) || s.isReference(id) || curvePoints.contains(id)) {
            continue;
        }
        out.insert(id);
    }
    return {out.begin(), out.end()};
}

Id linearPattern(Sketch& s, const std::vector<Id>& selection, const LinearPatternParams& p)
{
    const std::vector<Id> sources = patternSources(s, selection);
    if (sources.empty()) {
        throw SketchError("pattern: nothing selected to repeat");
    }
    if (p.count < 1 || p.count2 < 1 || along(p.count, p.symmetric) * along(p.count2, p.symmetric2) < 2) {
        throw SketchError("pattern: needs at least two instances");
    }
    if (!(p.spacing > 0.0) || !(p.spacing2 != 0.0)) {
        throw SketchError("pattern: spacing must be positive (the second one non-zero: negative goes the other way)");
    }
    const size_t total = along(p.count, p.symmetric) * along(p.count2, p.symmetric2);
    std::vector<Id> refs = makeCopies(s, sources, total, [&](size_t k) { return linearAt(p, k); });
    return s.addConstraint(ConstraintType::LinearPattern, std::move(refs), p.spacing, linearParamVector(p));
}

Id circularPattern(Sketch& s, const std::vector<Id>& selection, Id center, const CircularPatternParams& p)
{
    if (!s.hasEntity(center) || s.entity(center).kind() != EntityKind::Point) {
        throw SketchError("pattern: the centre must be a point");
    }
    std::vector<Id> sources = patternSources(s, selection);
    std::erase(sources, center);
    if (sources.empty()) {
        throw SketchError("pattern: nothing selected to repeat");
    }
    if (p.count < 2) {
        throw SketchError("pattern: needs at least two instances");
    }
    if (!(p.angle > 0.0 && p.angle <= 360.0)) {
        throw SketchError("pattern: the angle must be more than 0 and at most 360 degrees");
    }
    const Vec2 c = at(s, center);
    std::vector<Id> refs{center};
    for (Id r : makeCopies(s, sources, static_cast<size_t>(p.count), [&](size_t k) { return circularAt(p, c, k); })) {
        refs.push_back(r);
    }
    return s.addConstraint(ConstraintType::CircularPattern, std::move(refs), p.angle, {double(p.count)});
}

Id setLinearPattern(Sketch& s, Id id, const LinearPatternParams& p)
{
    const Constraint& c = s.constraint(id);
    if (c.type != ConstraintType::LinearPattern) {
        throw SketchError("not a rectangular pattern");
    }
    const LinearPatternParams old = linearParams(c);
    if (old.count == p.count && old.count2 == p.count2 && old.symmetric == p.symmetric
        && old.symmetric2 == p.symmetric2) {
        s.setConstraintParams(id, linearParamVector(p));
        s.setConstraintValue(id, p.spacing);
        placeCopies(s, id);
        return id;
    }
    const std::vector<Id> sources = pattern(c).sources;
    removeCopies(s, id);
    return linearPattern(s, sources, p);
}

Id setCircularPattern(Sketch& s, Id id, const CircularPatternParams& p)
{
    const Constraint& c = s.constraint(id);
    if (c.type != ConstraintType::CircularPattern) {
        throw SketchError("not a circular pattern");
    }
    if (circularParams(c).count == p.count) {
        s.setConstraintValue(id, p.angle);
        placeCopies(s, id);
        return id;
    }
    const Pattern old = pattern(c);
    removeCopies(s, id);
    return circularPattern(s, old.sources, old.center, p);
}

void placeCopies(Sketch& s, Id id)
{
    const Constraint& c = s.constraint(id);
    const Pattern p = pattern(c);
    for (size_t k = 0; k < p.copies.size(); ++k) {
        const Affine t = instanceTransform(s, c, k + 1);
        for (size_t i = 0; i < p.sources.size(); ++i) {
            const std::vector<Id> from = s.definingPoints(p.sources[i]);
            const std::vector<Id> to = s.definingPoints(p.copies[k][i]);
            for (size_t j = 0; j < from.size(); ++j) {
                const Vec2 q = t.apply(at(s, from[j]));
                s.point(to[j]) = Point{q.x, q.y};
            }
            if (s.entity(p.sources[i]).kind() == EntityKind::Circle) {
                s.circle(p.copies[k][i]).radius = s.circle(p.sources[i]).radius;
            }
        }
    }
}

std::vector<Id> sourcePoints(const Sketch& s, const Constraint& c)
{
    std::vector<Id> sources;
    if (c.type == ConstraintType::Offset) {
        sources = offsetRefs(c).sources;
    }
    else {
        const Pattern p = pattern(c);
        sources = p.sources;
        if (p.center != kNoId) {
            sources.push_back(p.center);
        }
    }
    std::set<Id> points;
    for (Id e : sources) {
        if (s.hasEntity(e)) {
            for (Id p : s.definingPoints(e)) {
                points.insert(p);
            }
        }
    }
    return {points.begin(), points.end()};
}

// ---------------------------------------------------------------------------------------------
// Offsets
// ---------------------------------------------------------------------------------------------

OffsetRefs offsetRefs(const Constraint& c)
{
    if (c.type != ConstraintType::Offset) {
        throw SketchError("not an offset");
    }
    const bool both = !c.params.empty() && c.params[0] == 2.0;
    const size_t caps = c.params.size() > 1 ? static_cast<size_t>(c.params[1]) : 0;
    const size_t n = (c.refs.size() - caps) / (both ? 3 : 2);
    OffsetRefs out{{c.refs.begin(), c.refs.begin() + n}, {c.refs.begin() + n, c.refs.begin() + 2 * n}, {}, {}};
    if (both) {
        out.offsets2.assign(c.refs.begin() + 2 * n, c.refs.begin() + 3 * n);
    }
    out.caps.assign(c.refs.end() - static_cast<std::ptrdiff_t>(caps), c.refs.end());
    return out;
}

std::vector<Id> chainAt(const Sketch& s, Id curve)
{
    if (!endsOf(s, curve)) {
        if (s.entity(curve).kind() == EntityKind::Circle) {
            return {curve};
        }
        throw SketchError("offset: pick a line, arc or circle");
    }
    const bool construction = s.entity(curve).construction;
    std::map<Id, std::vector<Id>> at;
    for (const auto& [id, e] : s.entities()) {
        if (e.construction != construction || s.isReference(id)) {
            continue;
        }
        if (const auto ends = endsOf(s, id)) {
            at[ends->first].push_back(id);
            at[ends->second].push_back(id);
        }
    }
    std::set<Id> found{curve};
    std::vector<Id> todo{curve};
    while (!todo.empty()) {
        const Id c = todo.back();
        todo.pop_back();
        const auto ends = *endsOf(s, c);
        for (Id p : {ends.first, ends.second}) {
            if (at[p].size() != 2) {
                continue;  // a free end, or a branch: the chain stops here
            }
            for (Id other : at[p]) {
                if (found.insert(other).second) {
                    todo.push_back(other);
                }
            }
        }
    }
    // In order along the chain.
    const std::vector<Id> curves(found.begin(), found.end());
    bool closed = false;
    std::vector<Id> ordered;
    for (const Step& step : orderChain(s, curves, closed)) {
        ordered.push_back(step.curve);
    }
    return ordered;
}

namespace
{
// One side of a chain's offset: `sign` +1 is left of the direction of travel, -1 right.
// Returns the new curves (in chain order) and, per vertex of the chain, the new point there.
struct Side
{
    std::vector<Id> curves;
    std::map<Id, Id> points;
};

Side offsetSide(Sketch& s, const std::vector<Step>& steps, bool closed, double sign, double d)
{
    // Each curve's offset, as a primitive.
    std::vector<Prim> prims;
    for (const Step& step : steps) {
        const Entity& e = s.entity(step.curve);
        Prim prim;
        if (const auto* l = std::get_if<Line>(&e.geom)) {
            const Vec2 n = sign * d * left(tangentAt(s, step, {}));
            prim.a = at(s, l->p1) + n;
            prim.b = at(s, l->p2) + n;
        }
        else {
            const Arc& a = std::get<Arc>(e.geom);
            prim.line = false;
            prim.c = at(s, a.center);
            prim.r = s.arcRadius(step.curve) + (step.forward ? -sign * d : sign * d);
            if (!(prim.r > 0.0)) {
                throw SketchError("offset: the distance is larger than an arc's radius");
            }
        }
        prims.push_back(prim);
    }
    auto offsetOf = [&](size_t i, Id point) {
        const Vec2 x = at(s, point);
        return x + (sign * d) * left(tangentAt(s, steps[i], x));
    };

    // The new point at each vertex of the chain.
    std::map<Id, Vec2> where;
    const size_t n = steps.size();
    for (size_t i = 0; i < n; ++i) {
        const bool last = i + 1 == n;
        if (last && !closed) {
            where[steps[i].exit] = offsetOf(i, steps[i].exit);
            break;
        }
        const size_t j = last ? 0 : i + 1;
        const Id p = steps[i].exit;
        const Vec2 qa = offsetOf(i, p), qb = offsetOf(j, p);
        const Vec2 x = at(s, p);
        const bool smooth = std::abs(cross(tangentAt(s, steps[i], x), tangentAt(s, steps[j], x))) < 1e-6;
        Vec2 q = 0.5 * (qa + qb);
        if (smooth) {
            q = qa;
        }
        else {
            double best = 1e300;
            for (const Vec2& candidate : intersect(prims[i], prims[j])) {
                const double dist = norm(candidate - q);
                if (dist < best) {
                    best = dist;
                    q = candidate;
                }
            }
        }
        where[p] = q;
    }
    if (!closed) {
        where[steps[0].entry] = offsetOf(0, steps[0].entry);
    }

    Side side;
    for (const auto& [p, q] : where) {
        side.points[p] = s.addPoint(q.x, q.y);
        s.entity(side.points[p]).construction = s.entity(p).construction;
    }
    for (const Step& step : steps) {
        const Entity e = s.entity(step.curve);
        Id curve = kNoId;
        if (const auto* l = std::get_if<Line>(&e.geom)) {
            curve = s.addLine(side.points.at(l->p1), side.points.at(l->p2));
        }
        else {
            const Arc& a = std::get<Arc>(e.geom);
            curve = s.addArc(a.center, side.points.at(a.start), side.points.at(a.end));
        }
        s.entity(curve).construction = e.construction;
        s.copyStroke(step.curve, curve);
        side.curves.push_back(curve);
    }
    return side;
}
}  // namespace

Id offset(Sketch& s, const std::vector<Id>& curves, double d, Vec2 toward, bool bothSides, bool caps)
{
    if (!(d > 0.0)) {
        throw SketchError("offset: the distance must be positive");
    }
    if (curves.empty()) {
        throw SketchError("offset: nothing to offset");
    }
    const std::vector<double> params = bothSides ? std::vector<double>{2.0} : std::vector<double>{};

    if (curves.size() == 1 && s.entity(curves[0]).kind() == EntityKind::Circle) {
        const Circle circle = s.circle(curves[0]);
        const bool inside = norm(toward - at(s, circle.center)) < circle.radius;
        std::vector<double> radii{inside ? circle.radius - d : circle.radius + d};
        if (bothSides) {
            radii = {circle.radius + d, circle.radius - d};
        }
        std::vector<Id> refs{curves[0]};
        for (double r : radii) {
            if (!(r > 0.0)) {
                throw SketchError("offset: the distance is larger than the circle");
            }
            const Id made = s.addCircle(circle.center, r);
            s.entity(made).construction = s.entity(curves[0]).construction;
            s.copyStroke(curves[0], made);
            refs.push_back(made);
        }
        return s.addConstraint(ConstraintType::Offset, std::move(refs), d, params);
    }

    bool closed = false;
    const std::vector<Step> steps = orderChain(s, curves, closed);

    // Which side: left (+1) or right (-1) of the direction of travel, at the nearest curve.
    double sign = 1.0;
    if (!bothSides) {
        double best = 1e300;
        for (const Step& step : steps) {
            const double dist = distanceTo(s, step.curve, toward);
            if (dist < best) {
                best = dist;
                const Vec2 x = closestPoint(s, step.curve, toward);
                sign = cross(tangentAt(s, step, x), toward - x) >= 0.0 ? 1.0 : -1.0;
            }
        }
    }

    std::vector<Side> sides{offsetSide(s, steps, closed, sign, d)};
    if (bothSides) {
        sides.push_back(offsetSide(s, steps, closed, -sign, d));
    }

    // Caps: half circles round the ends of an open chain, joining the two sides.
    std::vector<Id> capArcs;
    if (bothSides && caps && !closed) {
        const Step& first = steps.front();
        const Step& last = steps.back();
        for (const auto& [end, step, outward] :
             {std::tuple{first.entry, first, -1.0}, std::tuple{last.exit, last, 1.0}}) {
            const Vec2 p = at(s, end);
            const Vec2 out = outward * tangentAt(s, step, p);  // away from the chain
            const Id qa = sides[0].points.at(end), qb = sides[1].points.at(end);
            // Counter-clockwise from qa, the half circle passes p + rot90(qa - p): that must be
            // the outward side, else go from qb.
            const bool fromA = dot(left(at(s, qa) - p), out) >= 0.0;
            const Id cap = fromA ? s.addArc(end, qa, qb) : s.addArc(end, qb, qa);
            s.entity(cap).construction = s.entity(step.curve).construction;
            s.copyStroke(step.curve, cap);
            capArcs.push_back(cap);
        }
    }

    std::vector<Id> refs;
    for (const Step& step : steps) {
        refs.push_back(step.curve);
    }
    for (const Side& side : sides) {
        refs.insert(refs.end(), side.curves.begin(), side.curves.end());
    }
    refs.insert(refs.end(), capArcs.begin(), capArcs.end());
    std::vector<double> settings = params;
    if (!capArcs.empty()) {
        settings.push_back(double(capArcs.size()));
    }
    return s.addConstraint(ConstraintType::Offset, std::move(refs), d, settings);
}

}  // namespace core::feature
