#include "Modify.h"

#include "Dimensions.h"

#include <algorithm>
#include <cmath>
#include <numbers>
#include <optional>

namespace core::modify
{

namespace
{
constexpr double kPi = std::numbers::pi;
constexpr double kTwoPi = 2.0 * std::numbers::pi;
constexpr double kPosEps = 1e-7;    // positions closer than this are the same point
constexpr double kParamEps = 1e-9;  // parameter slack at curve ends
using CT = ConstraintType;

Vec2 add(Vec2 a, Vec2 b) { return {a.x + b.x, a.y + b.y}; }
Vec2 sub(Vec2 a, Vec2 b) { return {a.x - b.x, a.y - b.y}; }
Vec2 mul(Vec2 a, double k) { return {a.x * k, a.y * k}; }
double dot(Vec2 a, Vec2 b) { return a.x * b.x + a.y * b.y; }
double cross(Vec2 a, Vec2 b) { return a.x * b.y - a.y * b.x; }
double norm(Vec2 a) { return std::hypot(a.x, a.y); }
double dist(Vec2 a, Vec2 b) { return norm(sub(b, a)); }
Vec2 unit(Vec2 a) { const double n = norm(a); return n > 0 ? mul(a, 1.0 / n) : Vec2{1, 0}; }
Vec2 rot90(Vec2 a) { return {-a.y, a.x}; }

double angleFrom(double start, double angle)
{
    double a = std::fmod(angle - start, kTwoPi);
    return a < 0 ? a + kTwoPi : a;
}

Vec2 pos(const Sketch& s, Id p)
{
    return {s.point(p).x, s.point(p).y};
}

// A curve's underlying geometry, with a parameter running along it: t in [0, 1] for lines,
// the angle from the start for arcs (0..sweep) and circles (0..2pi from +x).
struct Geo
{
    enum class Kind
    {
        Line,
        Circle,
        Arc,
    } kind = Kind::Line;
    Vec2 a, b;  // line
    Vec2 c;     // circle/arc
    double r = 0, start = 0, sweep = kTwoPi;

    double end() const { return kind == Kind::Line ? 1.0 : sweep; }

    double param(Vec2 p) const
    {
        if (kind == Kind::Line) {
            const Vec2 d = sub(b, a);
            return dot(sub(p, a), d) / dot(d, d);
        }
        return angleFrom(start, std::atan2(p.y - c.y, p.x - c.x));
    }

    Vec2 at(double u) const
    {
        if (kind == Kind::Line) {
            return add(a, mul(sub(b, a), u));
        }
        return {c.x + r * std::cos(start + u), c.y + r * std::sin(start + u)};
    }

    // Is p (on the underlying line/circle) within the curve's extent?
    bool contains(Vec2 p) const
    {
        if (kind == Kind::Circle) {
            return true;
        }
        const double u = param(p);
        if (kind == Kind::Line) {
            const double slack = kPosEps / std::max(norm(sub(b, a)), kPosEps);
            return u >= -slack && u <= 1 + slack;
        }
        const double slack = kPosEps / std::max(r, kPosEps);
        return u <= sweep + slack || u >= kTwoPi - slack;
    }

