#include "Solver.h"

#include <planegcs/GCS.h>

#include <algorithm>
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

    void build(const Sketch& sketch);
    void addConstraint(const Sketch& sketch, const Constraint& c);
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
    store.reserve(4 * pointCount + 3 * roundCount + 2 * sketch.constraints().size() + 16);

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
            sys.addConstraintArcRules(it->second, kInternalTag);
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
    if (!d->dragging) {
        return false;
    }
    for (auto& t : d->dragTargets) {
        *t.x = t.startX + dx;
        *t.y = t.startY + dy;
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

}  // namespace core
