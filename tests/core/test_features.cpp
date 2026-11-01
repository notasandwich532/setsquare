// Copy / move / scale / mirror, linked patterns and offsets.

#include <core/Builders.h>
#include <core/Features.h>
#include <core/Geometry.h>
#include <core/Modify.h>
#include <core/Serialize.h>
#include <core/Solver.h>
#include <core/Transform.h>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <algorithm>
#include <cmath>
#include <numbers>

using namespace core;
using Catch::Approx;

namespace
{
Vec2 at(const Sketch& s, Id p)
{
    return {s.point(p).x, s.point(p).y};
}

// Solves and checks the sketch is healthy: solved, nothing redundant or conflicting.
SolveResult solveClean(Sketch& s)
{
    const SolveResult r = solve(s);
    INFO("redundant " << r.redundant.size() << ", partly " << r.partiallyRedundant.size() << ", conflicting "
                      << r.conflicting.size());
    CHECK(r.ok);
    CHECK(r.redundant.empty());
    CHECK(r.partiallyRedundant.empty());
    CHECK(r.conflicting.empty());
    return r;
}

// Changes a feature's value the way the app does: its source stays put.
void setValue(Sketch& s, Id feature, double value)
{
    s.setConstraintValue(feature, value);
    if (s.constraint(feature).type != ConstraintType::Offset) {
        feature::placeCopies(s, feature);
    }
    const SolveResult r = solveHolding(s, feature::sourcePoints(s, s.constraint(feature)));
    CHECK(r.ok);
    CHECK(r.redundant.empty());
}

// Drags a point and solves; returns whether it converged.
bool drag(Sketch& s, Id point, double dx, double dy)
{
    Solver solver(s);
    solver.beginDrag({point});
    const bool ok = solver.dragTo(dx, dy);
    solver.endDrag();
    solver.writeBack(s);
    return ok && solve(s).ok;
}

double lineDistance(const Sketch& s, Id line, Vec2 p)
{
    const Line& l = s.line(line);
    const Vec2 a = at(s, l.p1), b = at(s, l.p2);
    return std::abs((b.x - a.x) * (p.y - a.y) - (b.y - a.y) * (p.x - a.x)) / std::hypot(b.x - a.x, b.y - a.y);
}

build::Rectangle rect(Sketch& s, double x0, double y0, double x1, double y1)
{
    return build::rectangle(s, s.addPoint(x0, y0), s.addPoint(x1, y1));
}

std::vector<Id> ids(const build::Rectangle& r)
{
    return {r.sides.begin(), r.sides.end()};
}
}  // namespace

// ---------------------------------------------------------------------------------------------
// Transforms
// ---------------------------------------------------------------------------------------------

TEST_CASE("Affine maps compose and mirror", "[transform]")
{
    const Affine r = Affine::rotation({1, 1}, 90);
    const Vec2 p = r.apply({2, 1});
    CHECK(p.x == Approx(1.0));
    CHECK(p.y == Approx(2.0));
    const Affine m = Affine::mirror({0, 0}, {0, 1});
    CHECK(m.apply({3, 4}).x == Approx(-3.0));
    CHECK(m.mirrors());
    const Affine both = Affine::translation(1, 0).then(Affine::scaling({0, 0}, 2));
    CHECK(both.apply({1, 1}).x == Approx(4.0));
    CHECK(both.scale() == Approx(2.0));
}

TEST_CASE("Copying keeps internal constraints and scales dimensions", "[transform]")
{
    Sketch s;
    const auto r = rect(s, 0, 0, 20, 10);
    const Id width = s.addConstraint(ConstraintType::Distance, {r.sides[0]}, 20.0);
    solveClean(s);

    SECTION("translated")
    {
        const auto map = xform::copy(s, ids(r), Affine::translation(50, 0));
        CHECK(map.contains(width));
        CHECK(s.constraint(map.at(width)).value == Approx(20.0));
        const SolveResult res = solveClean(s);
        CHECK(res.dof == 2 * 3);  // two rectangles, each with a width
        CHECK(at(s, map.at(r.corners[0])).x == Approx(50.0));
    }
    SECTION("scaled")
    {
        const auto map = xform::copy(s, ids(r), Affine::scaling({0, 0}, 2));
        CHECK(s.constraint(map.at(width)).value == Approx(40.0));
        solveClean(s);
    }
    SECTION("rotated 90 degrees: horizontal and vertical swap")
    {
        const auto map = xform::copy(s, ids(r), Affine::rotation({0, 0}, 90));
        const auto horizontals = std::ranges::count_if(s.constraints(), [&](const auto& kv) {
            return kv.second.type == ConstraintType::Horizontal;
        });
        CHECK(horizontals == 4);
        CHECK(solveClean(s).dof == 6);
    }
    SECTION("rotated 30 degrees: horizontal and vertical are dropped")
    {
        const size_t before = s.constraints().size();
        xform::copy(s, ids(r), Affine::rotation({0, 0}, 30));
        CHECK(s.constraints().size() == before + 1);  // just the width
        CHECK(solveClean(s).dof == 3 + 7);
    }
}

