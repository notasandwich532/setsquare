// .dsk file format round-trips, error handling, and the origin point.

#include <core/Geometry.h>
#include <core/Serialize.h>
#include <core/Solver.h>

#include <cmath>
#include <numbers>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

using namespace core;
using CT = ConstraintType;
using Catch::Approx;

namespace
{
Sketch sampleSketch()
{
    Sketch s;
    const Id origin = s.ensureOrigin();
    const Id corner = s.addPoint(30, 0);
    const Id l1 = s.addLine(origin, corner);
    const Id l2 = s.addLine(30, 0, 30, 20);
    const Id c = s.addCircle(10, 10, 4);
    const Id a = s.addArc(50, 0, 6, 0, 120);
    s.entity(c).construction = true;
    s.entity(s.line(l2).p2).fixed = true;
    s.addConstraint(CT::Horizontal, {l1});
    s.addConstraint(CT::Coincident, {corner, s.line(l2).p1});
    s.addConstraint(CT::Tangent, {l2, a});
    const Id d = s.addConstraint(CT::Distance, {l1}, 30.0);
    s.setConstraintLabel(d, 0.0, -8.5);
    s.addConstraint(CT::Diameter, {c}, 8.0);
    s.addConstraint(CT::Angle, {l1, l2}, 90.0);
    return s;
}
}  // namespace

TEST_CASE("Sketches round-trip through JSON unchanged", "[serialize]")
{
    const Sketch original = sampleSketch();
    const Sketch loaded = fromJson(toJson(original));

    CHECK(loaded.origin() == original.origin());
    REQUIRE(loaded.entities().size() == original.entities().size());
    for (const auto& [id, e] : original.entities()) {
        const Entity& l = loaded.entity(id);
        CHECK(l.kind() == e.kind());
        CHECK(l.construction == e.construction);
        CHECK(l.fixed == e.fixed);
        CHECK(loaded.definingPoints(id) == original.definingPoints(id));
        if (e.kind() == EntityKind::Point) {
            CHECK(loaded.point(id).x == original.point(id).x);  // exact: JSON keeps full precision
            CHECK(loaded.point(id).y == original.point(id).y);
        }
        if (e.kind() == EntityKind::Circle) {
            CHECK(loaded.circle(id).radius == original.circle(id).radius);
        }
    }
    REQUIRE(loaded.constraints().size() == original.constraints().size());
    for (const auto& [id, c] : original.constraints()) {
        const Constraint& l = loaded.constraint(id);
        CHECK(l.type == c.type);
        CHECK(l.refs == c.refs);
        CHECK(l.value == c.value);
        CHECK(l.labelX == c.labelX);
        CHECK(l.labelY == c.labelY);
    }
    CHECK(toJson(loaded) == toJson(original));  // stable output
}

TEST_CASE("A loaded sketch solves the same and keeps allocating fresh ids", "[serialize]")
{
    Sketch original = sampleSketch();
    Sketch loaded = fromJson(toJson(original));
    const SolveResult a = solve(original);
    const SolveResult b = solve(loaded);
    CHECK(a.ok == b.ok);
    CHECK(a.dof == b.dof);

    Id maxId = 0;
    for (const auto& [id, e] : loaded.entities()) {
        maxId = std::max(maxId, id);
    }
    for (const auto& [id, c] : loaded.constraints()) {
        maxId = std::max(maxId, id);
    }
    CHECK(loaded.addPoint(1, 1) > maxId);
}

