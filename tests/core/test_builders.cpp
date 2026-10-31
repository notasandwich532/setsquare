// Composite shape builders: each must solve cleanly, with exactly the expected degrees of
// freedom and nothing redundant (so a fresh shape never shows up amber).

#include <core/Builders.h>
#include <core/Solver.h>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <numbers>

using namespace core;
using CT = ConstraintType;
using Catch::Approx;
namespace build = core::build;

namespace
{
constexpr double kTol = 1e-7;

Vec2 pos(const Sketch& s, Id p)
{
    return {s.point(p).x, s.point(p).y};
}

double dist(Vec2 a, Vec2 b)
{
    return std::hypot(b.x - a.x, b.y - a.y);
}

double length(const Sketch& s, Id line)
{
    return dist(pos(s, s.line(line).p1), pos(s, s.line(line).p2));
}

double radiusOf(const Sketch& s, Id round)
{
    return s.entity(round).kind() == EntityKind::Circle ? s.circle(round).radius : s.arcRadius(round);
}

Vec2 centreOf(const Sketch& s, Id round)
{
    return pos(s, s.entity(round).kind() == EntityKind::Circle ? s.circle(round).center : s.arc(round).center);
}

// Solve, and require a clean result with the given DOF.
void requireClean(Sketch& s, int dof)
{
    const SolveResult r = solve(s);
    CHECK(r.ok);
    CHECK(r.conflicting.empty());
    CHECK(r.redundant.empty());
    CHECK(r.dof == dof);
}

double lineDistance(const Sketch& s, Vec2 p, Id line)
{
    const Vec2 a = pos(s, s.line(line).p1), b = pos(s, s.line(line).p2);
    return std::abs((b.x - a.x) * (p.y - a.y) - (b.y - a.y) * (p.x - a.x)) / dist(a, b);
}
}  // namespace

TEST_CASE("3-point rectangle: rotated, right-angled, DOF 5", "[builders]")
{
    Sketch s;
    const Id a = s.addPoint(0, 0), b = s.addPoint(30, 10);
    const build::Rectangle r = build::rectangle3(s, a, b, {20, 40});
    requireClean(s, 5);
    for (int i = 0; i < 4; ++i) {
        const Vec2 p0 = pos(s, r.corners[i]), p1 = pos(s, r.corners[(i + 1) % 4]), p2 = pos(s, r.corners[(i + 2) % 4]);
        CHECK(((p1.x - p0.x) * (p2.x - p1.x) + (p1.y - p0.y) * (p2.y - p1.y)) == Approx(0.0).margin(1e-6));
    }
    CHECK(length(s, r.sides[0]) == Approx(std::hypot(30.0, 10.0)));
}

TEST_CASE("Centre rectangle: centred on its point, DOF 4", "[builders]")
{
    Sketch s;
    const Id c = s.addPoint(5, 5);
    const build::Rectangle r = build::rectangleCenter(s, c, {15, 12});
    requireClean(s, 4);
    const Vec2 a = pos(s, r.corners[0]), opp = pos(s, r.corners[2]);
    CHECK((a.x + opp.x) / 2 == Approx(5.0));
    CHECK((a.y + opp.y) / 2 == Approx(5.0));
    CHECK(s.entity(r.diagonals[0]).construction);
}

TEST_CASE("2-point and 3-point circles", "[builders]")
{
    Sketch s;
    const Id c2 = build::circle2(s, {0, 0}, {10, 0});
    CHECK(s.circle(c2).radius == Approx(5.0));
    CHECK(centreOf(s, c2).x == Approx(5.0));
    const Id c3 = build::circle3(s, {0, 10}, {10, 0}, {0, -10});
    CHECK(s.circle(c3).radius == Approx(10.0));
    requireClean(s, 6);
    CHECK_THROWS_AS(build::circle3(s, {0, 0}, {1, 1}, {2, 2}), SketchError);
}

TEST_CASE("Circle tangent to two lines: DOF 1 (its size)", "[builders]")
{
    Sketch s;
    const Id l1 = s.addLine(0, 0, 40, 0);
    const Id l2 = s.addLine(0, 0, 0, 40);
    s.entity(l1).fixed = s.entity(l2).fixed = true;
    const Id c = build::circleTangent(s, {l1, l2}, {10, 10}, 5.0);
    requireClean(s, 1);
    const Vec2 centre = centreOf(s, c);
    CHECK(lineDistance(s, centre, l1) == Approx(s.circle(c).radius).margin(kTol));
    CHECK(lineDistance(s, centre, l2) == Approx(s.circle(c).radius).margin(kTol));
    CHECK(centre.x > 0);  // in the quadrant of `near`
    CHECK(centre.y > 0);
}

TEST_CASE("Circle tangent to three lines picks the incircle or an excircle by position", "[builders]")
{
    Sketch s;
    const Id a = s.addLine(0, 0, 30, 0);
    const Id b = s.addLine(30, 0, 0, 40);
    const Id c = s.addLine(0, 40, 0, 0);
    for (Id l : {a, b, c}) {
        s.entity(l).fixed = true;
    }
    const Id in = build::circleTangent(s, {a, b, c}, {8, 8});
    requireClean(s, 0);
    CHECK(s.circle(in).radius == Approx(10.0).margin(1e-6));  // 3-4-5 triangle scaled: r = (30+40-50)/2

    Sketch t = s;
    const Id ex = build::circleTangent(t, {a, b, c}, {40, 40});
    requireClean(t, 0);
    CHECK(t.circle(ex).radius > 10.0);
}

