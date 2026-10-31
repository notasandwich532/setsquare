// Sketch model: construction, validation, and cascading removal.

#include <core/Sketch.h>

#include <catch2/catch_test_macros.hpp>

using namespace core;
using CT = ConstraintType;

TEST_CASE("addLine with coordinates creates two endpoints", "[sketch]")
{
    Sketch s;
    const Id l = s.addLine(0, 0, 10, 0);
    REQUIRE(s.entities().size() == 3);
    const Line& line = s.line(l);
    CHECK(s.point(line.p1).x == 0.0);
    CHECK(s.point(line.p2).x == 10.0);
}

TEST_CASE("Constraint references are validated", "[sketch]")
{
    Sketch s;
    const Id l = s.addLine(0, 0, 10, 0);
    const Id c = s.addCircle(0, 0, 5);
    const Id p = s.addPoint(3, 3);

    CHECK_THROWS_AS(s.addConstraint(CT::Parallel, {l, c}), SketchError);
    CHECK_THROWS_AS(s.addConstraint(CT::Horizontal, {c}), SketchError);
    CHECK_THROWS_AS(s.addConstraint(CT::Radius, {c}, -1.0), SketchError);
    CHECK_THROWS_AS(s.addConstraint(CT::Angle, {l, l}, 30.0), SketchError);  // not distinct
    CHECK_THROWS_AS(s.addConstraint(CT::Coincident, {p, 999}), SketchError);
    CHECK_NOTHROW(s.addConstraint(CT::Tangent, {c, l}));  // order is normalised
    CHECK_NOTHROW(s.addConstraint(CT::Coincident, {c, p}));
}

TEST_CASE("Argument order is normalised", "[sketch]")
{
    Sketch s;
    const Id l = s.addLine(0, 0, 10, 0);
    const Id c = s.addCircle(0, 5, 5);
    const Id p = s.addPoint(1, 1);
    const Id tangent = s.addConstraint(CT::Tangent, {c, l});
    const Id dist = s.addConstraint(CT::Distance, {l, p}, 2.0);
    CHECK(s.constraint(tangent).refs == std::vector<Id>{l, c});
    CHECK(s.constraint(dist).refs == std::vector<Id>{p, l});
}

TEST_CASE("Removing a line removes its unused endpoints and constraints", "[sketch]")
{
    Sketch s;
    const Id l1 = s.addLine(0, 0, 10, 0);
    const Id shared = s.line(l1).p2;
    const Id far = s.addPoint(10, 10);
    const Id l2 = s.addLine(shared, far);
    const Id h = s.addConstraint(CT::Horizontal, {l1});
    const Id v = s.addConstraint(CT::Vertical, {l2});

    s.remove(l1);

    CHECK_FALSE(s.hasEntity(l1));
    CHECK_FALSE(s.hasConstraint(h));
    CHECK(s.hasEntity(shared));  // still used by l2
    CHECK(s.hasEntity(l2));
    CHECK(s.hasConstraint(v));
    CHECK(s.entities().size() == 3);  // l2 and its two points
}

TEST_CASE("Removing a point removes curves built on it", "[sketch]")
{
    Sketch s;
    const Id c = s.addCircle(0, 0, 5);
    const Id centre = s.circle(c).center;
    s.addConstraint(CT::Radius, {c}, 5.0);
    s.remove(centre);
    CHECK(s.entities().empty());
    CHECK(s.constraints().empty());
}

TEST_CASE("Only dimensions accept values", "[sketch]")
{
    Sketch s;
    const Id l = s.addLine(0, 0, 10, 0);
    const Id h = s.addConstraint(CT::Horizontal, {l});
    const Id d = s.addConstraint(CT::Distance, {l}, 10.0);
    CHECK_THROWS_AS(s.setConstraintValue(h, 3.0), SketchError);
    s.setConstraintValue(d, 25.0);
    CHECK(s.constraint(d).value == 25.0);
}
