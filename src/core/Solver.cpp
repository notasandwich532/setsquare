#include "Solver.h"

#include "Curves.h"
#include "Features.h"

#include <planegcs/GCS.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <functional>
#include <stdexcept>
#include <vector>
#include <map>
#include <set>
#include <numbers>

namespace core
{

namespace
{
constexpr int kInternalTag = 0;  // tag 0: high priority, never reported as conflicting
constexpr double kPi = std::numbers::pi;

// Signed "area" of p relative to the directed line a->b; >= 0 means p is on the left (ccw).
bool isLeftOf(const GCS::Point& p, const GCS::Line& l)
{
    const double dx = *l.p2.x - *l.p1.x;
    const double dy = *l.p2.y - *l.p1.y;
    return (-*p.x * dy + *p.y * dx + *l.p1.x * *l.p2.y - *l.p2.x * *l.p1.y) >= 0.0;
}

double lineAngle(const GCS::Line& l)
{
    return std::atan2(*l.p2.y - *l.p1.y, *l.p2.x - *l.p1.x);
}

double wrapPi(double a)
{
    while (a > kPi) {
        a -= 2 * kPi;
    }
    while (a <= -kPi) {
        a += 2 * kPi;
    }
    return a;
}

// One coordinate of "q is p rotated about c by a fixed angle" (circular patterns). Linear in
// all six coordinates.
class ConstraintRotated : public GCS::Constraint
{
public:
    ConstraintRotated(GCS::Point& p, GCS::Point& q, GCS::Point& c, double angle, int component)
        : m_cos(std::cos(angle))
        , m_sin(std::sin(angle))
        , m_component(component)
    {
        pvec = {p.x, p.y, q.x, q.y, c.x, c.y};
        origpvec = pvec;
        rescale();
    }

    double error() override
    {
        const double dx = *pvec[0] - *pvec[4], dy = *pvec[1] - *pvec[5];
        if (m_component == 0) {
            return scale * (*pvec[2] - *pvec[4] - (m_cos * dx - m_sin * dy));
        }
        return scale * (*pvec[3] - *pvec[5] - (m_sin * dx + m_cos * dy));
    }

    double grad(double* param) override
    {
        // d(error)/d(px, py, qx, qy, cx, cy)
        const double x[6] = {-m_cos, m_sin, 1.0, 0.0, m_cos - 1.0, -m_sin};
        const double y[6] = {-m_sin, -m_cos, 0.0, 1.0, m_sin, m_cos - 1.0};
        const double* coef = m_component == 0 ? x : y;
        double deriv = 0.0;
        for (size_t i = 0; i < 6; ++i) {
            if (pvec[i] == param) {
                deriv += coef[i];
            }
        }
        return scale * deriv;
    }

private:
    double m_cos, m_sin;
    int m_component;
};

// "Copy arc end is in the same direction from its centre as the source arc's, turned by a fixed
// angle": cross(R(ps - cs), pq - cq) = 0, scaled to a length. Patterns use it for arc ends, which
// only have one degree of freedom left once the arc's centre and start are tied; unlike a single
// coordinate, it is well-conditioned wherever the end is.
class ConstraintSameDirection : public GCS::Constraint
{
public:
    ConstraintSameDirection(GCS::Point& ps, GCS::Point& cs, GCS::Point& pq, GCS::Point& cq, double angle)
        : m_cos(std::cos(angle))
        , m_sin(std::sin(angle))
    {
        pvec = {ps.x, ps.y, cs.x, cs.y, pq.x, pq.y, cq.x, cq.y};
        origpvec = pvec;
        const double r = std::hypot(*ps.x - *cs.x, *ps.y - *cs.y);
        m_norm = r > 0.0 ? 1.0 / r : 1.0;
        rescale();
    }

    double error() override
    {
        const Terms t = terms();
        return scale * m_norm * (t.wx * t.vy - t.wy * t.vx);
    }

    double grad(double* param) override
    {
        const Terms t = terms();
        const double dux = m_cos * t.vy - m_sin * t.vx;
        const double duy = -m_sin * t.vy - m_cos * t.vx;
        const double coef[8] = {dux, duy, -dux, -duy, -t.wy, t.wx, t.wy, -t.wx};
        double deriv = 0.0;
        for (size_t i = 0; i < 8; ++i) {
            if (pvec[i] == param) {
                deriv += coef[i];
            }
        }
        return scale * m_norm * deriv;
    }

private:
    struct Terms
    {
        double wx, wy, vx, vy;  // w = R(ps - cs), v = pq - cq
    };
    Terms terms() const
    {
        const double ux = *pvec[0] - *pvec[2], uy = *pvec[1] - *pvec[3];
        return {m_cos * ux - m_sin * uy, m_sin * ux + m_cos * uy, *pvec[4] - *pvec[6], *pvec[5] - *pvec[7]};
    }

    double m_cos, m_sin;
    double m_norm = 1.0;
};

// One coordinate of "q is the fraction t of the way from a to b" (dividing a line). Linear.
class ConstraintAlong : public GCS::Constraint
{
public:
    ConstraintAlong(GCS::Point& a, GCS::Point& b, GCS::Point& q, double t, int component)
        : m_t(t)
    {
        pvec = component == 0 ? GCS::VEC_pD{a.x, b.x, q.x} : GCS::VEC_pD{a.y, b.y, q.y};
        origpvec = pvec;
        rescale();
    }

    double error() override { return scale * (*pvec[2] - (1.0 - m_t) * *pvec[0] - m_t * *pvec[1]); }

    double grad(double* param) override
    {
        const double coef[3] = {-(1.0 - m_t), -m_t, 1.0};
        double deriv = 0.0;
        for (size_t i = 0; i < 3; ++i) {
            if (pvec[i] == param) {
                deriv += coef[i];
            }
        }
        return scale * deriv;
    }

private:
    double m_t;
};

// "p is k times q", for radii. (Not PlaneGCS's proportional constraint: its system merges the two
// parameters of any "equal" constraint into one before solving, losing the ratio.)
class ConstraintScaled : public GCS::Constraint
{
public:
    ConstraintScaled(double* p, double* q, double k)
        : m_k(k)
    {
        pvec = {p, q};
        origpvec = pvec;
        rescale();
    }

    double error() override { return scale * (*pvec[0] - m_k * *pvec[1]); }

    double grad(double* param) override
    {
        double deriv = 0.0;
        if (param == pvec[0]) {
            deriv += 1.0;
        }
        if (param == pvec[1]) {
            deriv -= m_k;
        }
        return scale * deriv;
    }

private:
    double m_k;
};

// "Line 1 is k times as long as line 2": |b1 - a1| - k |b2 - a2| = 0.
class ConstraintLengthRatio : public GCS::Constraint
{
public:
    ConstraintLengthRatio(GCS::Line& l1, GCS::Line& l2, double k)
        : m_k(k)
    {
        pvec = {l1.p1.x, l1.p1.y, l1.p2.x, l1.p2.y, l2.p1.x, l2.p1.y, l2.p2.x, l2.p2.y};
        origpvec = pvec;
        rescale();
    }

    double error() override { return scale * (length(0) - m_k * length(4)); }

    double grad(double* param) override
    {
        double deriv = 0.0;
        for (size_t first : {size_t{0}, size_t{4}}) {
            const double dx = *pvec[first + 2] - *pvec[first], dy = *pvec[first + 3] - *pvec[first + 1];
            const double len = std::hypot(dx, dy);
            if (len <= 0.0) {
                continue;
            }
            const double sign = first == 0 ? 1.0 : -m_k;
            const double coef[4] = {-dx / len, -dy / len, dx / len, dy / len};
            for (size_t i = 0; i < 4; ++i) {
                if (pvec[first + i] == param) {
                    deriv += sign * coef[i];
                }
            }
        }
        return scale * deriv;
    }

private:
    double length(size_t first) const
    {
        return std::hypot(*pvec[first + 2] - *pvec[first], *pvec[first + 3] - *pvec[first + 1]);
    }

    double m_k;
};

// An equation given as a function of some parameters, with its gradient found numerically: for
// the curves PlaneGCS doesn't have (ellipses, splines), whose shapes are built from their points.
// A parameter may appear more than once (a point two curves share): it's moved everywhere at once.
class ConstraintNumeric : public GCS::Constraint
{
public:
    using Fn = std::function<double(const std::vector<double>&)>;
    ConstraintNumeric(GCS::VEC_pD params, Fn fn)
        : m_fn(std::move(fn))
    {
        pvec = std::move(params);
        origpvec = pvec;
        rescale();
    }

    double error() override { return scale * m_fn(values()); }

