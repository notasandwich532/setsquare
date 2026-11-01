// Solver adapter: each constraint type, diagnosis (DOF, conflicts, redundancy,
// fully-constrained entities) and interactive dragging.

#include <core/Dimensions.h>
#include <core/Serialize.h>
#include <core/Sketch.h>
#include <core/Solver.h>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <numbers>

using namespace core;
using CT = ConstraintType;
using Catch::Approx;

namespace
{
constexpr double kTol = 1e-7;

double dist(const Sketch& s, Id a, Id b)
{
    const Point& p = s.point(a);
    const Point& q = s.point(b);
    return std::hypot(q.x - p.x, q.y - p.y);
}

double length(const Sketch& s, Id line)
{
    return dist(s, s.line(line).p1, s.line(line).p2);
}

double angleDeg(const Sketch& s, Id line)
{
    const Line& l = s.line(line);
    const Point& a = s.point(l.p1);
    const Point& b = s.point(l.p2);
    return std::atan2(b.y - a.y, b.x - a.x) * 180.0 / std::numbers::pi;
}

struct Rect
{
    Id corners[4];
    Id sides[4];  // bottom, right, top, left
};

// A closed rectangle whose sides share corner points, with H/V constraints.
Rect addRectangle(Sketch& s, double x0, double y0, double x1, double y1)
{
    Rect r;
    r.corners[0] = s.addPoint(x0, y0);
    r.corners[1] = s.addPoint(x1, y0);
    r.corners[2] = s.addPoint(x1, y1);
    r.corners[3] = s.addPoint(x0, y1);
    for (int i = 0; i < 4; ++i) {
        r.sides[i] = s.addLine(r.corners[i], r.corners[(i + 1) % 4]);
    }
    s.addConstraint(CT::Horizontal, {r.sides[0]});
    s.addConstraint(CT::Vertical, {r.sides[1]});
    s.addConstraint(CT::Horizontal, {r.sides[2]});
    s.addConstraint(CT::Vertical, {r.sides[3]});
    return r;
}
}  // namespace

TEST_CASE("Unconstrained geometry is free", "[solver]")
{
    Sketch s;
    const Id l = s.addLine(0, 0, 10, 5);
    const SolveResult r = solve(s);
    CHECK(r.ok);
    CHECK(r.dof == 4);
    CHECK(r.fullyConstrained.empty());
    CHECK(s.point(s.line(l).p2).x == Approx(10.0));  // nothing moved
}

TEST_CASE("Rectangle: H/V leave 4 DOF; fix + two dimensions fully constrain it", "[solver]")
{
    Sketch s;
    const Rect rect = addRectangle(s, 1, 2, 31, 22);
    SolveResult r = solve(s);
    REQUIRE(r.ok);
    CHECK(r.dof == 4);

    s.entity(rect.corners[0]).fixed = true;
    s.addConstraint(CT::Distance, {rect.sides[0]}, 40.0);
    s.addConstraint(CT::Distance, {rect.sides[1]}, 15.0);
    r = solve(s);
    REQUIRE(r.ok);
    CHECK(r.dof == 0);
    CHECK(r.conflicting.empty());
    CHECK(r.redundant.empty());
    for (const auto& [id, e] : s.entities()) {
        CHECK(r.fullyConstrained.contains(id));
    }
    const Point& opposite = s.point(rect.corners[2]);
    CHECK(opposite.x == Approx(41.0).margin(kTol));
    CHECK(opposite.y == Approx(17.0).margin(kTol));
}

TEST_CASE("Partially constrained sketch reports which entities are still free", "[solver]")
{
    Sketch s;
    const Id fixedLine = s.addLine(0, 0, 10, 0);
    s.entity(fixedLine).fixed = true;
    const Id loose = s.addPoint(5, 5);
    const SolveResult r = solve(s);
    REQUIRE(r.ok);
    CHECK(r.dof == 2);
    CHECK(r.fullyConstrained.contains(fixedLine));
    CHECK_FALSE(r.fullyConstrained.contains(loose));
}

TEST_CASE("Conflicting constraints are reported and geometry is left alone", "[solver]")
{
    Sketch s;
    const Id l = s.addLine(0, 0, 10, 3);
    const Id h = s.addConstraint(CT::Horizontal, {l});
    const Id v = s.addConstraint(CT::Vertical, {l});
    s.addConstraint(CT::Distance, {l}, 10.0);
    const SolveResult r = solve(s);
    CHECK_FALSE(r.ok);
    REQUIRE_FALSE(r.conflicting.empty());
    const bool namesHorV = std::ranges::find(r.conflicting, h) != r.conflicting.end()
        || std::ranges::find(r.conflicting, v) != r.conflicting.end();
    CHECK(namesHorV);
    CHECK(s.point(s.line(l).p2).y == 3.0);
}

