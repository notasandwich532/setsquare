// Trim, extend, break, fillet and chamfer.

#include <core/Builders.h>
#include <core/Modify.h>
#include <core/Solver.h>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cmath>
#include <numbers>

using namespace core;
using CT = ConstraintType;
using Catch::Approx;
namespace mod = core::modify;

namespace
{
Vec2 pos(const Sketch& s, Id p)
{
    return {s.point(p).x, s.point(p).y};
}
double dist(Vec2 a, Vec2 b)
{
    return std::hypot(b.x - a.x, b.y - a.y);
}
size_t countKind(const Sketch& s, EntityKind kind, bool construction)
{
    return static_cast<size_t>(std::ranges::count_if(s.entities(), [&](const auto& kv) {
        return kv.second.kind() == kind && kv.second.construction == construction;
    }));
}
void requireClean(Sketch& s, int dof)
{
    const SolveResult r = solve(s);
    CHECK(r.ok);
    CHECK(r.redundant.empty());
    CHECK(r.conflicting.empty());
    CHECK(r.dof == dof);
}
}  // namespace

TEST_CASE("Intersections respect each curve's extent", "[modify]")
{
    Sketch s;
    const Id h = s.addLine(-10, 0, 10, 0);
    const Id v = s.addLine(3, -10, 3, 10);
    const Id shortV = s.addLine(30, 1, 30, 10);  // its line would cross h's extension only
    const Id c = s.addCircle(0, 0, 5);
    const Id arc = s.addArc(0, 0, 5, 0, 90);  // the upper-right quarter

    REQUIRE(mod::intersections(s, h, v).size() == 1);
    CHECK(mod::intersections(s, h, v)[0].x == Approx(3.0));
    CHECK(mod::intersections(s, h, shortV).empty());
    CHECK(mod::intersections(s, h, c).size() == 2);
    CHECK(mod::intersections(s, v, c).size() == 2);
    CHECK(mod::intersections(s, v, arc).size() == 1);  // only the upper crossing is on the arc
}

TEST_CASE("Trim the end of a line back to where it crosses another", "[modify][trim]")
{
    Sketch s;
    const Id h = s.addLine(0, 0, 20, 0);
    const Id v = s.addLine(10, -5, 10, 5);
    mod::trim(s, h, {15, 0});
    REQUIRE(s.hasEntity(h));
    const Line l = s.line(h);
    CHECK(pos(s, l.p1).x == Approx(0.0));
    CHECK(pos(s, l.p2).x == Approx(10.0));
    // The new end stays on the vertical line.
    REQUIRE(s.constraints().size() == 1);
    CHECK(s.constraints().begin()->second.refs[1] == v);
    CHECK(countKind(s, EntityKind::Point, false) == 4);  // the old end is gone
    requireClean(s, 8 - 1);
}

TEST_CASE("Trim the middle of a line leaves two collinear pieces", "[modify][trim]")
{
    Sketch s;
    const Id h = s.addLine(0, 0, 30, 0);
    s.addLine(10, -5, 10, 5);
    s.addLine(20, -5, 20, 5);
    mod::trim(s, h, {15, 0});
    CHECK(countKind(s, EntityKind::Line, false) == 4);
    const Line l = s.line(h);
    CHECK(pos(s, l.p2).x == Approx(10.0));
    bool tailFound = false;
    for (const auto& [id, e] : s.entities()) {
        if (e.kind() == EntityKind::Line && id != h && std::abs(pos(s, s.line(id).p1).y) < 1e-9
            && std::abs(pos(s, s.line(id).p2).y) < 1e-9) {
            tailFound = true;
            CHECK(pos(s, s.line(id).p1).x == Approx(20.0));
        }
    }
    CHECK(tailFound);
    const auto collinear = std::ranges::count_if(s.constraints(), [](const auto& kv) { return kv.second.type == CT::Collinear; });
    CHECK(collinear == 1);
    REQUIRE(solve(s).ok);
}