TEST_CASE("Tangent arc continues smoothly off a line", "[builders]")
{
    Sketch s;
    const Id l = s.addLine(0, 0, 20, 0);
    s.entity(l).fixed = true;
    const Id end = s.addPoint(30, 10);
    const Id arc = build::arcTangent(s, l, s.line(l).p2, end);
    const SolveResult r = solve(s);
    CHECK(r.ok);
    CHECK(r.redundant.empty());
    // Centre straight above the join, so the arc leaves along the line's direction.
    const Vec2 c = centreOf(s, arc);
    CHECK(c.x == Approx(20.0).margin(1e-6));
    CHECK(c.y == Approx(10.0).margin(1e-6));
    CHECK_THROWS_AS(build::arcTangent(s, l, s.line(l).p2, s.addPoint(40, 0)), SketchError);  // straight ahead
}

TEST_CASE("Polygons: regular, DOF 4, every variant", "[builders]")
{
    for (int sides : {3, 5, 6, 8}) {
        Sketch s;
        const Id c = s.addPoint(0, 0);
        const build::Polygon in = build::polygon(s, c, {10, 0}, sides, build::PolygonKind::Inscribed);
        requireClean(s, 4);
        for (Id side : in.sides) {
            CHECK(length(s, side) == Approx(length(s, in.sides[0])).margin(1e-7));
        }
        for (Id v : in.vertices) {
            CHECK(dist(pos(s, v), {0, 0}) == Approx(10.0).margin(1e-7));
        }

        Sketch t;
        const Id tc = t.addPoint(0, 0);
        const build::Polygon out = build::polygon(t, tc, {10, 0}, sides, build::PolygonKind::Circumscribed);
        requireClean(t, 4);
        for (Id side : out.sides) {
            CHECK(lineDistance(t, {0, 0}, side) == Approx(10.0).margin(1e-7));  // apothem
        }

        Sketch u;
        const Id a = u.addPoint(0, 0), b = u.addPoint(10, 0);
        const build::Polygon edge = build::polygonEdge(u, a, b, sides);
        requireClean(u, 4);
        CHECK(pos(u, edge.center).y > 0);  // on the left of a->b
        for (Id side : edge.sides) {
            CHECK(length(u, side) == Approx(10.0).margin(1e-7));
        }
    }
}

TEST_CASE("Straight slots: DOF 5, sides tangent to equal caps", "[builders]")
{
    auto check = [](Sketch& s, const build::Slot& slot, double width) {
        requireClean(s, 5);
        const Id cap1 = slot.curves[3], cap2 = slot.curves[1];
        CHECK(radiusOf(s, cap1) == Approx(width / 2).margin(1e-7));
        CHECK(radiusOf(s, cap2) == Approx(width / 2).margin(1e-7));
        for (Id side : {slot.curves[0], slot.curves[2]}) {
            CHECK(lineDistance(s, centreOf(s, cap1), side) == Approx(width / 2).margin(1e-7));
        }
        CHECK(s.entity(slot.centerline).construction);
    };
    {
        Sketch s;
        const build::Slot slot = build::slot(s, s.addPoint(0, 0), s.addPoint(30, 10), 8);
        check(s, slot, 8);
    }
    {
        Sketch s;
        const build::Slot slot = build::slotOverall(s, {0, 0}, {40, 0}, 10);
        check(s, slot, 10);
        CHECK(pos(s, slot.centers[0]).x == Approx(5.0));
        CHECK(pos(s, slot.centers[1]).x == Approx(35.0));
    }
    {
        Sketch s;
        const Id m = s.addPoint(0, 0);
        const build::Slot slot = build::slotCenterPoint(s, m, {15, 0}, 6);
        check(s, slot, 6);
        CHECK(pos(s, slot.centers[0]).x == Approx(-15.0));
    }
}

TEST_CASE("Arc slots: DOF 6, concentric sides, caps tangent", "[builders]")
{
    auto check = [](Sketch& s, const build::Slot& slot, double width) {
        requireClean(s, 6);
        const Id outer = slot.curves[0], inner = slot.curves[2];
        CHECK(radiusOf(s, outer) - radiusOf(s, inner) == Approx(width).margin(1e-7));
        for (Id cap : {slot.curves[1], slot.curves[3]}) {
            CHECK(radiusOf(s, cap) == Approx(width / 2).margin(1e-7));
        }
        CHECK(s.entity(slot.centerline).construction);
    };
    {
        Sketch s;
        const Id c = s.addPoint(0, 0);
        const build::Slot slot = build::arcSlot(s, c, {20, 0}, {0, 20}, 4);
        check(s, slot, 4);
    }
    {
        Sketch s;
        const build::Slot slot = build::arcSlot3(s, {20, 0}, {-20, 0}, {0, 20}, 4);
        check(s, slot, 4);
    }
    {
        Sketch s;
        CHECK_THROWS_AS(build::arcSlot(s, s.addPoint(0, 0), {3, 0}, {0, 3}, 10), SketchError);  // too wide
    }
}

TEST_CASE("Collinear puts the second line on the first", "[builders][constraints]")
{
    Sketch s;
    const Id a = s.addLine(0, 0, 10, 0);
    s.entity(a).fixed = true;
    const Id b = s.addLine(20, 3, 30, 5);
    s.addConstraint(CT::Collinear, {a, b});
    REQUIRE(solve(s).ok);
    CHECK(s.point(s.line(b).p1).y == Approx(0.0).margin(1e-9));
    CHECK(s.point(s.line(b).p2).y == Approx(0.0).margin(1e-9));
}
