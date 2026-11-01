#include "Solver.h"

#include "Features.h"

#include <planegcs/GCS.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <stdexcept>
#include <vector>
#include <map>
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

Vec2 operator-(Vec2 a, Vec2 b)
{
    return {a.x - b.x, a.y - b.y};
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
    store.reserve(4 * pointCount + 3 * roundCount + 2 * sketch.constraints().size() + featureValues + 16);

    // Arcs capping a both-sides offset: their ends are pinned by the two sides already.
    for (const auto& [id, c] : sketch.constraints()) {
        if (c.type == ConstraintType::Offset) {
            for (Id cap : feature::offsetRefs(c).caps) {
                capArcs.insert(cap);
            }
        }
    }

    // Points that may not move: fixed points, and the defining points of fixed curves.
    std::set<Id> fixedPoints;
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
            if (!capArcs.contains(id)) {  // (caps get theirs from their offset: see addOffset)
                sys.addConstraintArcRules(it->second, kInternalTag);
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

void Solver::Impl::addConstraint(const Sketch& sketch, const Constraint& c)
{
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
            if (const Id joint = sharedEndpoint(sketch, r[0], r[1]); joint != kNoId) {
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
            // The ends of the second line lie on the first. An end the lines share is on it
            // already (and constraining it anyway is a degenerate, "redundant" equation).
            const Line& first = sketch.line(r[0]);
            const Line& second = sketch.line(r[1]);
            for (Id end : {second.p1, second.p2}) {
                if (end != first.p1 && end != first.p2) {
                    sys.addConstraintPointOnLine(pt(end), ln(r[0]), tag);
                }
            }
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
            if (kind(0) == EntityKind::Circle) {
                sys.addConstraintCircleRadius(circles.at(r[0]), alloc(c.value), tag);
            }
            else {
                sys.addConstraintArcRadius(arcs.at(r[0]), alloc(c.value), tag);
            }
            break;

        case ConstraintType::Diameter:
            if (kind(0) == EntityKind::Circle) {
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
        const Arc& a = std::get<Arc>(e.geom);
        return {a.start, a.end};
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
        if (joint.curves.size() == 1) {
            if (kind(i) == EntityKind::Line) {
                lineDistance(i, q);
                normal(i, p, q);
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

        if (arcI && arcJ) {
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

Solver::Solver(const Sketch& sketch)
    : d(std::make_unique<Impl>())
{
    d->build(sketch);
}

Solver::~Solver() = default;

SolveResult Solver::solve()
{
    SolveResult result;
    auto& sys = d->sys;

    sys.invalidatedDiagnosis();
    sys.initSolution(GCS::DogLeg);  // also runs diagnosis

    const GCS::SolveStatus status = sys.solve(GCS::DogLeg);
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

SolveResult solve(Sketch& sketch)
{
    Solver solver(sketch);
    SolveResult result = solver.solve();
    if (result.ok) {
        solver.writeBack(sketch);
    }
    return result;
}

SolveResult solveHolding(Sketch& sketch, const std::vector<Id>& points)
{
    Sketch held = sketch;
    {
        Solver solver(held);
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