TEST_CASE("Trim to construction keeps the piece, dashed and still joined", "[modify][trim]")
{
    Sketch s;
    const Id h = s.addLine(0, 0, 30, 0);
    s.addLine(10, -5, 10, 5);
    s.addLine(20, -5, 20, 5);
    const Id piece = mod::trimToConstruction(s, h, {15, 0});
    CHECK(s.entity(piece).construction);
    CHECK(countKind(s, EntityKind::Line, true) == 1);
    CHECK(countKind(s, EntityKind::Line, false) == 4);  // h's two ends + the verticals
    const Line l = s.line(piece);
    CHECK(pos(s, l.p1).x == Approx(10.0));
    CHECK(pos(s, l.p2).x == Approx(20.0));
    requireClean(s, 16 - 2 - 2);  // 8 points; pieces collinear (2), cut points on the verticals (2)

    // A circle: the clicked part becomes a construction arc, the rest a solid arc.
    Sketch t;
    const Id c = t.addCircle(0, 0, 10);
    t.addLine(-20, 0, 20, 0);
    mod::trimToConstruction(t, c, {0, -10});
    CHECK(countKind(t, EntityKind::Arc, true) == 1);
    CHECK(countKind(t, EntityKind::Arc, false) == 1);

    // Nothing crossing: the whole curve.
    Sketch u;
    const Id lone = u.addLine(0, 0, 10, 0);
    CHECK(mod::trimToConstruction(u, lone, {5, 0}) == lone);
    CHECK(u.entity(lone).construction);
}

TEST_CASE("Equal lines stay whole when part of one is trimmed to construction", "[modify][trim]")
{
    // A tapered hole: equal sides, a level bottom, the tops level; a rib crosses the left side.
    Sketch s;
    const Id a = s.addPoint(-69, 0), b = s.addPoint(-65, -26), c = s.addPoint(-37, -26), d = s.addPoint(-33, 0);
    const Id left = s.addLine(a, b), bottom = s.addLine(b, c), right = s.addLine(c, d);
    s.addConstraint(ConstraintType::Horizontal, {bottom});
    s.addConstraint(ConstraintType::Horizontal, {a, d});
    s.addConstraint(ConstraintType::Equal, {left, right});
    const Id rib = s.addLine(-80, -10, -60, -10);
    s.entity(s.line(rib).p1).fixed = s.entity(s.line(rib).p2).fixed = true;
    s.entity(a).fixed = s.entity(b).fixed = true;
    REQUIRE(solve(s).ok);
    auto length = [&](Id p, Id q) { return std::hypot(pos(s, p).x - pos(s, q).x, pos(s, p).y - pos(s, q).y); };
    const double side = length(a, b);
    const Vec2 was = pos(s, d);

    // The top of the left side, above the rib, to construction: the sides are still equal as
    // drawn, so nothing moves (it used to make the right side as short as the piece).
    const Id piece = mod::trimToConstruction(s, left, {-68.5, -3});
    REQUIRE(s.entity(piece).construction);
    CHECK(mod::wholeLine(s, piece) == std::pair{a, b});
    requireClean(s, 1);  // (the bottom's width is free)
    CHECK(length(c, d) == Approx(side));
    CHECK(pos(s, d).x == Approx(was.x).margin(1e-6));
    CHECK(pos(s, d).y == Approx(was.y).margin(1e-6));

    // A middle piece too: solid, construction, solid make one line.
    Sketch t;
    const Id h = t.addLine(0, 0, 30, 0);
    t.addLine(10, -5, 10, 5);
    t.addLine(20, -5, 20, 5);
    mod::trimToConstruction(t, h, {15, 0});
    const auto [e1, e2] = mod::wholeLine(t, h);
    CHECK(std::abs(pos(t, e1).x - pos(t, e2).x) == Approx(30.0));

    // Pieces only broken apart (no construction between) stay lines of their own.
    Sketch u;
    const Id g = u.addLine(0, 0, 30, 0);
    u.addLine(10, -5, 10, 5);
    const std::vector<Id> pieces = mod::breakCurve(u, g, {5, 0});
    REQUIRE(pieces.size() == 2);
    const auto [f1, f2] = mod::wholeLine(u, pieces[0]);
    CHECK(std::abs(pos(u, f1).x - pos(u, f2).x) == Approx(10.0));
}

TEST_CASE("Trim a curve that crosses nothing removes it", "[modify][trim]")
{
    Sketch s;
    const Id l = s.addLine(0, 0, 10, 0);
    mod::trim(s, l, {5, 0});
    CHECK(s.entities().empty());
}