TEST_CASE("Bad files are rejected with a readable error", "[serialize]")
{
    using Catch::Matchers::ContainsSubstring;
    CHECK_THROWS_WITH(fromJson("{not json"), ContainsSubstring("malformed"));
    CHECK_THROWS_WITH(fromJson(R"({"format":"something else"})"), ContainsSubstring("not a Setsquare"));
    CHECK_THROWS_WITH(fromJson(R"({"format":"setsquare.sketch","version":99,"entities":[],"constraints":[]})"),
                      ContainsSubstring("version 99"));
    CHECK_THROWS_WITH(
        fromJson(R"({"format":"setsquare.sketch","version":1,
                     "entities":[{"id":1,"type":"line","p1":7,"p2":8}],"constraints":[]})"),
        ContainsSubstring("missing point"));
    CHECK_THROWS_WITH(
        fromJson(R"({"format":"setsquare.sketch","version":1,
                     "entities":[{"id":1,"type":"point","x":0,"y":0}],
                     "constraints":[{"id":2,"type":"glue","refs":[1]}]})"),
        ContainsSubstring("unknown constraint type"));
    CHECK_THROWS_AS(fromJson(R"({"format":"setsquare.sketch","version":1,
                                 "entities":[{"id":1,"type":"point","x":0,"y":0},{"id":1,"type":"point","x":1,"y":1}],
                                 "constraints":[]})"),
                    SketchError);
}

TEST_CASE("Files saved before the app was named Setsquare still open", "[serialize]")
{
    const Sketch s = fromJson(R"({"format":"drawing_app.sketch","version":1,
                                  "entities":[{"id":1,"type":"point","x":3,"y":4}],"constraints":[]})");
    CHECK(s.hasEntity(1));
    CHECK(toJson(s).find("\"setsquare.sketch\"") != std::string::npos);  // (saved again under the new name)
}

TEST_CASE("Axes: fixed lines you can constrain and dimension against", "[origin][axes]")
{
    Sketch s;
    s.ensureOrigin();
    const Id x = s.xAxis(), y = s.yAxis();
    REQUIRE(x != kNoId);
    REQUIRE(y != kNoId);
    CHECK(s.isReference(x));
    CHECK_THROWS_AS(s.remove(x), SketchError);

    // A free line, rotated to 30 degrees from the X axis and pinned 5 from the Y axis.
    const Id l = s.addLine(2, 1, 12, 4);
    s.addConstraint(CT::Angle, {x, l}, 30.0);
    s.addConstraint(CT::Distance, {s.line(l).p1, y}, 5.0);
    REQUIRE(solve(s).ok);
    const Point& a = s.point(s.line(l).p1);
    const Point& b = s.point(s.line(l).p2);
    CHECK(std::atan2(b.y - a.y, b.x - a.x) * 180 / std::numbers::pi == Approx(30.0).margin(1e-6));
    CHECK(a.x == Approx(5.0).margin(1e-9));
    CHECK(s.point(s.line(x).p2).x == 1.0);  // the axis didn't move

    // Picking: the axis is found anywhere along it, but real geometry on it wins.
    PickOptions opt;
    opt.tolerance = 0.5;
    CHECK(pick(s, {500, 0.2}, opt) == x);
    const Id onAxis = s.addLine(100, 0, 200, 0);
    CHECK(pick(s, {150, 0.2}, opt) == onAxis);

    // Round trip keeps them.
    const Sketch loaded = fromJson(toJson(s));
    CHECK(loaded.xAxis() == x);
    CHECK(loaded.yAxis() == y);
    CHECK(loaded.isHiddenReference(s.line(x).p2));
}

TEST_CASE("The origin is a fixed point that can't be deleted", "[origin]")
{
    Sketch s;
    const Id origin = s.ensureOrigin();
    CHECK(s.ensureOrigin() == origin);  // only one
    CHECK(s.entity(origin).fixed);
    CHECK(s.point(origin).x == 0.0);

    const Id l = s.addLine(origin, s.addPoint(10, 5));
    s.remove(l);  // the line's other end goes, the origin stays
    CHECK(s.hasEntity(origin));
    CHECK(s.entities().size() == 5);  // origin + axes + their hidden ends
    CHECK_THROWS_AS(s.remove(origin), SketchError);

    // Geometry constrained to the origin moves to it; the origin doesn't move.
    const Id c = s.addCircle(3, 4, 2);
    s.addConstraint(CT::Coincident, {s.circle(c).center, origin});
    REQUIRE(solve(s).ok);
    CHECK(s.point(s.circle(c).center).x == Approx(0.0).margin(1e-9));
    CHECK(s.point(s.circle(c).center).y == Approx(0.0).margin(1e-9));
    CHECK(s.point(origin).x == 0.0);
}