TEST_CASE("Scaling in place scales the dimensions between the scaled entities", "[transform]")
{
    Sketch s;
    const auto r = rect(s, 0, 0, 20, 10);
    const Id width = s.addConstraint(ConstraintType::Distance, {r.sides[0]}, 20.0);
    const Id circle = s.addCircle(10, 5, 2);
    const Id radius = s.addConstraint(ConstraintType::Radius, {circle}, 2.0);
    std::vector<Id> all = ids(r);
    all.push_back(circle);
    xform::apply(s, all, Affine::scaling({0, 0}, 1.5));
    CHECK(s.constraint(width).value == Approx(30.0));
    CHECK(s.constraint(radius).value == Approx(3.0));
    CHECK(s.circle(circle).radius == Approx(3.0));
    solveClean(s);
    CHECK(at(s, r.corners[2]).x == Approx(30.0));
}

TEST_CASE("Mirroring ties the copy with symmetric constraints", "[transform][mirror]")
{
    Sketch s;
    s.ensureOrigin();
    const auto r = rect(s, 5, 0, 20, 10);
    const Id arc = s.addArc(30, 5, 4, 0, 90);
    const Id circle = s.addCircle(10, 20, 3);
    std::vector<Id> sel = ids(r);
    sel.push_back(arc);
    sel.push_back(circle);
    const SolveResult before = solveClean(s);

    const auto map = xform::mirror(s, sel, s.yAxis());
    const SolveResult after = solve(s);
    CHECK(after.ok);
    CHECK(after.redundant.empty());
    CHECK(after.dof == before.dof);  // the mirror image adds no freedom
    const ArcShape a = arcShape(s, map.at(arc));
    CHECK(a.center.x == Approx(-30.0));
    CHECK(a.radius == Approx(4.0));

    REQUIRE(drag(s, r.corners[2], 5, 3));
    CHECK(at(s, map.at(r.corners[2])).x == Approx(-at(s, r.corners[2]).x).margin(1e-6));
    CHECK(at(s, map.at(r.corners[2])).y == Approx(at(s, r.corners[2]).y).margin(1e-6));
}

TEST_CASE("Points on the mirror line are shared", "[transform][mirror]")
{
    Sketch s;
    s.ensureOrigin();
    const Id l = s.addLine(s.origin(), s.addPoint(10, 10));
    const auto map = xform::mirror(s, {l}, s.yAxis());
    const Line copy = s.line(map.at(l));
    CHECK(copy.p1 == s.origin());
    CHECK(solveClean(s).dof == 2);
}

// ---------------------------------------------------------------------------------------------
// Patterns
// ---------------------------------------------------------------------------------------------

TEST_CASE("Rectangular pattern follows its source and spacing", "[pattern]")
{
    Sketch s;
    const auto r = rect(s, 0, 0, 10, 5);
    const Id circle = s.addCircle(5, 2.5, 1);
    std::vector<Id> sel = ids(r);
    sel.push_back(circle);
    const int dof = solveClean(s).dof;

    feature::LinearPatternParams p;
    p.count = 3;
    p.spacing = 20;
    p.count2 = 2;
    p.spacing2 = 15;
    const Id k = feature::linearPattern(s, sel, p);
    const feature::Pattern decoded = feature::pattern(s.constraint(k));
    REQUIRE(decoded.copies.size() == 5);
    CHECK(solveClean(s).dof == dof);

    // Instance (2, 1): 40 across, 15 up.
    const Id lastCircle = decoded.copies[4].back();
    CHECK(at(s, s.circle(lastCircle).center).x == Approx(45.0));
    CHECK(at(s, s.circle(lastCircle).center).y == Approx(17.5));

    REQUIRE(drag(s, r.corners[2], 2, 1));  // the source's top-right corner
    CHECK(s.circle(lastCircle).radius == Approx(1.0));
    const Id copySide = decoded.copies[4][1];
    CHECK(at(s, s.line(copySide).p1).x == Approx(at(s, r.corners[1]).x + 40.0).margin(1e-6));

    setValue(s, k, 25.0);
    CHECK(at(s, s.circle(lastCircle).center).x == Approx(55.0));

    p.count = 2;
    const Id k2 = feature::setLinearPattern(s, k, p);
    CHECK(feature::pattern(s.constraint(k2)).copies.size() == 3);
    CHECK(solveClean(s).dof == dof);
    CHECK(!s.hasEntity(lastCircle));
}