TEST_CASE("Trim a circle leaves an arc that keeps its constraints", "[modify][trim]")
{
    Sketch s;
    const Id c = s.addCircle(0, 0, 10);
    s.addConstraint(CT::Radius, {c}, 10.0);
    s.addLine(-20, 0, 20, 0);
    mod::trim(s, c, {0, -10});  // remove the lower half
    CHECK_FALSE(s.hasEntity(c));
    REQUIRE(countKind(s, EntityKind::Arc, false) == 1);
    Id arc = kNoId;
    for (const auto& [id, e] : s.entities()) {
        if (e.kind() == EntityKind::Arc) {
            arc = id;
        }
    }
    const ArcShape a = arcShape(s, arc);
    CHECK(a.sweep == Approx(std::numbers::pi));
    CHECK(std::sin(a.startAngle + a.sweep / 2) > 0.99);  // the upper half
    const auto radius = std::ranges::find_if(s.constraints(), [](const auto& kv) { return kv.second.type == CT::Radius; });
    REQUIRE(radius != s.constraints().end());
    CHECK(radius->second.refs[0] == arc);
    REQUIRE(solve(s).ok);
}

TEST_CASE("Trim the middle of an arc", "[modify][trim]")
{
    Sketch s;
    const Id arc = s.addArc(0, 0, 10, 0, 180);
    s.addLine(5, 0, 5, 20);
    s.addLine(-5, 0, -5, 20);
    mod::trim(s, arc, {0, 10});
    CHECK(countKind(s, EntityKind::Arc, false) == 2);
    REQUIRE(solve(s).ok);
    CHECK(solve(s).redundant.empty());
}

TEST_CASE("Extend a line to the next curve in its path", "[modify][extend]")
{
    Sketch s;
    const Id h = s.addLine(0, 0, 10, 0);
    const Id v = s.addLine(25, -5, 25, 5);
    s.addLine(40, -5, 40, 5);  // further away
    mod::extend(s, h, {9, 0});
    CHECK(pos(s, s.line(h).p2).x == Approx(25.0));
    CHECK(s.constraints().begin()->second.refs[1] == v);
    CHECK_THROWS_AS(mod::extend(s, h, {1, 0}), SketchError);  // nothing behind the start
}

TEST_CASE("Extend an arc round to a line", "[modify][extend]")
{
    Sketch s;
    const Id arc = s.addArc(0, 0, 10, 0, 45);
    s.addLine(-20, 5, 20, 5);  // crosses the circle at about 30 and 150 degrees... 30 is on the arc
    s.addLine(-5, -20, -5, 20);  // crosses the upper half at ~120 degrees
    mod::extend(s, arc, {7.07, 7.07});  // the end (at 45 degrees)
    const ArcShape a = arcShape(s, arc);
    CHECK(a.sweep * 180 / std::numbers::pi == Approx(120.0).margin(1e-6));
}

TEST_CASE("Break splits at the crossings around the click", "[modify][break]")
{
    Sketch s;
    const Id h = s.addLine(0, 0, 30, 0);
    s.addLine(10, -5, 10, 5);
    s.addLine(20, -5, 20, 5);
    const auto pieces = mod::breakCurve(s, h, {15, 0});
    CHECK(pieces.size() == 3);
    CHECK(countKind(s, EntityKind::Line, false) == 5);
    REQUIRE(solve(s).ok);
    CHECK(solve(s).redundant.empty());

    Sketch t;
    const Id c = t.addCircle(0, 0, 10);
    t.addLine(-20, 0, 20, 0);
    const auto arcs = mod::breakCurve(t, c, {0, 10});
    CHECK(arcs.size() == 2);
    CHECK(countKind(t, EntityKind::Arc, false) == 2);
    REQUIRE(solve(t).ok);
}