    double grad(double* param) override
    {
        std::vector<double> v = values();
        std::vector<size_t> at;
        for (size_t i = 0; i < pvec.size(); ++i) {
            if (pvec[i] == param) {
                at.push_back(i);
            }
        }
        if (at.empty()) {
            return 0.0;
        }
        const double h = 1e-7 * std::max(1.0, std::abs(v[at.front()]));
        auto shifted = [&](double by) {
            for (size_t i : at) {
                v[i] = *pvec[i] + by;
            }
            return m_fn(v);
        };
        const double up = shifted(h), down = shifted(-h);
        return scale * (up - down) / (2 * h);
    }

private:
    std::vector<double> values() const
    {
        std::vector<double> v(pvec.size());
        for (size_t i = 0; i < pvec.size(); ++i) {
            v[i] = *pvec[i];
        }
        return v;
    }
    Fn m_fn;
};

double cross(Vec2 a, Vec2 b) { return a.x * b.y - a.y * b.x; }
double dot(Vec2 a, Vec2 b) { return a.x * b.x + a.y * b.y; }
Vec2 unitOf(Vec2 a)
{
    const double n = std::hypot(a.x, a.y);
    return n > 0 ? Vec2{a.x / n, a.y / n} : Vec2{1, 0};
}

Vec2 operator-(Vec2 a, Vec2 b)
{
    return {a.x - b.x, a.y - b.y};
}

// An ellipse from the values of its centre, major and minor points (six numbers from `v`).
curve::EllipseFrame frameFrom(const double* v)
{
    curve::EllipseFrame f;
    f.c = {v[0], v[1]};
    const Vec2 major{v[2] - v[0], v[3] - v[1]};
    f.a = std::hypot(major.x, major.y);
    f.u = unitOf(major);
    f.b = std::hypot(v[4] - v[0], v[5] - v[1]);
    return f;
}
}  // namespace

struct Solver::Impl
{
    // All solver parameters live in one contiguous block, reserved up front and never
    // reallocated (GCS keeps the pointers). Contiguity matters for determinism, not just
    // validity: PlaneGCS orders parameters by *address* (std::set<double*>), and with an
    // under-constrained sketch that order decides which of the possible solutions comes out.
    // With one block, address order is creation order, the same on every run; a std::deque's
    // blocks can land in any order and made results vary between runs.
    std::vector<double> store;
    double* alloc(double v)
    {
        if (store.size() == store.capacity()) {
            throw std::logic_error("solver parameter block too small");  // would move live pointers
        }
        store.push_back(v);
        return &store.back();
    }

    // std::map nodes are stable too; GCS keeps references/pointers into these.
    std::map<Id, GCS::Point> points;
    std::map<Id, GCS::Line> lines;
    std::map<Id, GCS::Circle> circles;
    std::map<Id, GCS::Arc> arcs;

    GCS::VEC_pD unknowns;
    std::map<Id, std::vector<double*>> entityUnknowns;  // per entity, the unknowns that define it
    std::set<Id> fixedEntities;
    std::set<Id> capArcs;  // see addOffset
    // Per centre: the ends of arcs on it, joined where arcs join them (union-find). Ends joined
    // this way are on one circle: see build.
    std::map<Id, std::map<Id, Id>> arcEnds;
    Id arcRoot(Id centre, Id p)
    {
        auto c = arcEnds.find(centre);
        if (c == arcEnds.end() || !c->second.contains(p)) {
            return kNoId;
        }
        auto& ends = c->second;
        while (ends[p] != p) {
            p = ends[p] = ends[ends[p]];
        }
        return p;
    }
    // Arcs on one circle: same centre, and joined end to end or by equal radii (union-find over
    // arc ids, extended as equal constraints are added).
    std::map<Id, Id> circleOf;
    Id circleRoot(Id arc)
    {
        if (!circleOf.contains(arc)) {
            circleOf[arc] = arc;
        }
        while (circleOf[arc] != arc) {
            arc = circleOf[arc] = circleOf[circleOf[arc]];
        }
        return arc;
    }
    std::map<std::pair<Id, Id>, Id> chainArc;  // (centre, end chain) -> an arc in it
    // Whether a point is on a spline by its making: one it passes through, or its own end.
    static bool splineEndOnCurve(const Spline& sp, Id p)
    {
        return (sp.kind == SplineKind::Fit && std::ranges::find(sp.points, p) != sp.points.end())
            || (!sp.closed && (p == sp.points.front() || p == sp.points.back()));
    }

    // A curve as the solver has it: the parameters its shape depends on, and how to build its
    // path from their values (in that order). Splines are always the whole curve.
    struct CurveModel
    {
        GCS::VEC_pD params;
        std::function<curve::Path(const double*)> build;
    };
    CurveModel model(const Sketch& sketch, Id id);
    // The same, but a spline piece's own path (from one end to the other), not the whole curve's.
    CurveModel pathModel(const Sketch& sketch, Id id);
    // Point p kept on a curve with no PlaneGCS counterpart (ellipse, elliptical arc, spline).
    // Once per point and underlying curve (an ellipse's three points, a spline's): pieces of one
    // sharing an end would otherwise say it twice. Returns false if it was there already.
    bool addPointOnCurve(const Sketch& sketch, Id p, Id curve, int tag);
    std::set<std::vector<Id>> onCurves;  // (the curve's defining points..., kind marker, p)
    static std::vector<Id> onCurveKey(const Sketch& sketch, Id p, Id curve)
    {
        std::vector<Id> key;
        if (sketch.isElliptic(curve)) {
            const std::array<Id, 3> ax = sketch.ellipseAxes(curve);
            key.assign(ax.begin(), ax.end());
            key.push_back(0);
        }
        else {
            const Spline& sp = sketch.spline(curve);
            key = sp.points;
            key.push_back(sp.kind == SplineKind::Fit ? 1 : 2);
            key.push_back(sp.closed ? 1 : 0);
        }
        key.push_back(p);
        return key;
    }
    // Two curves (one at least an ellipse or a spline) touching: where they share an end
    // (`joint`), their directions there agree; otherwise at a contact point found with them.
    void addTangentCurves(const Sketch& sketch, Id a, Id b, Id joint, int tag);

    // A constraint the arcs say already (left out, so it isn't reported as redundant; it still
    // applies once they don't, e.g. when the arc joining them is deleted).
    bool impliedByArcs(const Sketch& sketch, const Constraint& c);

    // Lines made collinear (pieces of a trimmed line, joined by collinear constraints) lie on one
    // line: per group (union-find over line ids), the points known to be on it. Constraints are
    // added in id order, and each consults and extends these, so what's already known on the
    // line isn't said again (which is redundant, and leaves drags unable to converge).
    std::map<Id, Id> lineGroup;
    std::map<Id, std::set<Id>> onLine;  // group root -> points on that line
    Id groupOf(const Sketch& sketch, Id line)
    {
        if (!lineGroup.contains(line)) {
            lineGroup[line] = line;
            const Line& l = sketch.line(line);
            onLine[line] = {l.p1, l.p2};
        }
        while (lineGroup[line] != line) {
            line = lineGroup[line] = lineGroup[lineGroup[line]];
        }
        return line;
    }

    GCS::System sys;
    bool dragging = false;
    struct DragTarget
    {
        double startX, startY;
        double* x;
        double* y;
    };
    std::vector<DragTarget> dragTargets;

    GCS::Circle& round(Id id)
    {
        if (auto it = circles.find(id); it != circles.end()) {
            return it->second;
        }
        return arcs.at(id);
    }

    GCS::Curve& curve(Id id)
    {
        if (auto it = lines.find(id); it != lines.end()) {
            return it->second;
        }
        return round(id);
    }

    // A point that is an end of both curves (line ends, arc start/end), if any.
    static Id sharedEndpoint(const Sketch& s, Id a, Id b)
    {
        auto ends = [&](Id id) -> std::vector<Id> {
            const Entity& e = s.entity(id);
            if (const auto* l = std::get_if<Line>(&e.geom)) {
                return {l->p1, l->p2};
            }
            if (const auto* arc = std::get_if<Arc>(&e.geom)) {
                return {arc->start, arc->end};
            }
            if (const auto* ea = std::get_if<EllipseArc>(&e.geom)) {
                return {ea->start, ea->end};
            }
            if (const auto* sp = std::get_if<Spline>(&e.geom)) {
                if (sp->isPiece()) {
                    return {sp->start, sp->end};
                }
                if (!sp->closed) {
                    return {sp->points.front(), sp->points.back()};
                }
            }
            return {};
        };
        for (Id p : ends(a)) {
            for (Id q : ends(b)) {
                if (p == q) {
                    return p;
                }
            }
        }
        return kNoId;
    }

    double* radiusOf(Id id) { return round(id).rad; }
    static Vec2 pos(const GCS::Point& p) { return {*p.x, *p.y}; }