TEST_CASE("Symmetric rectangular patterns go both ways", "[pattern]")
{
    Sketch s;
    const Id c = s.addCircle(0, 0, 1);
    feature::LinearPatternParams p;
    p.count = 3;
    p.spacing = 10;
    p.symmetric = true;
    p.count2 = 2;
    p.spacing2 = 5;
    p.symmetric2 = true;
    const Id k = feature::linearPattern(s, {c}, p);
    const feature::Pattern decoded = feature::pattern(s.constraint(k));
    REQUIRE(decoded.copies.size() == 5 * 3 - 1);
    double minX = 0, maxX = 0, minY = 0, maxY = 0;
    for (const auto& instance : decoded.copies) {
        const Vec2 q = at(s, s.circle(instance[0]).center);
        minX = std::min(minX, q.x);
        maxX = std::max(maxX, q.x);
        minY = std::min(minY, q.y);
        maxY = std::max(maxY, q.y);
    }
    CHECK(minX == Approx(-20.0));
    CHECK(maxX == Approx(20.0));
    CHECK(minY == Approx(-5.0));
    CHECK(maxY == Approx(5.0));
    CHECK(solveClean(s).dof == 3);
    CHECK(feature::linearParams(s.constraint(k)).symmetric2);

    p.symmetric2 = false;  // a different layout: the copies are remade
    const Id k2 = feature::setLinearPattern(s, k, p);
    CHECK(feature::pattern(s.constraint(k2)).copies.size() == 5 * 2 - 1);
    CHECK(solveClean(s).dof == 3);
}

TEST_CASE("Circular pattern rotates copies about the centre", "[pattern]")
{
    Sketch s;
    s.ensureOrigin();
    const Id circle = s.addCircle(20, 0, 2);
    const Id arc = s.addArc(30, 0, 3, 0, 120);
    const int dof = solveClean(s).dof;

    feature::CircularPatternParams p;
    p.count = 4;
    const Id k = feature::circularPattern(s, {circle, arc}, s.origin(), p);
    const feature::Pattern decoded = feature::pattern(s.constraint(k));
    REQUIRE(decoded.copies.size() == 3);
    CHECK(solveClean(s).dof == dof);
    const Vec2 c1 = at(s, s.circle(decoded.copies[0][0]).center);
    CHECK(c1.x == Approx(0.0).margin(1e-9));
    CHECK(c1.y == Approx(20.0));

    setValue(s, k, 90.0);  // spread over a quarter turn instead
    const Vec2 last = at(s, s.circle(decoded.copies[2][0]).center);
    CHECK(last.x == Approx(0.0).margin(1e-9));
    CHECK(last.y == Approx(20.0));

    REQUIRE(drag(s, s.arc(arc).end, 1, 1));
    CHECK(arcShape(s, decoded.copies[1][1]).radius == Approx(arcShape(s, arc).radius).margin(1e-6));
}

TEST_CASE("Patterned arcs can be dragged after the pattern changes", "[pattern]")
{
    Sketch s;
    s.ensureOrigin();
    const Id arc = s.addArc(30, 0, 3, 0, 120);
    const int n = GENERATE(0, 2, 4);
    const double total = GENERATE(360.0, 90.0);
    const bool change = GENERATE(false, true);
    if (n > 0) {
        const Id k = feature::circularPattern(s, {arc}, s.origin(), {n, 360});
        if (change) {
            setValue(s, k, total);
        }
        else {
            feature::setCircularPattern(s, k, {n, total});
            solve(s);
        }
    }
    INFO("n " << n << " total " << total << " change " << change);
    CHECK(drag(s, s.arc(arc).end, 1, 1));
    CHECK(drag(s, s.arc(arc).start, 1, 1));
    CHECK(drag(s, s.arc(arc).center, 1, 1));
}