TEST_CASE("Redundant constraints are reported but still solve", "[solver]")
{
    Sketch s;
    const Rect rect = addRectangle(s, 0, 0, 10, 5);
    const Id extra = s.addConstraint(CT::Parallel, {rect.sides[0], rect.sides[2]});
    const SolveResult r = solve(s);
    CHECK(r.ok);
    CHECK_FALSE(r.redundant.empty());
    (void)extra;
}

TEST_CASE("Coincident: point-point, point-on-line, point-on-circle", "[solver]")
{
    Sketch s;
    const Id a = s.addPoint(0, 0);
    const Id b = s.addPoint(3, 4);
    const Id l = s.addLine(-10, 1, 10, 1);
    const Id c = s.addCircle(0, 0, 20);
    const Id p = s.addPoint(5, 7);
    const Id q = s.addPoint(18, 3);
    s.addConstraint(CT::Coincident, {a, b});
    s.addConstraint(CT::Coincident, {p, l});
    s.addConstraint(CT::Coincident, {q, c});
    REQUIRE(solve(s).ok);

    CHECK(dist(s, a, b) == Approx(0.0).margin(kTol));
    const Line& line = s.line(l);
    const Point& l1 = s.point(line.p1);
    const Point& l2 = s.point(line.p2);
    const Point& pp = s.point(p);
    const double cross = (l2.x - l1.x) * (pp.y - l1.y) - (l2.y - l1.y) * (pp.x - l1.x);
    CHECK(cross == Approx(0.0).margin(1e-6));
    CHECK(dist(s, q, s.circle(c).center) == Approx(s.circle(c).radius).margin(kTol));
}

TEST_CASE("Parallel, perpendicular and equal lines", "[solver]")
{
    Sketch s;
    const Id a = s.addLine(0, 0, 10, 1);
    const Id b = s.addLine(0, 5, 8, 7);
    const Id c = s.addLine(20, 0, 22, 9);
    s.addConstraint(CT::Parallel, {a, b});
    s.addConstraint(CT::Perpendicular, {a, c});
    s.addConstraint(CT::Equal, {a, c});
    REQUIRE(solve(s).ok);

    const double aa = angleDeg(s, a);
    CHECK(std::remainder(angleDeg(s, b) - aa, 180.0) == Approx(0.0).margin(1e-6));
    CHECK(std::abs(std::remainder(angleDeg(s, c) - aa, 180.0)) == Approx(90.0).margin(1e-6));
    CHECK(length(s, a) == Approx(length(s, c)).margin(kTol));
}

TEST_CASE("Tangent line-circle and circle-circle", "[solver]")
{
    Sketch s;
    const Id c1 = s.addCircle(0, 0, 10);
    s.entity(c1).fixed = true;
    const Id l = s.addLine(-20, 12, 20, 13);
    s.addConstraint(CT::Horizontal, {l});
    s.addConstraint(CT::Tangent, {l, c1});

    const Id c2 = s.addCircle(25, 3, 8);
    s.addConstraint(CT::Tangent, {c1, c2});
    REQUIRE(solve(s).ok);

    CHECK(s.point(s.line(l).p1).y == Approx(10.0).margin(kTol));  // stays above the circle
    const double centres = dist(s, s.circle(c1).center, s.circle(c2).center);
    CHECK(centres == Approx(s.circle(c1).radius + s.circle(c2).radius).margin(kTol));
}