    void build(const Sketch& sketch);
    void addConstraint(const Sketch& sketch, const Constraint& c);
    void addPattern(const Sketch& sketch, const Constraint& c);
    void addOffset(const Sketch& sketch, const Constraint& c);
    void addOffsetSide(const Sketch& sketch, const Constraint& c, const std::vector<Id>& sources,
                       const std::vector<Id>& offsets);
};

void Solver::Impl::build(const Sketch& sketch)
{
    // Rank detection must be no stricter than the solver's own precision (it converges to about
    // 1e-10). With PlaneGCS's default of 1e-13, a constraint that is redundant in exact
    // arithmetic (e.g. "horizontal" on a regular pentagon's diagonal once a side is horizontal)
    // looks independent because of leftover solver noise, and the sketch is wrongly reported as
    // fully constrained.
    sys.qrpivotThreshold = 1e-9;

    // Enough for every parameter: 2 per point, 3 per arc, 1 per circle, at most 2 values per
    // constraint, plus drag targets (2 per point, and a held centre).
    size_t pointCount = 0, roundCount = 0;
    for (const auto& [id, e] : sketch.entities()) {
        pointCount += e.kind() == EntityKind::Point;
        roundCount += e.kind() == EntityKind::Circle || e.kind() == EntityKind::Arc;
    }
    // Features allocate per linked point: a few values for each entity they reference.
    size_t featureValues = 0;
    for (const auto& [id, c] : sketch.constraints()) {
        if (isFeature(c.type)) {
            featureValues += 8 * c.refs.size();
        }
    }
    // (+ the contact parameters of tangents and points on splines, and spline pieces' ends)
    store.reserve(4 * pointCount + 3 * roundCount + 3 * sketch.constraints().size() + 2 * sketch.entities().size()
                  + featureValues + 16);

    // Arcs capping a both-sides offset: their ends are pinned by the two sides already.
    for (const auto& [id, c] : sketch.constraints()) {
        if (c.type == ConstraintType::Offset) {
            for (Id cap : feature::offsetRefs(c).caps) {
                capArcs.insert(cap);
            }
        }
    }

    // Points that may not move: fixed points, and the defining points of fixed curves; and the
    // points along ellipses' and splines' offsets, which are placed from their sources.
    std::set<Id> fixedPoints = feature::placedOffsetPoints(sketch);
    for (const auto& [id, e] : sketch.entities()) {
        if (e.fixed) {
            fixedEntities.insert(id);
            for (Id p : sketch.definingPoints(id)) {
                fixedPoints.insert(p);
            }
        }
    }

    std::set<double*> unknownSet;
    auto declare = [&](double* p) {
        unknowns.push_back(p);
        unknownSet.insert(p);
    };

    for (const auto& [id, e] : sketch.entities()) {
        if (const auto* pt = std::get_if<Point>(&e.geom)) {
            GCS::Point gp(alloc(pt->x), alloc(pt->y));
            points.emplace(id, gp);
            if (!fixedPoints.contains(id)) {
                declare(gp.x);
                declare(gp.y);
            }
        }
    }

    for (const auto& [id, e] : sketch.entities()) {
        const bool fixed = e.fixed;
        if (const auto* l = std::get_if<Line>(&e.geom)) {
            GCS::Line gl;
            gl.p1 = points.at(l->p1);
            gl.p2 = points.at(l->p2);
            lines.emplace(id, gl);
        }
        else if (const auto* c = std::get_if<Circle>(&e.geom)) {
            GCS::Circle gc;
            gc.center = points.at(c->center);
            gc.rad = alloc(c->radius);
            if (!fixed) {
                declare(gc.rad);
            }
            circles.emplace(id, gc);
        }
        else if (const auto* a = std::get_if<Arc>(&e.geom)) {
            GCS::Arc ga;
            ga.center = points.at(a->center);
            ga.start = points.at(a->start);
            ga.end = points.at(a->end);
            const double cx = *ga.center.x, cy = *ga.center.y;
            const double sx = *ga.start.x - cx, sy = *ga.start.y - cy;
            const double ex = *ga.end.x - cx, ey = *ga.end.y - cy;
            double startAngle = std::atan2(sy, sx);
            double endAngle = std::atan2(ey, ex);
            if (endAngle <= startAngle) {
                endAngle += 2 * kPi;
            }
            ga.rad = alloc(std::hypot(sx, sy));
            ga.startAngle = alloc(startAngle);
            ga.endAngle = alloc(endAngle);
            if (!fixed) {
                declare(ga.rad);
                declare(ga.startAngle);
                declare(ga.endAngle);
            }
            auto [it, _] = arcs.emplace(id, ga);
            // Arcs on one centre chained end to end (a circle trimmed to construction, maybe
            // several times) go round in a loop: the last one's ends are already on the circle
            // through the others, so the usual arc rules would say that twice, which leaves drags
            // unable to converge. Like an offset's caps, an arc closing a loop takes its radius
            // and start angle from its start, and only the angle from its end.
            auto& ends = arcEnds[a->center];  // which ends are joined round this centre
            auto root = [&](Id p) {
                if (!ends.contains(p)) {
                    ends[p] = p;
                }
                while (ends[p] != p) {
                    p = ends[p] = ends[ends[p]];
                }
                return p;
            };
            const Id rootStart = root(a->start), rootEnd = root(a->end);
            if (capArcs.contains(id)) {
                // (caps get theirs from their offset: see addOffset)
            }
            else if (rootStart == rootEnd) {
                GCS::Arc& twin = it->second;
                sys.addConstraintP2PDistance(twin.center, twin.start, twin.rad, kInternalTag);
                sys.addConstraintP2PAngle(twin.center, twin.start, twin.startAngle, 0.0, kInternalTag);
                sys.addConstraintP2PAngle(twin.center, twin.end, twin.endAngle, 0.0, kInternalTag);
            }
            else {
                sys.addConstraintArcRules(it->second, kInternalTag);
                ends[rootStart] = rootEnd;
            }
        }
    }

    // Ellipses keep their axes at right angles (once per ellipse: pieces of one share its points),
    // except copies whose three points are all placed by a pattern or a mirror (of an ellipse that
    // has the rule): for them it would be said twice. Elliptical arcs' and spline pieces' ends stay
    // on their curves.
    std::set<Id> placedCopies;
    std::map<Id, std::pair<Id, Id>> mirrorOf;  // point -> (its mirror image, the mirror line)
    for (const auto& [cid, c] : sketch.constraints()) {
        if (c.type == ConstraintType::LinearPattern || c.type == ConstraintType::CircularPattern) {
            for (const auto& copies : feature::pattern(c).copies) {
                for (Id copy : copies) {
                    if (sketch.hasEntity(copy) && sketch.isElliptic(copy)) {
                        placedCopies.insert(copy);
                    }
                }
            }
        }
        if (c.type == ConstraintType::Symmetric && c.refs.size() == 3) {
            mirrorOf[c.refs[0]] = {c.refs[1], c.refs[2]};
            mirrorOf[c.refs[1]] = {c.refs[0], c.refs[2]};
        }
    }
    std::set<std::array<Id, 3>> rightAngled;
    for (const auto& [id, e] : sketch.entities()) {
        if (!sketch.isElliptic(id)) {
            continue;
        }
        const std::array<Id, 3> ax = sketch.ellipseAxes(id);
        bool mirrored = mirrorOf.contains(ax[0]) && mirrorOf.contains(ax[1]) && mirrorOf.contains(ax[2]);
        if (mirrored) {
            const Id line = mirrorOf.at(ax[0]).second;
            const std::array<Id, 3> image{mirrorOf.at(ax[0]).first, mirrorOf.at(ax[1]).first, mirrorOf.at(ax[2]).first};
            mirrored = mirrorOf.at(ax[1]).second == line && mirrorOf.at(ax[2]).second == line && rightAngled.contains(image);
        }
        if (placedCopies.contains(id) || mirrored || !rightAngled.insert(ax).second) {
            continue;
        }
        sys.addConstraintPerpendicular(points.at(ax[0]), points.at(ax[1]), points.at(ax[0]), points.at(ax[2]),
                                       kInternalTag);
    }
    for (const auto& [id, e] : sketch.entities()) {
        if (const auto* ea = std::get_if<EllipseArc>(&e.geom)) {
            for (Id end : {ea->start, ea->end}) {
                if (end != ea->major && end != ea->minor) {  // (those are on it already)
                    addPointOnCurve(sketch, end, id, kInternalTag);
                }
            }
        }
        if (const auto* sp = std::get_if<Spline>(&e.geom); sp && sp->isPiece()) {
            // (not an end that's on the curve already: a point it passes through, or its own end)
            for (Id end : {sp->start, sp->end}) {
                if (!splineEndOnCurve(*sp, end)) {
                    addPointOnCurve(sketch, end, id, kInternalTag);
                }
            }
        }
    }

    // Arcs joined end to end round a centre are on one circle.
    for (const auto& [id, e] : sketch.entities()) {
        if (const auto* a = std::get_if<Arc>(&e.geom)) {
            const auto key = std::make_pair(a->center, arcRoot(a->center, a->start));
            if (const auto [it, added] = chainArc.emplace(key, id); !added) {
                circleOf[circleRoot(id)] = circleRoot(it->second);
            }
        }
    }

    // Record, per entity, which unknowns define it (for fully-constrained colouring).
    for (const auto& [id, e] : sketch.entities()) {
        std::vector<double*> params;
        for (Id p : sketch.definingPoints(id)) {
            const GCS::Point& gp = points.at(p);
            params.push_back(gp.x);
            params.push_back(gp.y);
        }
        if (auto it = circles.find(id); it != circles.end()) {
            params.push_back(it->second.rad);
        }
        if (auto it = arcs.find(id); it != arcs.end()) {
            params.push_back(it->second.rad);
            params.push_back(it->second.startAngle);
            params.push_back(it->second.endAngle);
        }
        std::erase_if(params, [&](double* p) { return !unknownSet.contains(p); });
        entityUnknowns.emplace(id, std::move(params));
    }

    for (const auto& [id, c] : sketch.constraints()) {
        addConstraint(sketch, c);
    }

    sys.declareUnknowns(unknowns);
}

Solver::Impl::CurveModel Solver::Impl::model(const Sketch& sketch, Id id)
{
    CurveModel m;
    auto take = [&](Id p) {
        const GCS::Point& gp = points.at(p);
        m.params.push_back(gp.x);
        m.params.push_back(gp.y);
    };
    const Entity& e = sketch.entity(id);
    switch (e.kind()) {
        case EntityKind::Line:
            take(sketch.line(id).p1);
            take(sketch.line(id).p2);
            m.build = [](const double* v) { return curve::Path::line({v[0], v[1]}, {v[2], v[3]}); };
            break;
        case EntityKind::Circle:
            take(sketch.circle(id).center);
            m.params.push_back(circles.at(id).rad);
            m.build = [](const double* v) { return curve::Path::circle({v[0], v[1]}, v[2]); };
            break;
        case EntityKind::Arc: {
            const Arc& a = sketch.arc(id);
            take(a.center);
            take(a.start);
            take(a.end);
            m.build = [](const double* v) {
                const double s = std::atan2(v[3] - v[1], v[2] - v[0]);
                double sweep = std::fmod(std::atan2(v[5] - v[1], v[4] - v[0]) - s, 2 * kPi);
                if (sweep <= 0) {
                    sweep += 2 * kPi;
                }
                return curve::Path::arc({v[0], v[1]}, std::hypot(v[2] - v[0], v[3] - v[1]), s, sweep);
            };
            break;
        }
        case EntityKind::Ellipse:
            for (Id p : sketch.ellipseAxes(id)) {
                take(p);
            }
            m.build = [](const double* v) { return curve::Path::ellipse(frameFrom(v)); };
            break;
        case EntityKind::EllipseArc: {
            const EllipseArc& ea = sketch.ellipseArc(id);
            for (Id p : {ea.center, ea.major, ea.minor, ea.start, ea.end}) {
                take(p);
            }
            m.build = [](const double* v) {
                const curve::EllipseFrame f = frameFrom(v);
                const double t0 = f.param({v[6], v[7]});
                double sweep = std::fmod(f.param({v[8], v[9]}) - t0, 2 * kPi);
                if (sweep <= 0) {
                    sweep += 2 * kPi;
                }
                return curve::Path::ellipseArc(f, t0, sweep);
            };
            break;
        }
        case EntityKind::Spline: {
            const Spline& sp = sketch.spline(id);
            for (Id p : sp.points) {
                take(p);
            }
            const size_t n = sp.points.size();
            const SplineKind kind = sp.kind;
            const bool closed = sp.closed;
            m.build = [n, kind, closed](const double* v) {
                std::vector<Vec2> pts(n);
                for (size_t i = 0; i < n; ++i) {
                    pts[i] = {v[2 * i], v[2 * i + 1]};
                }
                std::vector<curve::Bezier> pieces = curve::splineBeziers(pts, kind, closed);
                const double count = static_cast<double>(pieces.size());
                return curve::Path::spline(std::move(pieces), closed, 0.0, count);
            };
            break;
        }
        case EntityKind::Point: throw SketchError("not a curve");
    }
    return m;
}

Solver::Impl::CurveModel Solver::Impl::pathModel(const Sketch& sketch, Id id)
{
    CurveModel m = model(sketch, id);
    const auto* sp = std::get_if<Spline>(&sketch.entity(id).geom);
    if (!sp || !sp->isPiece()) {
        return m;
    }
    // (as curve::pathOf: from the nearer-the-start end along the whole curve to the other)
    const size_t k = m.params.size();
    for (Id end : {sp->start, sp->end}) {
        m.params.push_back(points.at(end).x);
        m.params.push_back(points.at(end).y);
    }
    const bool closed = sp->closed;
    m.build = [whole = m.build, k, closed](const double* v) {
        const curve::Path w = whole(v);
        const double n = w.end();
        double u0 = w.param({v[k], v[k + 1]}), u1 = w.param({v[k + 2], v[k + 3]});
        if (closed) {
            double span = std::fmod(u1 - u0, n);
            if (span <= 0) {
                span += n;
            }
            return curve::Path::spline(w.pieces, true, u0, span);
        }
        if (u1 < u0) {
            std::swap(u0, u1);
        }
        return curve::Path::spline(w.pieces, false, u0, u1 - u0);
    };
    return m;
}

bool Solver::Impl::addPointOnCurve(const Sketch& sketch, Id p, Id curveId, int tag)
{
    if (!onCurves.insert(onCurveKey(sketch, p, curveId)).second) {
        return false;
    }
    const GCS::Point& gp = points.at(p);
    if (sketch.isElliptic(curveId)) {
        // On the whole ellipse (as PlaneGCS's point on an arc is on its circle): its implicit
        // equation, scaled to about a distance.
        GCS::VEC_pD params;
        for (Id q : sketch.ellipseAxes(curveId)) {
            params.push_back(points.at(q).x);
            params.push_back(points.at(q).y);
        }
        params.push_back(gp.x);
        params.push_back(gp.y);
        auto* on = new ConstraintNumeric(std::move(params), [](const std::vector<double>& v) {
            const curve::EllipseFrame f = frameFrom(v.data());
            const Vec2 d{v[6] - f.c.x, v[7] - f.c.y};
            const double x = dot(d, f.u) / std::max(f.a, 1e-12);
            const double y = dot(d, Vec2{-f.u.y, f.u.x}) / std::max(f.b, 1e-12);
            return (std::sqrt(x * x + y * y) - 1.0) * std::sqrt(f.a * f.b);
        });
        on->setTag(tag);
        sys.addConstraint(on);  // the system owns it
        return true;
    }
    // On a spline: at a parameter along it (a new unknown), in both coordinates.
    CurveModel m = model(sketch, curveId);
    std::vector<double> now(m.params.size());
    for (size_t i = 0; i < now.size(); ++i) {
        now[i] = *m.params[i];
    }
    double* t = alloc(m.build(now.data()).param({*gp.x, *gp.y}));
    unknowns.push_back(t);
    GCS::VEC_pD params = m.params;
    const size_t k = params.size();
    params.push_back(gp.x);
    params.push_back(gp.y);
    params.push_back(t);
    for (int component = 0; component < 2; ++component) {
        auto* on = new ConstraintNumeric(params, [build = m.build, k, component](const std::vector<double>& v) {
            const Vec2 q = build(v.data()).at(v[k + 2]);
            return component == 0 ? v[k] - q.x : v[k + 1] - q.y;
        });
        on->setTag(tag);
        sys.addConstraint(on);  // the system owns it
    }
    return true;
}

void Solver::Impl::addTangentCurves(const Sketch& sketch, Id a, Id b, Id joint, int tag)
{
    CurveModel ma = model(sketch, a), mb = model(sketch, b);
    GCS::VEC_pD params = ma.params;
    const size_t na = params.size();
    params.insert(params.end(), mb.params.begin(), mb.params.end());
    const size_t nb = params.size();
    std::vector<double> now(params.size());
    for (size_t i = 0; i < now.size(); ++i) {
        now[i] = *params[i];
    }
    const curve::Path pa = ma.build(now.data()), pb = mb.build(now.data() + na);

    if (joint != kNoId) {
        // Joined end to end: the same direction (or opposite) where they meet.
        params.push_back(points.at(joint).x);
        params.push_back(points.at(joint).y);
        auto* t = new ConstraintNumeric(params, [ba = ma.build, bb = mb.build, na, nb](const std::vector<double>& v) {
            const curve::Path p1 = ba(v.data()), p2 = bb(v.data() + na);
            const Vec2 j{v[nb], v[nb + 1]};
            return cross(unitOf(p1.derivative(p1.param(j))), unitOf(p2.derivative(p2.param(j))));
        });
        t->setTag(tag);
        sys.addConstraint(t);  // the system owns it
        return;
    }

    // Elsewhere: a contact point, at parameter s on a and t on b (two new unknowns). There b(t)
    // is on a (at its foot s), and their directions agree.
    double bestT = 0, best = 1e300;
    const int n = 256;
    for (int i = 0; i <= n; ++i) {
        const double t = pb.end() * i / n;
        const double gap = pa.distance(pb.at(t));
        if (gap < best) {
            best = gap;
            bestT = t;
        }
    }
    double* s = alloc(pa.param(pb.at(bestT)));
    double* t = alloc(bestT);
    unknowns.push_back(s);
    unknowns.push_back(t);
    params.push_back(s);
    params.push_back(t);
    auto contact = [ba = ma.build, bb = mb.build, na, nb](const std::vector<double>& v, int which) {
        const curve::Path p1 = ba(v.data()), p2 = bb(v.data() + na);
        const double s = v[nb], t = v[nb + 1];
        const Vec2 ta = unitOf(p1.derivative(s));
        const Vec2 gap = p2.at(t) - p1.at(s);
        switch (which) {
            case 0: return dot(gap, ta);    // s is the foot of b(t) on a
            case 1: return cross(ta, gap);  // b(t) is on a
            default: return cross(ta, unitOf(p2.derivative(t)));  // same direction there
        }
    };
    for (int which = 0; which < 3; ++which) {
        auto* eq = new ConstraintNumeric(params, [contact, which](const std::vector<double>& v) { return contact(v, which); });
        eq->setTag(tag);
        sys.addConstraint(eq);  // the system owns it
    }
}

bool Solver::Impl::impliedByArcs(const Sketch& sketch, const Constraint& c)
{
    auto arcOf = [&](Id id) -> const Arc* {
        return sketch.hasEntity(id) ? std::get_if<Arc>(&sketch.entity(id).geom) : nullptr;
    };
    // The circle (see circleOf) a point is on as the end of an arc round `centre`, if any.
    auto circleAt = [&](Id centre, Id p) -> Id {
        const Id root = arcRoot(centre, p);
        const auto it = chainArc.find({centre, root});
        return root == kNoId || it == chainArc.end() ? kNoId : circleRoot(it->second);
    };
    if (c.type == ConstraintType::Coincident && c.refs.size() == 2 && sketch.hasEntity(c.refs[1])) {
        // A point on an ellipse arc or spline that's on it by its making: an end of it, or a
        // point a spline passes through.
        const Entity& on = sketch.entity(c.refs[1]);
        const Id p = c.refs[0];
        if (const auto* ea = std::get_if<EllipseArc>(&on.geom); ea && (ea->start == p || ea->end == p)) {
            return true;
        }
        if (sketch.isElliptic(c.refs[1])) {
            const std::array<Id, 3> ax = sketch.ellipseAxes(c.refs[1]);
            if (p == ax[1] || p == ax[2]) {
                return true;  // its axis points are on it
            }
        }
        if (const auto* sp = std::get_if<Spline>(&on.geom)) {
            if (sp->start == p || sp->end == p
                || (sp->kind == SplineKind::Fit && std::ranges::find(sp->points, p) != sp->points.end())
                || (!sp->closed && (p == sp->points.front() || p == sp->points.back()))) {
                return true;
            }
        }
    }
    if (c.type == ConstraintType::Coincident && c.refs.size() == 2) {
        // A point on a line that's known to be on it already (an end of a line collinear with it,
        // or a point put on it before).
        for (int i = 0; i < 2; ++i) {
            const Id p = c.refs[static_cast<size_t>(i)], l = c.refs[static_cast<size_t>(1 - i)];
            if (sketch.hasEntity(p) && sketch.entity(p).kind() == EntityKind::Point && sketch.hasEntity(l)
                && sketch.entity(l).kind() == EntityKind::Line) {
                return !onLine[groupOf(sketch, l)].insert(p).second;
            }
        }
        // A point on an arc that is the end of an arc joined to it round the same centre.
        for (int i = 0; i < 2; ++i) {
            const Id p = c.refs[static_cast<size_t>(i)];
            const Id arcId = c.refs[static_cast<size_t>(1 - i)];
            if (const Arc* a = arcOf(arcId); a && sketch.hasEntity(p) && sketch.entity(p).kind() == EntityKind::Point) {
                // (an end of another arc round the same centre: now the two are one circle)
                const Id circle = circleAt(a->center, p), mine = circleRoot(arcId);
                if (circle == kNoId) {
                    return false;
                }
                if (circle == mine) {
                    return true;
                }
                circleOf[circle] = mine;
                return false;
            }
        }
    }
    if (c.type == ConstraintType::Equal && c.refs.size() == 2) {
        // Two arcs on the same centre already on one circle (joined end to end, or by other
        // equal radii); else they are now.
        const Arc* a = arcOf(c.refs[0]);
        const Arc* b = arcOf(c.refs[1]);
        if (!a || !b || a->center != b->center) {
            return false;
        }
        const Id ca = circleRoot(c.refs[0]), cb = circleRoot(c.refs[1]);
        if (ca == cb) {
            return true;
        }
        circleOf[cb] = ca;
        return false;
    }
    return false;
}

void Solver::Impl::addConstraint(const Sketch& sketch, const Constraint& c)
{
    if (impliedByArcs(sketch, c)) {
        return;
    }
    const int tag = static_cast<int>(c.id);
    const auto& r = c.refs;
    auto kind = [&](size_t i) { return sketch.entity(r[i]).kind(); };
    auto pt = [&](Id id) -> GCS::Point& { return points.at(id); };
    auto ln = [&](Id id) -> GCS::Line& { return lines.at(id); };

    switch (c.type) {
        case ConstraintType::Coincident:
            switch (kind(1)) {
                case EntityKind::Point: sys.addConstraintP2PCoincident(pt(r[0]), pt(r[1]), tag); break;
                case EntityKind::Line: sys.addConstraintPointOnLine(pt(r[0]), ln(r[1]), tag); break;
                case EntityKind::Circle: sys.addConstraintPointOnCircle(pt(r[0]), circles.at(r[1]), tag); break;
                case EntityKind::Arc: sys.addConstraintPointOnArc(pt(r[0]), arcs.at(r[1]), tag); break;
                case EntityKind::Ellipse:
                case EntityKind::EllipseArc:
                case EntityKind::Spline: addPointOnCurve(sketch, r[0], r[1], tag); break;  // (once)
            }
            break;

        case ConstraintType::Horizontal:
            if (r.size() == 1) {
                sys.addConstraintHorizontal(ln(r[0]), tag);
            }
            else {
                sys.addConstraintHorizontal(pt(r[0]), pt(r[1]), tag);
            }
            break;

        case ConstraintType::Vertical:
            if (r.size() == 1) {
                sys.addConstraintVertical(ln(r[0]), tag);
            }
            else {
                sys.addConstraintVertical(pt(r[0]), pt(r[1]), tag);
            }
            break;

        case ConstraintType::Parallel: sys.addConstraintParallel(ln(r[0]), ln(r[1]), tag); break;
        case ConstraintType::Perpendicular: sys.addConstraintPerpendicular(ln(r[0]), ln(r[1]), tag); break;

        case ConstraintType::Tangent:
            if (auto plain = [](EntityKind k) { return k == EntityKind::Line || k == EntityKind::Circle || k == EntityKind::Arc; };
                !plain(kind(0)) || !plain(kind(1))) {
                addTangentCurves(sketch, r[0], r[1], sharedEndpoint(sketch, r[0], r[1]), tag);
            }
            else if (const Id joint = sharedEndpoint(sketch, r[0], r[1]); joint != kNoId) {
                // Curves joined end to end: "meet at 0 or 180 degrees at the joint". The
                // distance-based form below is only satisfied at a minimum when the curves share
                // a point, which leaves the solver's Jacobian rank-deficient there (reported as
                // redundant, with the wrong DOF). Same approach as FreeCAD.
                GCS::Curve& c1 = curve(r[0]);
                GCS::Curve& c2 = curve(r[1]);
                GCS::Point& p = pt(joint);
                const double current = sys.calculateAngleViaPoint(c1, c2, p);
                sys.addConstraintAngleViaPoint(c1, c2, p, alloc(std::cos(current) >= 0.0 ? 0.0 : kPi), tag);
            }
            else if (kind(0) == EntityKind::Line) {
                GCS::Line& l = ln(r[0]);
                GCS::Circle& circ = round(r[1]);
                sys.addConstraintTangent(l, circ, isLeftOf(circ.center, l), tag);
            }
            else {
                sys.addConstraintTangent(round(r[0]), round(r[1]), tag);
            }
            break;

        case ConstraintType::Equal:
            if (kind(0) == EntityKind::Line) {
                sys.addConstraintEqualLength(ln(r[0]), ln(r[1]), tag);
            }
            else {
                sys.addConstraintEqualRadius(round(r[0]), round(r[1]), tag);
            }
            break;

        case ConstraintType::Ratio:
            if (kind(0) == EntityKind::Line) {
                auto* ratio = new ConstraintLengthRatio(ln(r[0]), ln(r[1]), c.value);
                ratio->setTag(tag);
                sys.addConstraint(ratio);  // the system owns it
            }
            else {
                auto* ratio = new ConstraintScaled(radiusOf(r[0]), radiusOf(r[1]), c.value);
                ratio->setTag(tag);
                sys.addConstraint(ratio);  // the system owns it
            }
            break;

        case ConstraintType::Midpoint: {
            GCS::Line& l = ln(r[1]);
            sys.addConstraintP2PSymmetric(l.p1, l.p2, pt(r[0]), tag);
            break;
        }

        case ConstraintType::Concentric:
            sys.addConstraintP2PCoincident(round(r[0]).center, round(r[1]).center, tag);
            break;

        case ConstraintType::Symmetric:
            sys.addConstraintP2PSymmetric(pt(r[0]), pt(r[1]), ln(r[2]), tag);
            break;

        case ConstraintType::Collinear: {
            // The ends of the second line lie on the first. An end known to be on it already (one
            // they share, or one on a line collinear with it) isn't constrained again: that's a
            // degenerate, "redundant" equation. Lines already collinear through others: nothing.
            const Id g1 = groupOf(sketch, r[0]), g2 = groupOf(sketch, r[1]);
            if (g1 == g2) {
                break;
            }
            const Line& second = sketch.line(r[1]);
            for (Id end : {second.p1, second.p2}) {
                if (!onLine[g1].contains(end)) {
                    sys.addConstraintPointOnLine(pt(end), ln(r[0]), tag);
                }
            }
            lineGroup[g2] = g1;
            onLine[g1].insert(onLine[g2].begin(), onLine[g2].end());
            onLine.erase(g2);
            break;
        }

        case ConstraintType::Distance:
            if (r.size() == 1) {
                GCS::Line& l = ln(r[0]);
                sys.addConstraintP2PDistance(l.p1, l.p2, alloc(c.value), tag);
            }
            else if (kind(1) == EntityKind::Point) {
                sys.addConstraintP2PDistance(pt(r[0]), pt(r[1]), alloc(c.value), tag);
            }
            else {
                GCS::Line& l = ln(r[1]);
                sys.addConstraintP2LDistance(pt(r[0]), l, alloc(c.value), isLeftOf(pt(r[0]), l), tag);
            }
            break;

        case ConstraintType::HorizontalDistance:
        case ConstraintType::VerticalDistance: {
            const bool horizontal = c.type == ConstraintType::HorizontalDistance;
            GCS::Point a, b;
            if (r.size() == 1) {
                a = ln(r[0]).p1;
                b = ln(r[0]).p2;
            }
            else {
                a = pt(r[0]);
                b = pt(r[1]);
            }
            double* pa = horizontal ? a.x : a.y;
            double* pb = horizontal ? b.x : b.y;
            // The value is unsigned; keep whichever side the second point is currently on.
            const double signedValue = (*pb - *pa) >= 0.0 ? c.value : -c.value;
            sys.addConstraintDifference(pa, pb, alloc(signedValue), tag);
            break;
        }

        case ConstraintType::Radius:
            if (sketch.isElliptic(r[0])) {  // the major semi-axis
                const std::array<Id, 3> ax = sketch.ellipseAxes(r[0]);
                sys.addConstraintP2PDistance(pt(ax[0]), pt(ax[1]), alloc(c.value), tag);
            }
            else if (kind(0) == EntityKind::Circle) {
                sys.addConstraintCircleRadius(circles.at(r[0]), alloc(c.value), tag);
            }
            else {
                sys.addConstraintArcRadius(arcs.at(r[0]), alloc(c.value), tag);
            }
            break;

        case ConstraintType::Diameter:
            if (sketch.isElliptic(r[0])) {  // the major axis
                const std::array<Id, 3> ax = sketch.ellipseAxes(r[0]);
                sys.addConstraintP2PDistance(pt(ax[0]), pt(ax[1]), alloc(c.value / 2), tag);
            }
            else if (kind(0) == EntityKind::Circle) {
                sys.addConstraintCircleDiameter(circles.at(r[0]), alloc(c.value), tag);
            }
            else {
                sys.addConstraintArcDiameter(arcs.at(r[0]), alloc(c.value), tag);
            }
            break;

        case ConstraintType::Angle: {
            // The angle between the rays the dimension measures (each line's p1->p2 direction,
            // possibly reversed: Constraint::flip), keeping its current rotation sense.
            // PlaneGCS constrains the angle between the lines' own directions, so convert.
            GCS::Line& l1 = ln(r[0]);
            GCS::Line& l2 = ln(r[1]);
            const double rayOffset = ((c.flip & 2) ? kPi : 0.0) - ((c.flip & 1) ? kPi : 0.0);
            const double current = wrapPi(lineAngle(l2) - lineAngle(l1) + rayOffset);
            const double rad = c.value * kPi / 180.0;
            const double betweenRays = current >= 0.0 ? rad : -rad;
            sys.addConstraintL2LAngle(l1, l2, alloc(wrapPi(betweenRays - rayOffset)), tag);
            break;
        }

        case ConstraintType::LinearPattern:
        case ConstraintType::CircularPattern: addPattern(sketch, c); break;
        case ConstraintType::Offset: addOffset(sketch, c); break;
        case ConstraintType::Divide: {
            const Line& l = sketch.line(r[0]);
            const std::vector<double> t = feature::divideFractions(feature::divideParams(c));
            for (size_t i = 0; i < t.size(); ++i) {
                for (int component = 0; component < 2; ++component) {
                    auto* along = new ConstraintAlong(points.at(l.p1), points.at(l.p2), points.at(r[i + 1]), t[i],
                                                      component);
                    along->setTag(tag);
                    sys.addConstraint(along);  // the system owns it
                }
            }
            break;
        }
    }
}

// Each copy is tied to its source point by point, so it is fully determined by the source and
// the pattern's settings: translated (rectangular) or rotated about the centre (circular).
// An arc already keeps its end on the circle through its start, so the end only needs one more
// equation (its direction from the centre); a second would be redundant.
void Solver::Impl::addPattern(const Sketch& sketch, const Constraint& c)
{
    const int tag = static_cast<int>(c.id);
    const feature::Pattern pattern = feature::pattern(c);
    const bool circular = c.type == ConstraintType::CircularPattern;

    for (size_t k = 0; k < pattern.copies.size(); ++k) {
        const Affine t = feature::instanceTransform(sketch, c, k + 1);
        const double angle = circular ? std::atan2(t.c, t.a) : 0.0;

        std::map<Id, Id> source;  // copy point -> source point
        std::map<Id, int> reduced;  // copy point -> arcs that pin it partly
        std::map<Id, Id> reducedBy;  // copy point -> that arc's (copy) centre
        for (size_t i = 0; i < pattern.sources.size(); ++i) {
            const Id src = pattern.sources[i];
            const Id copy = pattern.copies[k][i];
            const std::vector<Id> from = sketch.definingPoints(src);
            const std::vector<Id> to = sketch.definingPoints(copy);
            for (size_t j = 0; j < from.size(); ++j) {
                source[to[j]] = from[j];
            }
            const EntityKind kind = sketch.entity(src).kind();
            if (kind == EntityKind::Circle) {
                sys.addConstraintEqual(radiusOf(src), radiusOf(copy), tag);
            }
            else if (kind == EntityKind::Arc) {
                const Arc& a = sketch.arc(copy);
                const Id pinned = reduced[a.end] < 2 ? a.end : a.start;
                ++reduced[pinned];
                reducedBy[pinned] = a.center;
            }
            else if (kind == EntityKind::EllipseArc) {
                // Both ends are on the (copied) ellipse already: one equation each to place them.
                const EllipseArc& a = sketch.ellipseArc(copy);
                for (Id end : {a.start, a.end}) {
                    ++reduced[end];
                    reducedBy[end] = a.center;
                }
            }
            else if (kind == EntityKind::Spline) {
                // A piece's ends are on its (copied) curve: likewise, by direction from its first point.
                const Spline& sp = sketch.spline(copy);
                for (Id end : {sp.start, sp.end}) {
                    if (end != kNoId && !splineEndOnCurve(sp, end)) {
                        ++reduced[end];
                        reducedBy[end] = sp.points.front();
                    }
                }
            }
        }

        for (const auto& [q, p] : source) {
            const int needed = 2 - std::min(2, reduced[q]);
            GCS::Point& from = points.at(p);
            GCS::Point& to = points.at(q);
            if (needed == 1) {
                // Along the arc: the end's direction from the centre, turned like the copy.
                const Id copyCenter = reducedBy[q];
                auto* same = new ConstraintSameDirection(from, points.at(source.at(copyCenter)), to,
                                                         points.at(copyCenter), angle);
                same->setTag(tag);
                sys.addConstraint(same);  // the system owns it
                continue;
            }
            for (int component = 0; component < needed; ++component) {
                if (circular) {
                    auto* rotated = new ConstraintRotated(from, to, points.at(pattern.center), angle, component);
                    rotated->setTag(tag);
                    sys.addConstraint(rotated);  // the system owns it
                }
                else {
                    sys.addConstraintDifference(component == 0 ? from.x : from.y, component == 0 ? to.x : to.y,
                                                alloc(component == 0 ? t.tx : t.ty), tag);
                }
            }
        }
    }
}

// An offset arc shares its source's centre, with the radius off by the distance; an offset line
// keeps the distance from its source. Where offset curves meet, the shared point gets just
// enough equations to pin it without redundancy:
//  - free end of a line: at the distance, on the normal through the source end;
//  - free end of an arc, or any smooth join with an arc: on the arc's radius through the source
//    point (the arc keeps it on its circle);
//  - sharp corner: on both offset curves (at the distance from each line; arcs see to themselves);
//  - smooth join of two lines: at the distance, on the normal.
// Arcs joined smoothly to arcs pass the distance along through the join (the radial line pins the
// point, which sets the next arc's radius), so only one arc per such group carries the radius
// equation.
void Solver::Impl::addOffset(const Sketch& sketch, const Constraint& c)
{
    // Both-sides offsets are two independent sides sharing the distance.
    const feature::OffsetRefs all = feature::offsetRefs(c);
    addOffsetSide(sketch, c, all.sources, all.offsets);
    if (!all.offsets2.empty()) {
        addOffsetSide(sketch, c, all.sources, all.offsets2);
    }
    // A cap's ends are both fixed by the sides, so the usual arc rules (start and end on the
    // circle: four equations) would say one thing twice. Its radius and start angle come from
    // its start, and only the angle from its end.
    const int tag = static_cast<int>(c.id);
    for (Id cap : all.caps) {
        GCS::Arc& a = arcs.at(cap);
        sys.addConstraintP2PDistance(a.center, a.start, a.rad, tag);
        sys.addConstraintP2PAngle(a.center, a.start, a.startAngle, 0.0, tag);
        sys.addConstraintP2PAngle(a.center, a.end, a.endAngle, 0.0, tag);
    }
}

void Solver::Impl::addOffsetSide(const Sketch& sketch, const Constraint& c, const std::vector<Id>& sources,
                                 const std::vector<Id>& offsets)
{
    const int tag = static_cast<int>(c.id);
    const double distance = c.value;
    const struct
    {
        const std::vector<Id>& sources;
        const std::vector<Id>& offsets;
    } refs{sources, offsets};
    const size_t n = refs.sources.size();
    auto kind = [&](size_t i) { return sketch.entity(refs.sources[i]).kind(); };
    auto ends = [&](Id curve) -> std::array<Id, 2> {
        const Entity& e = sketch.entity(curve);
        if (const auto* l = std::get_if<Line>(&e.geom)) {
            return {l->p1, l->p2};
        }
        if (const auto* ea = std::get_if<EllipseArc>(&e.geom)) {
            return {ea->start, ea->end};
        }
        if (const auto* sp = std::get_if<Spline>(&e.geom)) {
            if (!sp->isPiece()) {
                return {sp->points.front(), sp->points.back()};
            }
            const Vec2 first = curve::pathOf(sketch, curve).at(0);  // (in the order its path runs)
            const Vec2 s0 = pos(points.at(sp->start)), s1 = pos(points.at(sp->end));
            return std::hypot(s0.x - first.x, s0.y - first.y) <= std::hypot(s1.x - first.x, s1.y - first.y)
                ? std::array<Id, 2>{sp->start, sp->end}
                : std::array<Id, 2>{sp->end, sp->start};
        }
        const Arc& a = std::get<Arc>(e.geom);
        return {a.start, a.end};
    };
    // Ellipses and splines: their offsets are splines through points kept at the distance.
    auto curvy = [&](size_t i) {
        const EntityKind k = sketch.entity(refs.sources[i]).kind();
        return k == EntityKind::Ellipse || k == EntityKind::EllipseArc || k == EntityKind::Spline;
    };
    auto closedCurvy = [&](size_t i) {
        const Entity& e = sketch.entity(refs.sources[i]);
        const auto* sp = std::get_if<Spline>(&e.geom);
        return e.kind() == EntityKind::Ellipse || (sp && sp->closed && !sp->isPiece());
    };
    std::map<size_t, CurveModel> models;
    auto sourceModel = [&](size_t i) -> CurveModel& {
        if (!models.contains(i)) {
            models.emplace(i, pathModel(sketch, refs.sources[i]));
        }
        return models.at(i);
    };
    auto pathNow = [&](size_t i) {
        const CurveModel& m = sourceModel(i);
        std::vector<double> v(m.params.size());
        for (size_t k = 0; k < v.size(); ++k) {
            v[k] = *m.params[k];
        }
        return m.build(v.data());
    };
    // Which side of the source (in its own direction) its offset is on: as it is now.
    std::map<size_t, double> sides;
    auto sideOf = [&](size_t i) {
        if (!sides.contains(i)) {
            const Spline& off = sketch.spline(refs.offsets[i]);
            const bool closedOff = off.closed;
            const size_t pieces = closedOff ? off.points.size() : off.points.size() - 1;
            const size_t k = closedOff ? 0 : pieces / 2;
            const curve::Path p = pathNow(i);
            const double u = p.end() * static_cast<double>(k) / static_cast<double>(pieces);
            const Vec2 q = pos(points.at(off.points[k]));
            sides[i] = cross(p.derivative(u), q - p.at(u)) >= 0.0 ? 1.0 : -1.0;
        }
        return sides.at(i);
    };
    auto offsetPoint = [](const curve::Path& p, double u, double sign, double d) {
        const Vec2 n = unitOf(p.derivative(u));
        const Vec2 s = p.at(u);
        return Vec2{s.x - sign * d * n.y, s.y + sign * d * n.x};
    };
    // q at the distance from source i, square to it at the fraction `frac` along it.
    auto fixedOffset = [&](size_t i, Id q, double frac) {
        const CurveModel& m = sourceModel(i);
        const double sign = sideOf(i);
        GCS::VEC_pD params = m.params;
        const size_t k = params.size();
        params.push_back(points.at(q).x);
        params.push_back(points.at(q).y);
        for (int component = 0; component < 2; ++component) {
            auto* eq = new ConstraintNumeric(params, [build = m.build, k, component, frac, sign, distance,
                                                      offsetPoint](const std::vector<double>& v) {
                const curve::Path p = build(v.data());
                const Vec2 o = offsetPoint(p, frac * p.end(), sign, distance);
                return component == 0 ? v[k] - o.x : v[k + 1] - o.y;
            });
            eq->setTag(tag);
            sys.addConstraint(eq);  // the system owns it
        }
    };
    // q on source i's offset somewhere (at a parameter found with it).
    auto onOffset = [&](size_t i, Id q) {
        const CurveModel& m = sourceModel(i);
        const double sign = sideOf(i);
        double* u = alloc(pathNow(i).param(pos(points.at(q))));
        unknowns.push_back(u);
        GCS::VEC_pD params = m.params;
        const size_t k = params.size();
        params.push_back(points.at(q).x);
        params.push_back(points.at(q).y);
        params.push_back(u);
        for (int component = 0; component < 2; ++component) {
            auto* eq = new ConstraintNumeric(params, [build = m.build, k, component, sign, distance,
                                                      offsetPoint](const std::vector<double>& v) {
                const Vec2 o = offsetPoint(build(v.data()), v[k + 2], sign, distance);
                return component == 0 ? v[k] - o.x : v[k + 1] - o.y;
            });
            eq->setTag(tag);
            sys.addConstraint(eq);  // the system owns it
        }
    };
    auto radiusEquation = [&](size_t i) {
        double* src = radiusOf(refs.sources[i]);
        double* off = radiusOf(refs.offsets[i]);
        sys.addConstraintDifference(src, off, alloc(*off >= *src ? distance : -distance), tag);
    };

    struct Joint
    {
        Id source = kNoId;
        std::vector<size_t> curves;
    };
    std::map<Id, Joint> joints;  // offset point -> where it comes from
    for (size_t i = 0; i < n; ++i) {
        if (kind(i) == EntityKind::Circle) {
            radiusEquation(i);
            continue;
        }
        if (curvy(i) && closedCurvy(i)) {
            continue;  // (its offset's points are all placed from it: see feature::placeOffsetPoints)
        }
        const auto from = ends(refs.sources[i]);
        const auto to = ends(refs.offsets[i]);
        for (size_t j = 0; j < 2; ++j) {
            joints[to[j]].source = from[j];
            joints[to[j]].curves.push_back(i);
        }
    }

    auto tangent = [&](size_t i, Id p) {
        const Entity& e = sketch.entity(refs.sources[i]);
        Vec2 t;
        if (const auto* l = std::get_if<Line>(&e.geom)) {
            t = pos(points.at(l->p2)) - pos(points.at(l->p1));
        }
        else if (curvy(i)) {
            const curve::Path path = pathNow(i);
            t = path.derivative(path.param(pos(points.at(p))));
        }
        else {
            const Vec2 r = pos(points.at(p)) - pos(points.at(std::get<Arc>(e.geom).center));
            t = {-r.y, r.x};
        }
        const double len = std::hypot(t.x, t.y);
        return len > 0.0 ? Vec2{t.x / len, t.y / len} : Vec2{1.0, 0.0};
    };
    auto lineDistance = [&](size_t i, Id q) {
        GCS::Line& l = lines.at(refs.sources[i]);
        GCS::Point& p = points.at(q);
        sys.addConstraintP2LDistance(p, l, alloc(distance), isLeftOf(p, l), tag);
    };
    auto normal = [&](size_t i, Id p, Id q) {
        GCS::Line& l = lines.at(refs.sources[i]);
        sys.addConstraintPerpendicular(points.at(p), points.at(q), l.p1, l.p2, tag);
    };
    auto radial = [&](size_t i, Id p, Id q) {
        sys.addConstraintPointOnLine(points.at(q), arcs.at(refs.sources[i]).center, points.at(p), tag);
    };

    std::vector<size_t> group(n);
    for (size_t i = 0; i < n; ++i) {
        group[i] = i;
    }
    auto root = [&](size_t i) {
        while (group[i] != i) {
            i = group[i];
        }
        return i;
    };

    for (const auto& [q, joint] : joints) {
        const Id p = joint.source;
        const size_t i = joint.curves[0];
        // (at an end of source i: 0 at its first, 1 at its last)
        auto fracOf = [&](size_t k) { return ends(refs.sources[k])[0] == p ? 0.0 : 1.0; };
        if (joint.curves.size() == 1) {
            if (kind(i) == EntityKind::Line) {
                lineDistance(i, q);
                normal(i, p, q);
            }
            else if (curvy(i)) {
                fixedOffset(i, q, fracOf(i));
            }
            else {
                radial(i, p, q);
            }
            continue;
        }
        const size_t j = joint.curves[1];
        const Vec2 ti = tangent(i, p), tj = tangent(j, p);
        const bool smooth = std::abs(ti.x * tj.y - ti.y * tj.x) < 1e-6;
        const bool arcI = kind(i) == EntityKind::Arc, arcJ = kind(j) == EntityKind::Arc;

        if (curvy(i) || curvy(j)) {
            const bool lineI = kind(i) == EntityKind::Line, lineJ = kind(j) == EntityKind::Line;
            if (smooth) {
                if (arcI || arcJ) {
                    radial(arcI ? i : j, p, q);  // (the arc keeps it on its circle)
                }
                else if (lineI || lineJ) {
                    lineDistance(lineI ? i : j, q);
                    normal(lineI ? i : j, p, q);
                }
                else {
                    fixedOffset(i, q, fracOf(i));
                }
            }
            else {
                // A sharp corner: on both offsets (an arc's keeps it on its circle by itself).
                for (size_t k : {i, j}) {
                    if (kind(k) == EntityKind::Line) {
                        lineDistance(k, q);
                    }
                    else if (curvy(k)) {
                        onOffset(k, q);
                    }
                }
            }
        }
        else if (arcI && arcJ) {
            if (smooth) {
                if (root(i) != root(j)) {
                    group[root(j)] = root(i);
                }
                radial(i, p, q);
            }
        }
        else if (arcI || arcJ) {
            if (smooth) {
                radial(arcI ? i : j, p, q);
            }
            else {
                lineDistance(arcI ? j : i, q);
            }
        }
        else {
            lineDistance(i, q);
            if (smooth) {
                normal(i, p, q);
            }
            else {
                lineDistance(j, q);
            }
        }
    }

    for (size_t i = 0; i < n; ++i) {
        if (kind(i) == EntityKind::Arc && root(i) == i) {
            radiusEquation(i);
        }
    }
}

// ---------------------------------------------------------------------------------------------

namespace
{
// The part of a sketch in one layer, plus the origin and axes, to solve on its own.
Sketch layerPart(const Sketch& s, Id layer)
{
    Sketch part;
    std::vector<const Entity*> curves;
    for (const auto& [id, e] : s.entities()) {
        if (!s.isReference(id) && s.layerOf(id) != layer) {
            continue;
        }
        if (e.kind() == EntityKind::Point) {
            part.insertEntity(e);
        }
        else {
            curves.push_back(&e);
        }
    }
    // (Anything that would reach outside the layer is left out rather than break the solve.)
    for (const Entity* e : curves) {
        const std::vector<Id> on = s.definingPoints(e->id);
        if (std::ranges::all_of(on, [&](Id p) { return p == e->id || part.hasEntity(p); })) {
            part.insertEntity(*e);
        }
    }
    for (const auto& [id, c] : s.constraints()) {
        if (s.layerOf(id) == layer && std::ranges::all_of(c.refs, [&](Id r) { return part.hasEntity(r); })) {
            part.insertConstraint(c);
        }
    }
    return part;
}

// Layers in the order to solve them: each after those it projects from.
std::vector<Id> solveOrder(const Sketch& s)
{
    std::vector<Id> todo;
    for (auto it = s.layers().rbegin(); it != s.layers().rend(); ++it) {
        todo.push_back(it->id);
    }
    std::vector<Id> order;
    while (!todo.empty()) {
        auto ready = std::ranges::find_if(todo, [&](Id l) {
            return std::ranges::none_of(todo, [&](Id other) { return other != l && s.dependsOn(l, other); });
        });
        if (ready == todo.end()) {
            ready = todo.begin();  // (a circle, which projecting refuses to make)
        }
        order.push_back(*ready);
        todo.erase(ready);
    }
    return order;
}
}  // namespace

Solver::Solver(const Sketch& sketch)
    : d(std::make_unique<Impl>())
{
    d->build(sketch);
}

Solver::Solver(const Sketch& sketch, Id layer)
    : d(std::make_unique<Impl>())
{
    if (layer == kNoId || sketch.layers().size() == 1) {
        d->build(sketch);
    }
    else {
        d->build(layerPart(sketch, layer));
    }
}

Solver::~Solver() = default;

SolveResult Solver::solve()
{
    SolveResult result;
    auto& sys = d->sys;

    sys.invalidatedDiagnosis();
    sys.initSolution(GCS::DogLeg);  // also runs diagnosis

    GCS::SolveStatus status = sys.solve(GCS::DogLeg);
    if (status != GCS::SolveStatus::Success) {
        // Big changes (a point pulled onto an ellipse far away) can leave DogLeg short of the
        // answer; Levenberg-Marquardt, from where it started, often gets there.
        sys.undoSolution();
        status = sys.solve(GCS::LevenbergMarquardt);
    }
    if (status == GCS::SolveStatus::Success) {
        sys.applySolution();
    }
    else {
        sys.undoSolution();
    }

    result.dof = sys.dofsNumber();
    GCS::VEC_I tags;
    auto toIds = [](const GCS::VEC_I& in) {
        std::vector<Id> out;
        for (int t : in) {
            if (t > 0) {
                out.push_back(static_cast<Id>(t));
            }
        }
        return out;
    };
    sys.getConflicting(tags);
    result.conflicting = toIds(tags);
    sys.getRedundant(tags);
    result.redundant = toIds(tags);
    sys.getPartiallyRedundant(tags);
    result.partiallyRedundant = toIds(tags);
    // When everything is pinned down, PlaneGCS reports "over-constrained by n" as negative DOF.
    // If none of the extra equations is the user's (e.g. an arc slot's five arcs, where any four
    // of their radius rules imply the fifth), the sketch is just fully constrained.
    if (result.dof < 0 && result.redundant.empty() && result.conflicting.empty()) {
        result.dof = 0;
    }

    result.ok = status == GCS::SolveStatus::Success && result.conflicting.empty();

    // An entity is fully constrained when none of its unknowns can still move. With no driving
    // constraints at all, PlaneGCS reports no dependent parameters, so everything is free.
    GCS::VEC_pD dependent;
    sys.getDependentParams(dependent);
    const std::set<double*> freeParams(dependent.begin(), dependent.end());
    const bool nothingConstrained = sys.isEmptyDiagnoseMatrix();
    for (const auto& [id, params] : d->entityUnknowns) {
        const bool free = std::ranges::any_of(params, [&](double* p) {
            return nothingConstrained || freeParams.contains(p);
        });
        if (!free) {
            result.fullyConstrained.insert(id);
        }
    }

    return result;
}

void Solver::writeBack(Sketch& sketch) const
{
    for (const auto& [id, gp] : d->points) {
        if (sketch.hasEntity(id)) {
            sketch.point(id) = Point{*gp.x, *gp.y};
        }
    }
    for (const auto& [id, gc] : d->circles) {
        if (sketch.hasEntity(id)) {
            sketch.circle(id).radius = *gc.rad;
        }
    }
}

void Solver::beginDrag(const std::vector<Id>& pointIds)
{
    endDrag();
    for (Id id : pointIds) {
        auto it = d->points.find(id);
        if (it == d->points.end()) {
            continue;
        }
        GCS::Point& p = it->second;
        if (std::ranges::find(d->unknowns, p.x) == d->unknowns.end()) {
            continue;  // fixed
        }
        Impl::DragTarget t{*p.x, *p.y, d->alloc(*p.x), d->alloc(*p.y)};
        GCS::Point target(t.x, t.y);
        d->sys.addConstraintP2PCoincident(p, target, GCS::DefaultTemporaryConstraint);
        d->dragTargets.push_back(t);
    }
    d->sys.invalidatedDiagnosis();
    d->sys.initSolution(GCS::DogLeg);
    d->dragging = true;
}

void Solver::beginDragEdge(Id id, double x, double y)
{
    endDrag();
    if (!d->circles.contains(id) && !d->arcs.contains(id)) {
        return;
    }
    GCS::Circle& c = d->round(id);

    // If the radius can still change, hold the centre so the drag resizes the circle (as in
    // Fusion). Otherwise the rim pull moves the whole circle.
    d->sys.invalidatedDiagnosis();
    d->sys.initSolution(GCS::DogLeg);
    GCS::VEC_pD dependent;
    d->sys.getDependentParams(dependent);
    const bool radiusFree = std::ranges::find(d->unknowns, c.rad) != d->unknowns.end()
        && (d->sys.isEmptyDiagnoseMatrix() || std::ranges::find(dependent, c.rad) != dependent.end());
    if (radiusFree) {
        GCS::Point hold(d->alloc(*c.center.x), d->alloc(*c.center.y));
        d->sys.addConstraintP2PCoincident(c.center, hold, GCS::DefaultTemporaryConstraint);
    }

    Impl::DragTarget t{x, y, d->alloc(x), d->alloc(y)};
    GCS::Point target(t.x, t.y);
    d->sys.addConstraintP2PDistance(c.center, target, c.rad, GCS::DefaultTemporaryConstraint);
    d->dragTargets.push_back(t);
    d->sys.invalidatedDiagnosis();
    d->sys.initSolution(GCS::DogLeg);
    d->dragging = true;
}

bool Solver::dragTo(double dx, double dy)
{
    return dragMap([&](Vec2 p) { return Vec2{p.x + dx, p.y + dy}; });
}

bool Solver::dragMap(const std::function<Vec2(Vec2)>& to)
{
    if (!d->dragging) {
        return false;
    }
    for (auto& t : d->dragTargets) {
        const Vec2 target = to({t.startX, t.startY});
        *t.x = target.x;
        *t.y = target.y;
    }
    const GCS::SolveStatus status = d->sys.solve(GCS::DogLeg);
    if (status == GCS::SolveStatus::Success) {
        d->sys.applySolution();
        return true;
    }
    d->sys.undoSolution();
    return false;
}

void Solver::endDrag()
{
    if (!d->dragging) {
        return;
    }
    d->sys.clearByTag(GCS::DefaultTemporaryConstraint);
    d->dragTargets.clear();
    d->dragging = false;
}

SolveResult solveOnce(Sketch& sketch);

SolveResult solve(Sketch& sketch)
{
    // Layers share no unknowns (projections are fixed copies), so one solve does them all; but a
    // projection is a constant there, so when its original moves, it's moved and solved again
    // (once per layer it passes through, at most).
    // Offsets of ellipses and splines are placed from their sources after each solve; if that
    // moved them (and something is tied to them), solve again.
    feature::placeOffsetPoints(sketch);
    SolveResult result = solveOnce(sketch);
    for (int pass = 0; pass < 3 && result.ok && feature::placeOffsetPoints(sketch); ++pass) {
        result = solveOnce(sketch);
    }
    return result;
}

SolveResult solveOnce(Sketch& sketch)
{
    if (sketch.layers().size() == 1) {
        sketch.syncProjections();  // (only unlinked ones can be here: they stay put)
        Solver solver(sketch);
        SolveResult result = solver.solve();
        if (result.ok) {
            solver.writeBack(sketch);
        }
        else {
            result.failedLayers.insert(sketch.layers().front().id);
        }
        return result;
    }
    SolveResult total;
    total.ok = true;
    for (Id layer : solveOrder(sketch)) {
        sketch.syncProjections(layer);
        Solver solver(sketch, layer);
        SolveResult r = solver.solve();
        if (r.ok) {
            solver.writeBack(sketch);
        }
        else {
            total.failedLayers.insert(layer);
        }
        total.ok = total.ok && r.ok;
        total.dof += r.dof;
        total.conflicting.insert(total.conflicting.end(), r.conflicting.begin(), r.conflicting.end());
        total.redundant.insert(total.redundant.end(), r.redundant.begin(), r.redundant.end());
        total.partiallyRedundant.insert(total.partiallyRedundant.end(), r.partiallyRedundant.begin(),
                                        r.partiallyRedundant.end());
        total.fullyConstrained.insert(r.fullyConstrained.begin(), r.fullyConstrained.end());
    }
    return total;
}

SolveResult solveHolding(Sketch& sketch, const std::vector<Id>& points)
{
    Sketch held = sketch;
    held.syncProjections();
    {
        Solver solver(held, points.empty() ? kNoId : held.layerOf(points.front()));
        solver.beginDrag(points);  // pulled softly towards where they are now
        if (solver.dragTo(0.0, 0.0)) {
            solver.writeBack(held);
        }
    }
    SolveResult result = solve(held);
    if (result.ok) {
        sketch = std::move(held);
    }
    return result;
}

}  // namespace core