TEST_CASE("Patterned slots: arcs joined to lines aren't over-constrained", "[pattern]")
{
    Sketch s;
    const auto slot = build::slot(s, s.addPoint(0, 0), s.addPoint(20, 0), 6);
    const int dof = solveClean(s).dof;
    feature::LinearPatternParams p;
    p.angle = 90;
    feature::linearPattern(s, slot.curves, p);
    CHECK(solveClean(s).dof == dof);
}

TEST_CASE("Deleting a pattern leaves the copies, unlinked", "[pattern]")
{
    Sketch s;
    const Id l = s.addLine(0, 0, 10, 0);
    const Id k = feature::linearPattern(s, {l}, {});
    s.remove(k);
    CHECK(solveClean(s).dof == 4 * 3);
}

TEST_CASE("Patterns survive a save and load", "[pattern][serialize]")
{
    Sketch s;
    s.ensureOrigin();
    const Id l = s.addLine(10, 0, 15, 0);
    const Id k = feature::circularPattern(s, {l}, s.origin(), {5, 180});
    Sketch loaded = fromJson(toJson(s));
    REQUIRE(loaded.hasConstraint(k));
    CHECK(loaded.constraint(k).params == s.constraint(k).params);
    CHECK(solveClean(loaded).dof == 4);
}

// ---------------------------------------------------------------------------------------------
// Offsets
// ---------------------------------------------------------------------------------------------

TEST_CASE("Offset a rectangle outwards and inwards", "[offset]")
{
    Sketch s;
    const auto r = rect(s, 0, 0, 20, 10);
    const int dof = solveClean(s).dof;

    const bool outside = GENERATE(true, false);
    const Id k = feature::offset(s, ids(r), 2.0, outside ? Vec2{-5, 5} : Vec2{2, 5});
    CHECK(solveClean(s).dof == dof);
    const feature::OffsetRefs refs = feature::offsetRefs(s.constraint(k));
    REQUIRE(refs.offsets.size() == 4);
    const Line bottom = s.line(refs.offsets[0]);
    CHECK(at(s, bottom.p1).y == Approx(outside ? -2.0 : 2.0));
    CHECK(at(s, bottom.p1).x == Approx(outside ? -2.0 : 2.0));

    setValue(s, k, 1.0);
    CHECK(at(s, s.line(refs.offsets[0]).p1).y == Approx(outside ? -1.0 : 1.0));
    REQUIRE(drag(s, r.corners[2], 5, 5));
    for (size_t i = 0; i < 4; ++i) {
        const Line o = s.line(refs.offsets[i]);
        CHECK(lineDistance(s, refs.sources[i], at(s, o.p1)) == Approx(1.0));
        CHECK(lineDistance(s, refs.sources[i], at(s, o.p2)) == Approx(1.0));
    }
}

TEST_CASE("Offset chains with arcs", "[offset]")
{
    Sketch s;
    std::vector<Id> curves;
    Vec2 toward;
    SECTION("slot (smooth line-arc joins, closed)")
    {
        curves = build::slot(s, s.addPoint(0, 0), s.addPoint(20, 0), 6).curves;
        toward = {10, 10};
    }
    SECTION("arc slot (smooth arc-arc joins, closed)")
    {
        curves = build::arcSlot(s, s.addPoint(0, 0), {20, 0}, {0, 20}, 4).curves;
        toward = {0, 0};
    }
    SECTION("filleted corner (open)")
    {
        const Id a = s.addLine(0, 0, 20, 0);
        const Id b = s.addLine(20, 0, 20, 20);
        s.addConstraint(ConstraintType::Coincident, {s.line(a).p2, s.line(b).p1});
        const auto corner = modify::fillet(s, a, {5, 0}, b, {20, 15}, 5);
        curves = feature::chainAt(s, corner.joint);
        REQUIRE(curves.size() == 3);
        toward = {10, 5};
    }
    SECTION("sharp line-arc corner (open)")
    {
        const Id a = s.addLine(-20, 0, 0, 0);
        const Id arc = s.addArc(s.addPoint(0, 10), s.line(a).p2, s.addPoint(10, 10));
        curves = {a, arc};
        toward = {-10, 5};
    }
    const int dof = solveClean(s).dof;
    const Id k = feature::offset(s, curves, 1.5, toward);
    CHECK(solveClean(s).dof == dof);
    setValue(s, k, 2.5);
    const feature::OffsetRefs refs = feature::offsetRefs(s.constraint(k));
    for (size_t i = 0; i < refs.sources.size(); ++i) {
        if (s.entity(refs.sources[i]).kind() == EntityKind::Arc) {
            CHECK(std::abs(arcShape(s, refs.offsets[i]).radius - arcShape(s, refs.sources[i]).radius)
                  == Approx(2.5));
        }
        else {
            const Line o = s.line(refs.offsets[i]);
            CHECK(lineDistance(s, refs.sources[i], at(s, o.p1)) == Approx(2.5));
            CHECK(lineDistance(s, refs.sources[i], at(s, o.p2)) == Approx(2.5));
        }
    }
}