    std::vector<Vec2> sample(double from, double to, int n = 32) const
    {
        std::vector<Vec2> out;
        for (int i = 0; i <= n; ++i) {
            out.push_back(at(from + (to - from) * i / n));
        }
        return out;
    }
};

Geo geoOf(const Sketch& s, Id id)
{
    Geo g;
    const Entity& e = s.entity(id);
    switch (e.kind()) {
        case EntityKind::Line:
            g.kind = Geo::Kind::Line;
            g.a = pos(s, s.line(id).p1);
            g.b = pos(s, s.line(id).p2);
            break;
        case EntityKind::Circle:
            g.kind = Geo::Kind::Circle;
            g.c = pos(s, s.circle(id).center);
            g.r = s.circle(id).radius;
            break;
        case EntityKind::Arc: {
            const ArcShape a = arcShape(s, id);
            g.kind = Geo::Kind::Arc;
            g.c = a.center;
            g.r = a.radius;
            g.start = a.startAngle;
            g.sweep = a.sweep;
            break;
        }
        case EntityKind::Point: throw SketchError("not a curve");
    }
    return g;
}

// Crossings of the underlying infinite lines / full circles.
std::vector<Vec2> rawIntersect(const Geo& g1, const Geo& g2)
{
    const bool line1 = g1.kind == Geo::Kind::Line, line2 = g2.kind == Geo::Kind::Line;
    if (line1 && line2) {
        const Vec2 d1 = sub(g1.b, g1.a), d2 = sub(g2.b, g2.a);
        const double c = cross(d1, d2);
        if (std::abs(c) < 1e-12 * norm(d1) * norm(d2)) {
            return {};
        }
        return {add(g1.a, mul(d1, cross(sub(g2.a, g1.a), d2) / c))};
    }
    if (line1 || line2) {
        const Geo& l = line1 ? g1 : g2;
        const Geo& o = line1 ? g2 : g1;
        const Vec2 d = unit(sub(l.b, l.a));
        const Vec2 footP = add(l.a, mul(d, dot(sub(o.c, l.a), d)));
        const double h = dist(footP, o.c);
        if (h > o.r + kPosEps) {
            return {};
        }
        if (std::abs(h - o.r) <= kPosEps) {
            return {footP};  // tangent
        }
        const double half = std::sqrt(o.r * o.r - h * h);
        return {add(footP, mul(d, half)), sub(footP, mul(d, half))};
    }
    const double d = dist(g1.c, g2.c);
    if (d < kPosEps || d > g1.r + g2.r + kPosEps || d < std::abs(g1.r - g2.r) - kPosEps) {
        return {};
    }
    const double along = (g1.r * g1.r - g2.r * g2.r + d * d) / (2 * d);
    const double h2 = g1.r * g1.r - along * along;
    const Vec2 u = mul(sub(g2.c, g1.c), 1.0 / d);
    const Vec2 base = add(g1.c, mul(u, along));
    if (h2 <= kPosEps * kPosEps) {
        return {base};
    }
    const double h = std::sqrt(h2);
    return {add(base, mul(rot90(u), h)), sub(base, mul(rot90(u), h))};
}

bool isCutter(const Sketch& s, Id id, Id except)
{
    const Entity& e = s.entity(id);
    return id != except && e.kind() != EntityKind::Point && !e.construction && !s.isReference(id);
}

struct Cut
{
    double param = 0;
    Id cutter = kNoId;
    Vec2 at;
};

// Crossings with other curves strictly inside `curve` (anywhere round a circle).
std::vector<Cut> cutsOn(const Sketch& s, Id curve, const Geo& g)
{
    std::vector<Cut> cuts;
    for (const auto& [id, e] : s.entities()) {
        if (!isCutter(s, id, curve)) {
            continue;
        }
        for (const Vec2& p : intersections(s, curve, id)) {
            const double u = g.param(p);
            const double slack = g.kind == Geo::Kind::Line ? kPosEps / std::max(norm(sub(g.b, g.a)), kPosEps)
                                                           : kPosEps / std::max(g.r, kPosEps);
            if (g.kind == Geo::Kind::Circle || (u > slack && u < g.end() - slack)) {
                cuts.push_back({u, id, p});
            }
        }
    }
    std::ranges::sort(cuts, {}, &Cut::param);
    return cuts;
}

// The crossings either side of a parameter (circle: cyclically).
struct Span
{
    Geo g;
    double click = 0;
    std::optional<Cut> lo, hi;
    bool whole = false;  // no crossings: the operation applies to the whole curve
};

Span spanAt(const Sketch& s, Id curve, Vec2 near)
{
    Span sp;
    sp.g = geoOf(s, curve);
    sp.click = std::clamp(sp.g.param(near), 0.0, sp.g.end());
    const std::vector<Cut> cuts = cutsOn(s, curve, sp.g);
    for (const Cut& c : cuts) {
        if (c.param < sp.click) {
            sp.lo = c;
        }
        else if (!sp.hi) {
            sp.hi = c;
        }
    }
    if (sp.g.kind == Geo::Kind::Circle) {
        // Distinct crossings needed to cut a circle; the span wraps around 0.
        if (cuts.size() < 2 || std::abs(cuts.front().param - cuts.back().param) < kParamEps) {
            sp.whole = true;
            return sp;
        }
        if (!sp.lo) {
            sp.lo = cuts.back();
        }
        if (!sp.hi) {
            sp.hi = cuts.front();
        }
    }
    sp.whole = !sp.lo && !sp.hi;
    return sp;
}

// An existing end point of `cutter` at `at`, or a new point there kept on `cutter`.
Id pointOn(Sketch& s, Vec2 at, Id cutter)
{
    for (Id p : s.definingPoints(cutter)) {
        if (dist(pos(s, p), at) < 10 * kPosEps) {
            return p;
        }
    }
    const Id p = s.addPoint(at.x, at.y);
    s.addConstraint(CT::Coincident, {p, cutter});
    return p;
}

// Splits a line at a crossing: `line` keeps p1..cut, the returned new line is cut..p2.
Id splitLine(Sketch& s, Id line, const Cut& cut)
{
    const Id oldEnd = s.line(line).p2;
    const Id p = pointOn(s, cut.at, cut.cutter);
    s.rebindPoint(line, oldEnd, p);
    const Id piece = s.addLine(p, oldEnd);
    s.entity(piece).construction = s.entity(line).construction;
    s.addConstraint(CT::Collinear, {line, piece});
    return piece;
}

// Splits an arc at a crossing: `arc` keeps start..cut, the returned new arc is cut..end. They
// share the centre and the cut point, so they stay on one circle without an extra constraint.
Id splitArc(Sketch& s, Id arc, const Cut& cut)
{
    const Arc a = s.arc(arc);
    const Id p = pointOn(s, cut.at, cut.cutter);
    s.rebindPoint(arc, a.end, p);
    const Id piece = s.addArc(a.center, p, a.end);
    s.entity(piece).construction = s.entity(arc).construction;
    return piece;
}

// Replaces a circle by the arc from `from` to `to` (counter-clockwise), keeping its constraints.
Id circleToArc(Sketch& s, Id circle, Id from, Id to)
{
    const Id centre = s.circle(circle).center;
    const Id arc = s.addArc(centre, from, to);
    s.entity(arc).construction = s.entity(circle).construction;
    s.retarget(circle, arc);
    s.remove(circle);
    return arc;
}
}  // namespace

std::vector<Vec2> intersections(const Sketch& s, Id a, Id b)
{
    const Geo ga = geoOf(s, a), gb = geoOf(s, b);
    std::vector<Vec2> out;
    for (const Vec2& p : rawIntersect(ga, gb)) {
        if (ga.contains(p) && gb.contains(p)) {
            out.push_back(p);
        }
    }
    return out;
}

std::vector<Crossing> crossings(const Sketch& s, Vec2 a, Vec2 b)
{
    if (dist(a, b) < kPosEps) {
        return {};
    }
    Geo path;
    path.kind = Geo::Kind::Line;
    path.a = a;
    path.b = b;
    std::vector<std::pair<double, Crossing>> found;
    for (const auto& [id, e] : s.entities()) {
        if (e.kind() == EntityKind::Point || s.isReference(id)) {
            continue;
        }
        const Geo g = geoOf(s, id);
        for (const Vec2& p : rawIntersect(path, g)) {
            if (path.contains(p) && g.contains(p)) {
                found.push_back({path.param(p), Crossing{id, p}});
            }
        }
    }
    std::ranges::sort(found, {}, &std::pair<double, Crossing>::first);
    std::vector<Crossing> out;
    for (const auto& [t, c] : found) {
        out.push_back(c);
    }
    return out;
}

// =============================================================================================
// Trim / break / extend
// =============================================================================================

void trim(Sketch& s, Id curve, Vec2 near)
{
    const Span sp = spanAt(s, curve, near);
    if (sp.whole) {
        s.remove(curve);
        return;
    }
    switch (sp.g.kind) {
        case Geo::Kind::Line: {
            const Line l = s.line(curve);
            if (!sp.lo) {  // the start, up to the first crossing
                s.rebindPoint(curve, l.p1, pointOn(s, sp.hi->at, sp.hi->cutter));
                s.removeIfUnused(l.p1);
            }
            else if (!sp.hi) {  // from the last crossing to the end
                s.rebindPoint(curve, l.p2, pointOn(s, sp.lo->at, sp.lo->cutter));
                s.removeIfUnused(l.p2);
            }
            else {  // a middle piece: split off the tail at the upper crossing, then pull this
                    // line's end back to the lower one (the tail keeps the upper point)
                splitLine(s, curve, *sp.hi);
                s.rebindPoint(curve, s.line(curve).p2, pointOn(s, sp.lo->at, sp.lo->cutter));
            }
            break;
        }
        case Geo::Kind::Arc: {
            const Arc a = s.arc(curve);
            if (!sp.lo) {
                s.rebindPoint(curve, a.start, pointOn(s, sp.hi->at, sp.hi->cutter));
                s.removeIfUnused(a.start);
            }
            else if (!sp.hi) {
                s.rebindPoint(curve, a.end, pointOn(s, sp.lo->at, sp.lo->cutter));
                s.removeIfUnused(a.end);
            }
            else {
                const Id tail = splitArc(s, curve, *sp.hi);
                const Id cutEnd = s.arc(curve).end;  // the point at the upper crossing
                s.rebindPoint(curve, cutEnd, pointOn(s, sp.lo->at, sp.lo->cutter));
                s.addConstraint(CT::Equal, {curve, tail});  // no longer joined: keep one circle
            }
            break;
        }
        case Geo::Kind::Circle: {
            // Keep the rest of the circle: from the upper crossing round to the lower one.
            const Id from = pointOn(s, sp.hi->at, sp.hi->cutter);
            const Id to = pointOn(s, sp.lo->at, sp.lo->cutter);
            circleToArc(s, curve, from, to);
            break;
        }
    }
}

Id trimToConstruction(Sketch& s, Id curve, Vec2 near)
{
    if (spanAt(s, curve, near).whole) {
        s.entity(curve).construction = true;
        return curve;
    }
    // Split at the crossings, then turn the piece that was clicked into construction.
    const std::vector<Id> pieces = breakCurve(s, curve, near);
    const Id clicked = *std::ranges::min_element(pieces, {}, [&](Id p) { return distanceTo(s, p, near); });
    s.entity(clicked).construction = true;
    return clicked;
}

std::vector<Vec2> trimPreview(const Sketch& s, Id curve, Vec2 near)
{
    if (!s.hasEntity(curve) || s.entity(curve).kind() == EntityKind::Point) {
        return {};
    }
    const Span sp = spanAt(s, curve, near);
    if (sp.whole) {
        return sp.g.sample(0, sp.g.end());
    }
    const double from = sp.lo ? sp.lo->param : 0.0;
    double to = sp.hi ? sp.hi->param : sp.g.end();
    if (sp.g.kind == Geo::Kind::Circle && to <= from) {
        to += kTwoPi;
    }
    return sp.g.sample(from, to);
}

std::vector<Id> breakCurve(Sketch& s, Id curve, Vec2 near)
{
    const Span sp = spanAt(s, curve, near);
    if (sp.whole) {
        throw SketchError("break: nothing crosses this curve");
    }
    // Split along the curve in order, each time splitting the remaining piece, so each new
    // constraint ties a new piece to its neighbour (never two to the same one: redundant).
    auto chain = [&](auto split) {
        std::vector<Id> pieces{curve};
        for (const std::optional<Cut>& cut : {sp.lo, sp.hi}) {
            if (cut) {
                pieces.push_back(split(s, pieces.back(), *cut));
            }
        }
        return pieces;
    };
    switch (sp.g.kind) {
        case Geo::Kind::Line: return chain(splitLine);
        case Geo::Kind::Arc: return chain(splitArc);
        case Geo::Kind::Circle: {
            const Id lo = pointOn(s, sp.lo->at, sp.lo->cutter);
            const Id hi = pointOn(s, sp.hi->at, sp.hi->cutter);
            const Id centre = s.circle(curve).center;
            const Id clicked = s.addArc(centre, lo, hi);  // shares centre and both ends with the rest
            s.entity(clicked).construction = s.entity(curve).construction;
            const Id rest = circleToArc(s, curve, hi, lo);
            return {clicked, rest};
        }
    }
    return {};
}

std::vector<Vec2> breakPreview(const Sketch& s, Id curve, Vec2 near)
{
    if (!s.hasEntity(curve) || s.entity(curve).kind() == EntityKind::Point) {
        return {};
    }
    const Span sp = spanAt(s, curve, near);
    return sp.whole ? std::vector<Vec2>{} : trimPreview(s, curve, near);
}

namespace
{
struct Extension
{
    Id end = kNoId;  // the point being moved
    Cut target;
    Geo g;
    double from = 0, to = 0;  // parameter range of the extension (for previews)
};

std::optional<Extension> findExtension(const Sketch& s, Id curve, Vec2 near)
{
    const Entity& e = s.entity(curve);
    if (e.kind() != EntityKind::Line && e.kind() != EntityKind::Arc) {
        return std::nullopt;
    }
    Geo g = geoOf(s, curve);
    Geo full = g;  // infinite line / full circle
    full.kind = g.kind == Geo::Kind::Line ? Geo::Kind::Line : Geo::Kind::Circle;
    const double u = g.param(near);
    const bool atEnd = g.kind == Geo::Kind::Line ? u >= 0.5 : u >= g.sweep / 2;

    std::optional<Extension> best;
    for (const auto& [id, other] : s.entities()) {
        if (!isCutter(s, id, curve)) {
            continue;
        }
        const Geo og = geoOf(s, id);
        for (const Vec2& p : rawIntersect(full, og)) {
            if (!og.contains(p)) {
                continue;
            }
            double t = g.param(p);
            bool ahead = false;
            double distance = 0;  // how far the end moves, in parameter terms
            if (g.kind == Geo::Kind::Line) {
                const double slack = kParamEps;
                ahead = atEnd ? t > 1 + slack : t < -slack;
                distance = atEnd ? t - 1 : -t;
            }
            else {
                ahead = t > g.sweep + kParamEps && t < kTwoPi - kParamEps;
                distance = atEnd ? t - g.sweep : kTwoPi - t;
            }
            if (ahead && (!best || distance < best->target.param)) {
                Extension x;
                x.target = {distance, id, p};
                x.g = g;
                if (g.kind == Geo::Kind::Line) {
                    x.end = atEnd ? s.line(curve).p2 : s.line(curve).p1;
                    x.from = atEnd ? 1.0 : t;
                    x.to = atEnd ? t : 0.0;
                }
                else {
                    x.end = atEnd ? s.arc(curve).end : s.arc(curve).start;
                    x.from = atEnd ? g.sweep : t - kTwoPi;
                    x.to = atEnd ? t : 0.0;
                }
                best = x;
            }
        }
    }
    return best;
}
}  // namespace

void extend(Sketch& s, Id curve, Vec2 near)
{
    const auto x = findExtension(s, curve, near);
    if (!x) {
        throw SketchError("extend: nothing to extend to");
    }
    s.rebindPoint(curve, x->end, pointOn(s, x->target.at, x->target.cutter));
    s.removeIfUnused(x->end);
}

std::vector<Vec2> extendPreview(const Sketch& s, Id curve, Vec2 near)
{
    if (!s.hasEntity(curve)) {
        return {};
    }
    const auto x = findExtension(s, curve, near);
    return x ? x->g.sample(x->from, x->to, 16) : std::vector<Vec2>{};
}

// =============================================================================================
// Fillet / chamfer
// =============================================================================================

namespace
{
// The line/circle at distance r from a curve's underlying line/circle, on each side.
std::vector<Geo> offsets(const Geo& g, double r)
{
    std::vector<Geo> out;
    if (g.kind == Geo::Kind::Line) {
        const Vec2 n = mul(rot90(unit(sub(g.b, g.a))), r);
        for (double sign : {1.0, -1.0}) {
            Geo o = g;
            o.a = add(g.a, mul(n, sign));
            o.b = add(g.b, mul(n, sign));
            out.push_back(o);
        }
        return out;
    }
    for (double rr : {g.r + r, g.r - r}) {
        if (rr > kPosEps) {
            Geo o = g;
            o.kind = Geo::Kind::Circle;
            o.r = rr;
            out.push_back(o);
        }
    }
    return out;
}

// Where a circle centred at `c` touches the curve's underlying line/circle.
Vec2 touch(const Geo& g, Vec2 c)
{
    if (g.kind == Geo::Kind::Line) {
        const Vec2 d = unit(sub(g.b, g.a));
        return add(g.a, mul(d, dot(sub(c, g.a), d)));
    }
    return add(g.c, mul(unit(sub(c, g.c)), g.r));
}

void requireCornerCurves(const Sketch& s, Id a, Id b, bool linesOnly, const char* what)
{
    for (Id id : {a, b}) {
        const EntityKind k = s.entity(id).kind();
        const bool ok = k == EntityKind::Line || (!linesOnly && k == EntityKind::Arc);
        if (!ok || s.isReference(id)) {
            throw SketchError(std::string(what) + (linesOnly ? ": pick two lines" : ": pick two lines or arcs"));
        }
    }
    if (a == b) {
        throw SketchError(std::string(what) + ": pick two different curves");
    }
}

// The new solid piece of `curve` from the point `t` (where the fillet/chamfer meets it) along
// the curve in the direction `away` (away from the corner), and the point at `t`. Throws if
// the curve doesn't reach past `t` that way, i.e. the fillet/chamfer is too big for it. The
// original curve is left alone (the caller makes it construction).
std::pair<Id, Id> keptPiece(Sketch& s, Id curve, Vec2 t, Vec2 away, const char* what)
{
    const Geo g = geoOf(s, curve);
    if (g.kind == Geo::Kind::Line) {
        const Line l = s.line(curve);
        Vec2 dir = unit(sub(g.b, g.a));
        if (dot(dir, away) < 0) {
            dir = mul(dir, -1);
        }
        const Id keep = dot(sub(pos(s, l.p2), t), dir) >= dot(sub(pos(s, l.p1), t), dir) ? l.p2 : l.p1;
        if (dot(sub(pos(s, keep), t), dir) <= 10 * kPosEps) {
            throw SketchError(std::string(what) + ": too big for these lines");
        }
        const Id tp = s.addPoint(t.x, t.y);
        s.addConstraint(CT::Coincident, {tp, curve});  // stays on the original line
        return {s.addLine(keep, tp), tp};
    }
    // Arcs run counter-clockwise; carry on that way if that heads away from the corner.
    const Arc a = s.arc(curve);
    const double uT = g.param(t);
    const bool ccw = dot(rot90(sub(t, g.c)), away) > 0;
    const Id tp = s.addPoint(t.x, t.y);
    // The piece shares the original's centre and far end, so it stays on the same circle.
    if (ccw && uT < g.sweep - kParamEps) {
        return {s.addArc(a.center, tp, a.end), tp};
    }
    if (!ccw && uT > kParamEps && uT <= g.sweep + kParamEps) {
        return {s.addArc(a.center, a.start, tp), tp};
    }
    throw SketchError(std::string(what) + ": too big for this arc");
}
}  // namespace

Corner fillet(Sketch& s, Id a, Vec2 nearA, Id b, Vec2 nearB, double radius)
{
    requireCornerCurves(s, a, b, false, "fillet");
    if (!(radius > kPosEps)) {
        throw SketchError("fillet: the radius must be positive");
    }
    const Geo ga = geoOf(s, a), gb = geoOf(s, b);

    // Centres at distance `radius` from both curves; take the one whose touching points are
    // nearest where the curves were picked (i.e. in the corner meant).
    std::optional<Vec2> centre;
    double bestScore = 0;
    for (const Geo& oa : offsets(ga, radius)) {
        for (const Geo& ob : offsets(gb, radius)) {
            for (const Vec2& c : rawIntersect(oa, ob)) {
                const double score = dist(touch(ga, c), nearA) + dist(touch(gb, c), nearB);
                if (!centre || score < bestScore) {
                    centre = c;
                    bestScore = score;
                }
            }
        }
    }
    if (!centre) {
        throw SketchError("fillet: these curves can't be joined by an arc of that radius");
    }

    // Each curve is kept leading away from the arc (away from the other touching point).
    Corner out;
    const Vec2 touchA = touch(ga, *centre), touchB = touch(gb, *centre);
    const auto [pieceA, ta] = keptPiece(s, a, touchA, sub(touchA, touchB), "fillet");
    const auto [pieceB, tb] = keptPiece(s, b, touchB, sub(touchB, touchA), "fillet");
    s.entity(a).construction = true;
    s.entity(b).construction = true;
    out.pieces = {pieceA, pieceB};

    // The short way round between the touching points.
    const Id c = s.addPoint(centre->x, centre->y);
    const Vec2 pa = pos(s, ta), pb = pos(s, tb);
    const double sweep = angleFrom(std::atan2(pa.y - centre->y, pa.x - centre->x), std::atan2(pb.y - centre->y, pb.x - centre->x));
    out.joint = sweep <= kPi ? s.addArc(c, ta, tb) : s.addArc(c, tb, ta);
    s.addConstraint(CT::Tangent, {pieceA, out.joint});
    s.addConstraint(CT::Tangent, {pieceB, out.joint});
    out.dimensions.push_back(s.addConstraint(CT::Radius, {out.joint}, radius));
    return out;
}

Corner chamfer(Sketch& s, Id a, Vec2 nearA, Id b, Vec2 nearB, ChamferKind kind, double value1, double value2)
{
    requireCornerCurves(s, a, b, true, "chamfer");
    const Geo ga = geoOf(s, a), gb = geoOf(s, b);
    const auto xs = rawIntersect(ga, gb);
    if (xs.empty()) {
        throw SketchError("chamfer: the lines are parallel");
    }
    const Vec2 x = xs.front();
    Vec2 u1 = unit(sub(ga.b, ga.a)), u2 = unit(sub(gb.b, gb.a));
    if (dot(sub(nearA, x), u1) < 0) {
        u1 = mul(u1, -1);
    }
    if (dot(sub(nearB, x), u2) < 0) {
        u2 = mul(u2, -1);
    }
    const double theta = std::acos(std::clamp(dot(u1, u2), -1.0, 1.0));

    const double d1 = value1;
    double d2 = value1;
    if (kind == ChamferKind::TwoDistances) {
        d2 = value2;
    }
    else if (kind == ChamferKind::DistanceAngle) {
        const double alpha = value2 * kPi / 180.0;
        const double other = kPi - theta - alpha;  // angle at the far end of the chamfer
        if (!(alpha > 0) || !(other > 1e-6)) {
            throw SketchError("chamfer: that angle doesn't fit this corner");
        }
        d2 = d1 * std::sin(alpha) / std::sin(other);
    }
    if (!(d1 > kPosEps) || !(d2 > kPosEps)) {
        throw SketchError("chamfer: distances must be positive");
    }

    // The corner point: the lines' shared end if they meet there, otherwise a new point kept
    // on both lines.
    Id corner = kNoId;
    for (Id p : s.definingPoints(a)) {
        const auto pb = s.definingPoints(b);
        if (std::ranges::find(pb, p) != pb.end() && dist(pos(s, p), x) < 10 * kPosEps) {
            corner = p;
        }
    }
    if (corner == kNoId) {
        corner = s.addPoint(x.x, x.y);
        s.addConstraint(CT::Coincident, {corner, a});
        s.addConstraint(CT::Coincident, {corner, b});
    }

    Corner out;
    const auto [pieceA, c1] = keptPiece(s, a, add(x, mul(u1, d1)), u1, "chamfer");
    const auto [pieceB, c2] = keptPiece(s, b, add(x, mul(u2, d2)), u2, "chamfer");
    s.entity(a).construction = true;
    s.entity(b).construction = true;
    out.pieces = {pieceA, pieceB};
    out.joint = s.addLine(c1, c2);

    out.dimensions.push_back(s.addConstraint(CT::Distance, {corner, c1}, d1));
    if (kind == ChamferKind::DistanceAngle) {
        // The angle inside the cut-off triangle, between the remaining side and the chamfer.
        const Vec2 inside = mul(add(add(x, pos(s, c1)), pos(s, c2)), 1.0 / 3.0);
        const Id angle = s.addConstraint(CT::Angle, {pieceA, out.joint}, value2);
        s.setConstraintFlip(angle, angleFlipAt(s, pieceA, out.joint, inside));
        out.dimensions.push_back(angle);
    }
    else {
        out.dimensions.push_back(s.addConstraint(CT::Distance, {corner, c2}, d2));
    }
    return out;
}

bool cornerAt(const Sketch& s, Id point, Id& a, Vec2& nearA, Id& b, Vec2& nearB)
{
    std::vector<std::pair<Id, Vec2>> found;  // curve, its other end
    for (const auto& [id, e] : s.entities()) {
        if (e.construction || s.isReference(id)) {
            continue;
        }
        if (const auto* l = std::get_if<Line>(&e.geom)) {
            if (l->p1 == point || l->p2 == point) {
                found.push_back({id, pos(s, l->p1 == point ? l->p2 : l->p1)});
            }
        }
        else if (const auto* arc = std::get_if<Arc>(&e.geom)) {
            if (arc->start == point || arc->end == point) {
                // A point along the arc, away from the corner (its far end can be a long way
                // round; the middle is a safe "this side").
                const ArcShape sh = arcShape(s, id);
                const double mid = sh.startAngle + sh.sweep / 2;
                found.push_back({id, {sh.center.x + sh.radius * std::cos(mid), sh.center.y + sh.radius * std::sin(mid)}});
            }
        }
    }
    if (found.size() != 2) {
        return false;
    }
    a = found[0].first;
    nearA = found[0].second;
    b = found[1].first;
    nearB = found[1].second;
    return true;
}

}  // namespace core::modify
