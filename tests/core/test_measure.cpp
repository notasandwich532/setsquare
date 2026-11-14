// Measuring: curve lengths (exact, not from a polyline), point separations and closed areas.

#include <core/Builders.h>
#include <core/Measure.h>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <numbers>

using namespace core;
using Catch::Approx;

namespace
{
constexpr double pi = std::numbers::pi;

build::Rectangle rect(Sketch& s, double x0, double y0, double x1, double y1)
{
    return build::rectangle(s, s.addPoint(x0, y0), s.addPoint(x1, y1));
}
}  // namespace

TEST_CASE("Curve lengths are exact", "[measure]")
{
    Sketch s;
    const Id line = s.addLine(s.addPoint(1, 1), s.addPoint(4, 5));
    CHECK(measuring::length(s, {line}) == Approx(5.0).epsilon(1e-12));

    const Id circle = s.addCircle(0, 0, 2);
    CHECK(measuring::length(s, {circle}) == Approx(4 * pi).epsilon(1e-12));

    const Id arc = s.addArc(0, 0, 3, 0, 90);  // a quarter
    CHECK(measuring::length(s, {arc}) == Approx(1.5 * pi).epsilon(1e-12));

    // An ellipse 5 by 3 (semi-axes): its perimeter, to 12 figures.
    const Id c = s.addPoint(20, 0);
    const Id ellipse = s.addEllipse(c, s.addPoint(25, 0), s.addPoint(20, 3));
    CHECK(measuring::length(s, {ellipse}) == Approx(25.5269988633981).epsilon(1e-11));

    // A spline through points in a row is straight.
    Spline sp;
    sp.points = {s.addPoint(0, 10), s.addPoint(2, 10), s.addPoint(4, 10), s.addPoint(6, 10)};
    const Id spline = s.addSpline(sp);
    CHECK(measuring::length(s, {spline}) == Approx(6.0).epsilon(1e-9));

    // Several: their total; points add nothing.
    CHECK(measuring::length(s, {line, circle, c}) == Approx(5.0 + 4 * pi).epsilon(1e-12));
}

TEST_CASE("Two points: the distance and the differences in x and y", "[measure]")
{
    Sketch s;
    const Id a = s.addPoint(10, 20);
    const Id b = s.addPoint(7, 24);
    const measuring::Separation d = measuring::between(s, a, b);
    CHECK(d.distance == Approx(5.0));
    CHECK(d.dx == Approx(-3.0));  // (b - a)
    CHECK(d.dy == Approx(4.0));
}

TEST_CASE("Areas: inside the outline, less the holes, close to exact for curved ones", "[measure]")
{
    Sketch s;
    rect(s, 0, 0, 20, 10);
    s.addCircle(5, 5, 2);  // a hole in the rectangle
    const auto around = measuring::areaAt(s, s.activeLayer(), {15, 5});
    REQUIRE(around);
    CHECK(measuring::area(*around) == Approx(200 - 4 * pi).epsilon(1e-5));

    const auto inside = measuring::areaAt(s, s.activeLayer(), {5, 5});  // the circle's own
    REQUIRE(inside);
    CHECK(measuring::area(*inside) == Approx(4 * pi).epsilon(1e-5));

    CHECK_FALSE(measuring::areaAt(s, s.activeLayer(), {50, 50}));  // (not enclosed)
}