TEST_CASE("Fillet a rectangle corner: construction corner kept, DOF unchanged", "[modify][fillet]")
{
    Sketch s;
    const Id a = s.addPoint(0, 0), b = s.addPoint(40, 30);
    const build::Rectangle r = build::rectangle(s, a, b);
    s.entity(a).fixed = s.entity(b).fixed = true;  // pin it, so only the fillet can change
    requireClean(s, 0);
    const Id corner = r.corners[1];  // bottom right
    Id ca, cb;
    Vec2 na, nb;
    REQUIRE(mod::cornerAt(s, corner, ca, na, cb, nb));
    const mod::Corner f = mod::fillet(s, ca, na, cb, nb, 5.0);

    requireClean(s, 0);  // the radius is dimensioned, everything else as before
    CHECK(s.entity(r.sides[0]).construction);
    CHECK(s.entity(r.sides[1]).construction);
    CHECK(s.hasEntity(corner));  // the sharp corner survives for dimensions
    CHECK(s.arcRadius(f.joint) == Approx(5.0));
    const ArcShape arc = arcShape(s, f.joint);
    CHECK(arc.center.x == Approx(35.0).margin(1e-6));
    CHECK(arc.center.y == Approx(5.0).margin(1e-6));
    CHECK(arc.sweep * 180 / std::numbers::pi == Approx(90.0).margin(1e-6));

    // The radius dimension drives it.
    s.setConstraintValue(f.dimensions[0], 8.0);
    REQUIRE(solve(s).ok);
    CHECK(s.arcRadius(f.joint) == Approx(8.0).margin(1e-6));
    CHECK(arcShape(s, f.joint).center.x == Approx(32.0).margin(1e-6));
}

TEST_CASE("Fillet two crossing lines in the clicked quadrant", "[modify][fillet]")
{
    Sketch s;
    const Id h = s.addLine(-20, 0, 20, 0);
    const Id v = s.addLine(0, -20, 0, 20);
    s.entity(h).fixed = s.entity(v).fixed = true;
    const mod::Corner f = mod::fillet(s, h, {-10, 0}, v, {0, 10}, 4.0);
    requireClean(s, 0);
    const ArcShape arc = arcShape(s, f.joint);
    CHECK(arc.center.x == Approx(-4.0).margin(1e-6));  // the upper-left quadrant
    CHECK(arc.center.y == Approx(4.0).margin(1e-6));
}

TEST_CASE("Fillet a sharp corner picked close in, nearer than the arc will touch", "[modify][fillet]")
{
    // A 45 degree corner: a 5 mm fillet touches 12 mm from it. Picked 6 mm out, the wide angle
    // next to it has an arc touching nearer the picks (though past the corner on one line);
    // the fillet must still go in the corner picked.
    Sketch s;
    const Id base = s.addLine(0, 0, 30, 0);
    const Id slope = s.addLine(30, 0, 0, 30);
    s.entity(s.line(base).p1).fixed = true;
    const double k = 6.0 / std::sqrt(2.0);
    const mod::Corner f = mod::fillet(s, base, {24, 0}, slope, {30 - k, k}, 5.0);
    const ArcShape arc = arcShape(s, f.joint);
    const double reach = 5.0 / std::tan(std::numbers::pi / 8);  // corner to touching point
    CHECK(arc.center.x == Approx(30 - reach).margin(1e-6));
    CHECK(arc.center.y == Approx(5.0).margin(1e-6));
    CHECK(arc.sweep == Approx(3 * std::numbers::pi / 4).margin(1e-6));
}

TEST_CASE("Fillet a line and an arc", "[modify][fillet]")
{
    Sketch s;
    const Id l = s.addLine(-30, 0, 0, 0);
    const Id arc = s.addArc(10, 0, 10, 90, 180);  // upper-left quarter of a circle at (10,0): meets l at (0,0)
    s.entity(l).fixed = true;
    s.entity(s.arc(arc).center).fixed = true;
    const mod::Corner f = mod::fillet(s, l, {-20, 0}, arc, {10 - 7.07, 7.07}, 3.0);
    const SolveResult r = solve(s);
    CHECK(r.ok);
    CHECK(r.redundant.empty());
    CHECK(s.arcRadius(f.joint) == Approx(3.0).margin(1e-6));
    // The fillet touches the arc's circle: centre distance = 10 + 3 (outside) or 10 - 3.
    const double d = dist(arcShape(s, f.joint).center, {10, 0});
    CHECK((std::abs(d - 13.0) < 1e-6 || std::abs(d - 7.0) < 1e-6));
}