TEST_CASE("Midpoint, symmetric and concentric", "[solver]")
{
    Sketch s;
    const Id l = s.addLine(0, 0, 10, 0);
    const Id m = s.addPoint(4, 3);
    s.addConstraint(CT::Midpoint, {m, l});

    const Id axis = s.addLine(0, -10, 0, 10);
    const Id p = s.addPoint(-3, 2);
    const Id q = s.addPoint(5, 1);
    s.addConstraint(CT::Symmetric, {p, q, axis});

    const Id c1 = s.addCircle(30, 30, 5);
    const Id c2 = s.addCircle(33, 28, 9);
    s.addConstraint(CT::Concentric, {c1, c2});
    REQUIRE(solve(s).ok);

    const Line& line = s.line(l);
    const Point& a = s.point(line.p1);
    const Point& b = s.point(line.p2);
    CHECK(s.point(m).x == Approx((a.x + b.x) / 2).margin(kTol));
    CHECK(s.point(m).y == Approx((a.y + b.y) / 2).margin(kTol));

    const Line& ax = s.line(axis);
    const Point& ax1 = s.point(ax.p1);
    const Point& ax2 = s.point(ax.p2);
    const Point& pp = s.point(p);
    const Point& qq = s.point(q);
    // Midpoint of p-q lies on the axis and p-q is perpendicular to it.
    const double mx = (pp.x + qq.x) / 2, my = (pp.y + qq.y) / 2;
    CHECK(((ax2.x - ax1.x) * (my - ax1.y) - (ax2.y - ax1.y) * (mx - ax1.x)) == Approx(0.0).margin(1e-6));
    CHECK(((ax2.x - ax1.x) * (qq.x - pp.x) + (ax2.y - ax1.y) * (qq.y - pp.y)) == Approx(0.0).margin(1e-6));

    CHECK(dist(s, s.circle(c1).center, s.circle(c2).center) == Approx(0.0).margin(kTol));
}

TEST_CASE("Distance dimensions: point-point, point-line, horizontal, vertical", "[solver]")
{
    Sketch s;
    const Id a = s.addPoint(0, 0);
    s.entity(a).fixed = true;
    const Id b = s.addPoint(3, 4);
    s.addConstraint(CT::Distance, {a, b}, 10.0);

    const Id l = s.addLine(-50, -20, 50, -20);
    s.entity(l).fixed = true;
    const Id p = s.addPoint(7, -10);
    s.addConstraint(CT::Distance, {p, l}, 4.0);

    const Id c = s.addPoint(-5, -3);
    s.entity(c).fixed = true;
    const Id d = s.addPoint(-12, -9);
    s.addConstraint(CT::HorizontalDistance, {c, d}, 20.0);
    s.addConstraint(CT::VerticalDistance, {c, d}, 2.0);
    REQUIRE(solve(s).ok);

    CHECK(dist(s, a, b) == Approx(10.0).margin(kTol));
    CHECK(s.point(p).y == Approx(-16.0).margin(kTol));  // stays on the same side of the line
    CHECK(s.point(d).x == Approx(-25.0).margin(kTol));  // keeps sign: d was left of c
    CHECK(s.point(d).y == Approx(-5.0).margin(kTol));
}

TEST_CASE("Radius and diameter on circles and arcs", "[solver]")
{
    Sketch s;
    const Id c = s.addCircle(0, 0, 3);
    s.addConstraint(CT::Diameter, {c}, 18.0);
    const Id arc = s.addArc(20, 0, 4, 0, 90);
    s.addConstraint(CT::Radius, {arc}, 12.0);
    REQUIRE(solve(s).ok);

    CHECK(s.circle(c).radius == Approx(9.0).margin(kTol));
    const Arc& a = s.arc(arc);
    CHECK(dist(s, a.center, a.start) == Approx(12.0).margin(kTol));
    CHECK(dist(s, a.center, a.end) == Approx(12.0).margin(kTol));
}

TEST_CASE("Arc endpoints stay on the arc when one is moved by a constraint", "[solver]")
{
    Sketch s;
    const Id arc = s.addArc(0, 0, 10, 0, 90);
    const Arc& a = s.arc(arc);
    s.entity(a.center).fixed = true;
    s.addConstraint(CT::HorizontalDistance, {a.center, a.end}, 5.0);
    REQUIRE(solve(s).ok);
    CHECK(dist(s, a.center, a.start) == Approx(dist(s, a.center, a.end)).margin(kTol));
    CHECK(s.point(a.end).x == Approx(5.0).margin(kTol));
}

TEST_CASE("Angle between lines keeps rotation sense", "[solver]")
{
    Sketch s;
    const Id base = s.addLine(0, 0, 10, 0);
    s.entity(base).fixed = true;
    const Id arm = s.addLine(s.line(base).p1, s.addPoint(5, 8));
    s.addConstraint(CT::Distance, {arm}, 10.0);
    s.addConstraint(CT::Angle, {base, arm}, 30.0);
    REQUIRE(solve(s).ok);
    CHECK(angleDeg(s, arm) == Approx(30.0).margin(1e-6));
    CHECK(length(s, arm) == Approx(10.0).margin(kTol));
}