TEST_CASE("Offset to both sides, capping an open chain's ends", "[offset]")
{
    Sketch s;
    std::vector<Id> chain;
    SECTION("polyline")
    {
        const Id a = s.addLine(0, 0, 20, 0);
        const Id b = s.addLine(s.line(a).p2, s.addPoint(30, 15));
        chain = {a, b};
    }
    SECTION("line and tangent arc")
    {
        const Id a = s.addLine(-20, 0, 0, 0);
        const Id arc = s.addArc(s.addPoint(0, 10), s.line(a).p2, s.addPoint(10, 10));
        s.addConstraint(ConstraintType::Tangent, {a, arc});
        chain = {a, arc};
    }
    const int dof = solveClean(s).dof;
    const size_t before = s.entities().size();
    const Id k = feature::offset(s, chain, 2.0, {}, true, true);
    CHECK(solveClean(s).dof == dof);
    const feature::OffsetRefs refs = feature::offsetRefs(s.constraint(k));
    CHECK(refs.offsets.size() == 2);
    CHECK(refs.offsets2.size() == 2);
    // Two caps: arcs round the chain's ends, radius the distance.
    size_t caps = 0;
    for (const auto& [id, e] : s.entities()) {
        if (id > s.highestId() - (s.entities().size() - before) && e.kind() == EntityKind::Arc
            && std::ranges::find(refs.offsets, id) == refs.offsets.end()
            && std::ranges::find(refs.offsets2, id) == refs.offsets2.end() && std::ranges::find(chain, id) == chain.end()) {
            CHECK(arcShape(s, id).radius == Approx(2.0));
            CHECK(arcShape(s, id).sweep == Approx(std::numbers::pi));
            ++caps;
        }
    }
    CHECK(caps == 2);

    setValue(s, k, 3.0);  // one distance drives both sides and the caps
    for (const auto* side : {&refs.offsets, &refs.offsets2}) {
        const Id first = (*side)[0];
        const Line o = s.line(first);
        CHECK(lineDistance(s, chain[0], at(s, o.p1)) == Approx(3.0));
    }
    CHECK(solveClean(s).dof == dof);
}

TEST_CASE("Both sides of closed chains and circles", "[offset]")
{
    Sketch s;
    const auto r = rect(s, 0, 0, 20, 10);
    const Id c = s.addCircle(50, 50, 10);
    const int dof = solveClean(s).dof;
    const Id k1 = feature::offset(s, ids(r), 2, {}, true, true);  // closed: nothing to cap
    const Id k2 = feature::offset(s, {c}, 3, {}, true);
    CHECK(solveClean(s).dof == dof);
    CHECK(feature::offsetRefs(s.constraint(k1)).offsets2.size() == 4);
    const auto circles = feature::offsetRefs(s.constraint(k2));
    CHECK(s.circle(circles.offsets[0]).radius == Approx(13.0));
    CHECK(s.circle(circles.offsets2[0]).radius == Approx(7.0));
    const Sketch loaded = fromJson(toJson(s));
    CHECK(feature::offsetRefs(loaded.constraint(k1)).offsets2.size() == 4);
}

TEST_CASE("Offset a circle", "[offset]")
{
    Sketch s;
    const Id c = s.addCircle(0, 0, 10);
    const Id k = feature::offset(s, {c}, 3, {0, 0});
    const Id inner = feature::offsetRefs(s.constraint(k)).offsets[0];
    CHECK(s.circle(inner).radius == Approx(7.0));
    CHECK(solveClean(s).dof == 3);
    CHECK_THROWS_AS(feature::offset(s, {c}, 12, {0, 0}), SketchError);
}

// --- dividing a line ---

