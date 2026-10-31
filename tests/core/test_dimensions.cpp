// Dimension measurement and label layout; constraint pre-validation.

#include <core/Dimensions.h>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

using namespace core;
using CT = ConstraintType;
using Catch::Approx;

TEST_CASE("measure reports current sizes", "[dimensions]")
{
    Sketch s;
    const Id l = s.addLine(0, 0, 30, 40);
    const Id p = s.addPoint(0, 10);
    const Id h = s.addLine(-10, 0, 10, 0);
    const Id c = s.addCircle(0, 0, 7);
    const Id a = s.addArc(0, 0, 4, 0, 90);
    const Id v = s.addLine(0, 0, 0, 10);

    CHECK(measure(s, CT::Distance, {l}) == Approx(50.0));
    CHECK(measure(s, CT::HorizontalDistance, {l}) == Approx(30.0));
    CHECK(measure(s, CT::VerticalDistance, {l}) == Approx(40.0));
    CHECK(measure(s, CT::Distance, {p, h}) == Approx(10.0));  // point to line
    CHECK(measure(s, CT::Radius, {c}) == Approx(7.0));
    CHECK(measure(s, CT::Diameter, {c}) == Approx(14.0));
    CHECK(measure(s, CT::Radius, {a}) == Approx(4.0));
    CHECK(measure(s, CT::Angle, {h, v}) == Approx(90.0));
    CHECK(measure(s, CT::Angle, {h, l}) == Approx(53.130102354).epsilon(1e-9));
}

TEST_CASE("A dimension created at its measured value doesn't move anything", "[dimensions]")
{
    Sketch s;
    const Id l = s.addLine(1, 2, 31, 42);
    const double len = measure(s, CT::Distance, {l});
    s.addConstraint(CT::Distance, {l}, len);
    const Point before = s.point(s.line(l).p2);
    // Measuring again gives the same value; nothing to solve.
    CHECK(measure(s, CT::Distance, {l}) == Approx(len));
    CHECK(before.x == 31.0);
}

TEST_CASE("Label offsets round-trip through layoutDimension", "[dimensions]")
{
    Sketch s;
    const Id l = s.addLine(0, 0, 20, 0);
    const Id c = s.addCircle(50, 50, 5);
    const Id l2 = s.addLine(0, 0, 0, 20);
    for (const auto& [type, refs] : std::vector<std::pair<CT, std::vector<Id>>>{
             {CT::Distance, {l}}, {CT::Radius, {c}}, {CT::Angle, {l, l2}}}) {
        const Id d = s.addConstraint(type, refs, measure(s, type, refs));
        const Vec2 want{12.5, -7.25};
        const Vec2 off = labelOffsetFor(s, s.constraint(d), want);
        s.setConstraintLabel(d, off.x, off.y);
        const DimensionLayout L = layoutDimension(s, s.constraint(d));
        REQUIRE(L.valid);
        CHECK(L.label.x == Approx(want.x));
        CHECK(L.label.y == Approx(want.y));
    }
}

TEST_CASE("Linear layout measures between the right points", "[dimensions]")
{
    Sketch s;
    const Id p = s.addPoint(5, 10);
    const Id l = s.addLine(0, 0, 20, 0);
    const Id d = s.addConstraint(CT::Distance, {p, l}, 10.0);
    const DimensionLayout L = layoutDimension(s, s.constraint(d));
    REQUIRE(L.valid);
    CHECK(L.a.x == Approx(5.0));
    CHECK(L.a.y == Approx(10.0));
    CHECK(L.b.x == Approx(5.0));  // foot of the perpendicular
    CHECK(L.b.y == Approx(0.0).margin(1e-12));
}

TEST_CASE("Angle layout of parallel lines is invalid", "[dimensions]")
{
    Sketch s;
    const Id a = s.addLine(0, 0, 10, 0);
    const Id b = s.addLine(0, 5, 10, 5);
    Constraint c{99, CT::Angle, {a, b}, 0.0};
    CHECK_FALSE(layoutDimension(s, c).valid);
}

TEST_CASE("canAddConstraint checks without adding", "[sketch]")
{
    Sketch s;
    const Id l = s.addLine(0, 0, 10, 0);
    const Id p = s.addPoint(1, 1);
    const Id q = s.addPoint(2, 2);
    std::string why;
    CHECK(s.canAddConstraint(CT::Horizontal, {l}));
    CHECK_FALSE(s.canAddConstraint(CT::Horizontal, {p}, &why));
    CHECK_FALSE(why.empty());
    CHECK(s.canAddConstraint(CT::Symmetric, {l, p, q}));  // any click order
    CHECK(s.constraints().empty());

    const Id sym = s.addConstraint(CT::Symmetric, {p, l, q});
    CHECK(s.constraint(sym).refs == std::vector<Id>{p, q, l});
}