TEST_CASE("Changing a dimension value re-solves to the new size", "[solver]")
{
    Sketch s;
    const Id l = s.addLine(0, 0, 10, 0);
    s.entity(s.line(l).p1).fixed = true;
    s.addConstraint(CT::Horizontal, {l});
    const Id d = s.addConstraint(CT::Distance, {l}, 10.0);
    REQUIRE(solve(s).ok);
    s.setConstraintValue(d, 42.5);
    REQUIRE(solve(s).ok);
    CHECK(s.point(s.line(l).p2).x == Approx(42.5).margin(kTol));
}

TEST_CASE("Dragging an endpoint of a pinned fixed-length line rotates it", "[solver][drag]")
{
    Sketch s;
    const Id l = s.addLine(0, 0, 10, 0);
    const Id pivot = s.line(l).p1;
    const Id tip = s.line(l).p2;
    s.entity(pivot).fixed = true;
    s.addConstraint(CT::Distance, {l}, 10.0);

    Solver solver(s);
    solver.beginDrag({tip});
    REQUIRE(solver.dragTo(-10.0, 10.0));  // pull towards (0, 10)
    solver.endDrag();
    solver.writeBack(s);

    CHECK(length(s, l) == Approx(10.0).margin(kTol));
    CHECK(s.point(tip).x == Approx(0.0).margin(1e-5));
    CHECK(s.point(tip).y == Approx(10.0).margin(1e-5));
    CHECK(s.point(pivot).x == 0.0);
}

TEST_CASE("Dragging a free line moves both endpoints", "[solver][drag]")
{
    Sketch s;
    const Id l = s.addLine(0, 0, 10, 0);
    s.addConstraint(CT::Horizontal, {l});
    Solver solver(s);
    solver.beginDrag(s.definingPoints(l));
    REQUIRE(solver.dragTo(5.0, 7.0));
    REQUIRE(solver.dragTo(6.0, 8.0));  // successive moves are relative to the drag start
    solver.endDrag();
    solver.writeBack(s);
    CHECK(s.point(s.line(l).p1).x == Approx(6.0).margin(1e-6));
    CHECK(s.point(s.line(l).p1).y == Approx(8.0).margin(1e-6));
    CHECK(s.point(s.line(l).p2).x == Approx(16.0).margin(1e-6));

    // A normal solve after dragging still works and diagnoses correctly.
    const SolveResult r = solver.solve();
    CHECK(r.ok);
    CHECK(r.dof == 3);
}

TEST_CASE("Ratio: one line k times another's length, or one radius k times another's", "[solver][ratio]")
{
    Sketch s;
    const Id a = s.addLine(0, 0, 30, 0);
    const Id b = s.addLine(0, 10, 20, 10);
    for (Id l : {a, b}) {
        s.entity(s.line(l).p1).fixed = true;
        s.addConstraint(ConstraintType::Horizontal, {l});
    }
    s.addConstraint(ConstraintType::Distance, {b}, 12.0);
    CHECK(measure(s, ConstraintType::Ratio, {a, b}) == Catch::Approx(1.5));
    const Id k = s.addConstraint(ConstraintType::Ratio, {a, b}, 1.5);
    SolveResult r = solve(s);
    CHECK(r.ok);
    CHECK(r.dof == 0);
    CHECK(r.redundant.empty());
    CHECK(s.point(s.line(a).p2).x == Catch::Approx(18.0).margin(1e-7));

    s.setConstraintValue(k, 2.5);  // changed afterwards
    REQUIRE(solve(s).ok);
    CHECK(s.point(s.line(a).p2).x == Catch::Approx(30.0).margin(1e-7));

    // Radii.
    const Id big = s.addCircle(100, 0, 9), small = s.addCircle(130, 0, 2);
    s.addConstraint(ConstraintType::Radius, {small}, 2.0);
    s.addConstraint(ConstraintType::Ratio, {big, small}, 3.0);
    REQUIRE(solve(s).ok);
    CHECK(s.circle(big).radius == Catch::Approx(6.0).margin(1e-7));

    // Saved and loaded with its value; only lines with lines, rounds with rounds, and positive.
    Sketch back = fromJson(toJson(s));
    CHECK(back.constraint(k).value == Catch::Approx(2.5));
    CHECK_THROWS_AS(s.addConstraint(ConstraintType::Ratio, {a, big}, 2.0), SketchError);
    CHECK_THROWS_AS(s.addConstraint(ConstraintType::Ratio, {a, b}, 0.0), SketchError);
    CHECK_THROWS_AS(s.setConstraintValue(k, -1.0), SketchError);
}