TEST_CASE("Divide: a ratio's parts are the gaps from the start, biggest first for 3:2:1", "[features][divide]")
{
    CHECK(feature::parseRatio("3:2:1") == std::vector<double>{3, 2, 1});
    CHECK(feature::parseRatio(" 1, 2.5 ,1 ") == std::vector<double>{1, 2.5, 1});
    CHECK(feature::ratioText({3, 2, 1}) == "3:2:1");
    CHECK_THROWS_AS(feature::parseRatio("3"), SketchError);       // no point
    CHECK_THROWS_AS(feature::parseRatio("3:0:1"), SketchError);   // a gap of nothing
    CHECK_THROWS_AS(feature::parseRatio("3:-1"), SketchError);
    CHECK_THROWS_AS(feature::parseRatio("3:two"), SketchError);

    feature::DivideParams p{{3, 2, 1}, false, true};
    auto t = feature::divideFractions(p);
    REQUIRE(t.size() == 2);
    CHECK(t[0] == Approx(0.5));
    CHECK(t[1] == Approx(5.0 / 6.0));
    p.reversed = true;
    t = feature::divideFractions(p);
    CHECK(t[0] == Approx(0.5));
    CHECK(t[1] == Approx(1.0 / 6.0));

    t = feature::divideFractions(feature::evenly(3));
    CHECK(t == std::vector<double>{0.25, 0.5, 0.75});
}

TEST_CASE("Divide: the points stay at their fractions as the line moves, adding no freedom", "[features][divide]")
{
    Sketch s;
    const Id l = s.addLine(0, 0, 60, 0);
    const Id k = feature::divide(s, l, {{3, 2, 1}, false, true});
    const Constraint c = s.constraint(k);
    REQUIRE(c.refs.size() == 3);
    CHECK(c.value == 2.0);
    CHECK(solveClean(s).dof == 4);  // just the line's

    // Fix the line's ends somewhere else: the points follow.
    const Line line = s.line(l);
    s.point(line.p1) = Point{6, 12};
    s.point(line.p2) = Point{36, 42};
    s.entity(line.p1).fixed = true;
    s.entity(line.p2).fixed = true;
    CHECK(solveClean(s).dof == 0);
    CHECK(at(s, c.refs[1]).x == Approx(21.0).margin(1e-7));  // halfway
    CHECK(at(s, c.refs[1]).y == Approx(27.0).margin(1e-7));
    CHECK(at(s, c.refs[2]).x == Approx(31.0).margin(1e-7));  // five sixths
    CHECK(at(s, c.refs[2]).y == Approx(37.0).margin(1e-7));
}

TEST_CASE("Divide: changing the spacing moves the points; a new count makes new ones", "[features][divide]")
{
    Sketch s;
    const Id l = s.addLine(0, 0, 12, 0);
    const Id k = feature::divide(s, l, feature::evenly(1));
    const Id point = s.constraint(k).refs[1];
    CHECK(at(s, point).x == Approx(6.0));

    CHECK(feature::setDivide(s, k, {{1, 2}, true, true}) == k);  // still one point: same feature
    CHECK(at(s, point).x == Approx(8.0));  // a third of the way from the end
    CHECK(solveClean(s).dof == 4);

    const Id again = feature::setDivide(s, k, feature::evenly(3));
    CHECK(again != k);
    CHECK_FALSE(s.hasEntity(point));
    CHECK(s.constraint(again).refs.size() == 4);
    CHECK(at(s, s.constraint(again).refs[3]).x == Approx(9.0));
}

TEST_CASE("Divide: saved and loaded; deleting a point deletes the divide", "[features][divide]")
{
    Sketch s;
    const Id l = s.addLine(0, 0, 10, 10);
    const Id k = feature::divide(s, l, {{3, 2, 1}, true, true});
    Sketch back = fromJson(toJson(s));
    REQUIRE(back.hasConstraint(k));
    const auto p = feature::divideParams(back.constraint(k));
    CHECK(p.parts == std::vector<double>{3, 2, 1});
    CHECK(p.reversed);
    CHECK(p.byRatio);
    CHECK(solveClean(back).dof == 4);

    back.remove(back.constraint(k).refs[1]);
    CHECK_FALSE(back.hasConstraint(k));

    CHECK_THROWS_AS(feature::divide(s, s.line(l).p1, feature::evenly(2)), SketchError);  // not a line
    CHECK_THROWS_AS(s.setConstraintValue(k, 5), SketchError);  // the value is the count
}