TEST_CASE("Chamfer: equal, two distances, distance and angle", "[modify][chamfer]")
{
    for (auto kind : {mod::ChamferKind::EqualDistance, mod::ChamferKind::TwoDistances, mod::ChamferKind::DistanceAngle}) {
        Sketch s;
        const build::Rectangle r = build::rectangle(s, s.addPoint(0, 0), s.addPoint(40, 30));
        const Id corner = r.corners[2];  // top right (40, 30)
        Id a, b;
        Vec2 na, nb;
        REQUIRE(mod::cornerAt(s, corner, a, na, b, nb));
        const double v2 = kind == mod::ChamferKind::DistanceAngle ? 30.0 : 8.0;
        const mod::Corner c = mod::chamfer(s, a, na, b, nb, kind, 5.0, v2);
        requireClean(s, 4);
        const Line cut = s.line(c.joint);
        const double d1 = dist(pos(s, cut.p1), {40, 30});
        const double d2 = dist(pos(s, cut.p2), {40, 30});
        CHECK(d1 == Approx(5.0).margin(1e-6));
        if (kind == mod::ChamferKind::EqualDistance) {
            CHECK(d2 == Approx(5.0).margin(1e-6));
            // One dimension, which sets both legs.
            REQUIRE(c.dimensions.size() == 1);
            s.setConstraintValue(c.dimensions[0], 7.0);
            REQUIRE(solve(s).ok);
            // (from the corner, wherever the rectangle has gone: it's free to move)
            CHECK(dist(pos(s, cut.p1), pos(s, corner)) == Approx(7.0).margin(1e-6));
            CHECK(dist(pos(s, cut.p2), pos(s, corner)) == Approx(7.0).margin(1e-6));
            requireClean(s, 4);
        }
        else if (kind == mod::ChamferKind::TwoDistances) {
            CHECK(d2 == Approx(8.0).margin(1e-6));
        }
        else {
            // Right-angled corner: tan(30) = d2 / d1.
            CHECK(d2 == Approx(5.0 * std::tan(30.0 * std::numbers::pi / 180)).margin(1e-6));
        }
    }
}

TEST_CASE("Fillet/chamfer refuse what can't fit", "[modify]")
{
    Sketch s;
    const Id a = s.addLine(0, 0, 10, 0);
    const Id b = s.addLine(0, 0, 0, 10);
    CHECK_THROWS_AS(mod::fillet(s, a, {5, 0}, b, {0, 5}, 50.0), SketchError);
    CHECK_THROWS_AS(mod::chamfer(s, a, {5, 0}, b, {0, 5}, mod::ChamferKind::EqualDistance, 20.0), SketchError);
    const Id p = s.addLine(0, 5, 10, 5);
    CHECK_THROWS_AS(mod::chamfer(s, a, {5, 0}, p, {5, 5}, mod::ChamferKind::EqualDistance, 1.0), SketchError);
}

TEST_CASE("Filleting every corner of a polygon leaves one solid piece per side", "[modify][fillet]")
{
    Sketch s;
    const auto poly = build::polygon(s, s.addPoint(0, 0), {0, 50}, 5, build::PolygonKind::Inscribed);
    const int dof = solve(s).dof;
    for (int corner = 0; corner < 5; ++corner) {
        // Each corner point where two solid sides (or pieces of them) meet.
        Id point = kNoId, a = kNoId, b = kNoId;
        Vec2 na, nb;
        for (const auto& [id, e] : s.entities()) {
            if (e.kind() == EntityKind::Point && modify::cornerAt(s, id, a, na, b, nb)) {
                point = id;
                break;
            }
        }
        REQUIRE(point != kNoId);
        modify::fillet(s, a, na, b, nb, 4);
        REQUIRE(solve(s).ok);
    }
    size_t solid = 0, construction = 0;
    for (const auto& [id, e] : s.entities()) {
        if (e.kind() == EntityKind::Line && !s.isReference(id)) {
            ++(e.construction ? construction : solid);
        }
    }
    CHECK(solid == 5);
    CHECK(construction == 5);  // the original sides, once each
    const SolveResult r = solve(s);
    CHECK(r.redundant.empty());
    CHECK(r.dof == dof);
    (void)poly;
}
