// Geometric queries: picking, box selection, arcs; and dragging circle rims.

#include <core/Geometry.h>
#include <core/Solver.h>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cmath>
#include <numbers>

using namespace core;
using Catch::Approx;

namespace
{
bool contains(const std::vector<Id>& v, Id id)
{
    return std::ranges::find(v, id) != v.end();
}
}  // namespace

TEST_CASE("Picking prefers points over curves", "[geometry]")
{
    Sketch s;
    const Id l = s.addLine(0, 0, 10, 0);
    const Id end = s.line(l).p2;
    PickOptions opt;
    opt.tolerance = 0.5;
    CHECK(pick(s, {9.8, 0.1}, opt) == end);
    CHECK(pick(s, {5.0, 0.3}, opt) == l);
    CHECK(pick(s, {5.0, 3.0}, opt) == kNoId);
    opt.exclude = {end};
    CHECK(pick(s, {9.8, 0.1}, opt) == l);
}

TEST_CASE("Distance to circle and arc rims", "[geometry]")
{
    Sketch s;
    const Id c = s.addCircle(0, 0, 10);
    const Id a = s.addArc(0, 0, 10, 0, 90);
    CHECK(distanceTo(s, c, {0, 12}) == Approx(2.0));
    CHECK(distanceTo(s, a, {0, 12}) == Approx(2.0));          // within the sweep
    CHECK(distanceTo(s, a, {-12, 0}) == Approx(std::hypot(12.0, 10.0)));  // nearest endpoint (0, 10)
    CHECK(distanceTo(s, a, {0, -10}) > 9.0);                  // outside the sweep: nearest endpoint
}

TEST_CASE("Arc shape reports a counter-clockwise sweep", "[geometry]")
{
    Sketch s;
    const Id a = s.addArc(0, 0, 5, 270, 90);  // right half, wrapping through 0
    const ArcShape shape = arcShape(s, a);
    CHECK(shape.radius == Approx(5.0));
    CHECK(shape.sweep == Approx(std::numbers::pi));
}

TEST_CASE("Circumcircle of three points", "[geometry]")
{
    Vec2 c;
    double r = 0;
    REQUIRE(circumcircle({10, 0}, {0, 10}, {-10, 0}, c, r));
    CHECK(c.x == Approx(0.0).margin(1e-12));
    CHECK(c.y == Approx(0.0).margin(1e-12));
    CHECK(r == Approx(10.0));
    CHECK_FALSE(circumcircle({0, 0}, {1, 1}, {2, 2}, c, r));
}

TEST_CASE("Window selection takes enclosed entities; crossing takes touched ones", "[geometry]")
{
    Sketch s;
    const Id inside = s.addLine(1, 1, 4, 4);
    const Id across = s.addLine(3, 3, 20, 3);
    const Id circle = s.addCircle(50, 50, 5);

    const auto window = boxSelect(s, {0, 0}, {10, 10}, false);
    CHECK(contains(window, inside));
    CHECK_FALSE(contains(window, across));
    CHECK(contains(window, s.line(across).p1));  // its endpoint is inside
    CHECK_FALSE(contains(window, circle));

    const auto crossing = boxSelect(s, {10, 0}, {0, 10}, true);
    CHECK(contains(crossing, inside));
    CHECK(contains(crossing, across));
    CHECK_FALSE(contains(crossing, circle));

    // A box inside a circle doesn't touch its rim.
    CHECK_FALSE(contains(boxSelect(s, {49, 49}, {51, 51}, true), circle));
    CHECK(contains(boxSelect(s, {54, 49}, {56, 51}, true), circle));
}

TEST_CASE("Dragging a free circle's rim changes its radius, not its centre", "[solver][drag]")
{
    Sketch s;
    const Id c = s.addCircle(0, 0, 10);
    Solver solver(s);
    solver.beginDragEdge(c, 10, 0);
    REQUIRE(solver.dragTo(5, 0));
    solver.endDrag();
    solver.writeBack(s);
    CHECK(s.circle(c).radius == Approx(15.0).margin(1e-5));
    const Point& centre = s.point(s.circle(c).center);
    CHECK(std::hypot(centre.x, centre.y) < 1e-4);
}

TEST_CASE("Dragging the rim of a circle with a fixed radius moves it instead", "[solver][drag]")
{
    Sketch s;
    const Id c = s.addCircle(0, 0, 10);
    s.addConstraint(ConstraintType::Radius, {c}, 10.0);
    Solver solver(s);
    solver.beginDragEdge(c, 10, 0);
    REQUIRE(solver.dragTo(5, 0));
    solver.endDrag();
    solver.writeBack(s);
    CHECK(s.circle(c).radius == Approx(10.0).margin(1e-6));
    const Point& centre = s.point(s.circle(c).center);
    CHECK(std::hypot(centre.x - 15.0, centre.y) == Approx(10.0).margin(1e-4));  // rim passes through (15, 0)
}
