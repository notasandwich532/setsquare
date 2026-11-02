// Closed areas (for fills) and fills following their geometry.

#include <core/Builders.h>
#include <core/Modify.h>
#include <core/Regions.h>
#include <core/Serialize.h>
#include <core/Transform.h>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <numbers>

using namespace core;
using Catch::Approx;
using regions::Arrangement;

namespace
{
std::vector<Id> sides(const build::Rectangle& r)
{
    return {r.sides.begin(), r.sides.end()};
}

std::vector<Id> sorted(std::vector<Id> ids)
{
    std::ranges::sort(ids);
    return ids;
}

build::Rectangle rect(Sketch& s, double x0, double y0, double x1, double y1)
{
    const Id a = s.addPoint(x0, y0);
    return build::rectangle(s, a, s.addPoint(x1, y1));
}
}  // namespace

TEST_CASE("A rectangle encloses its inside", "[regions]")
{
    Sketch s;
    const auto r = rect(s, 0, 0, 20, 10);
    const Arrangement a(s, s.activeLayer());
    const auto inside = a.regionAt({5, 5});
    REQUIRE(inside);
    CHECK(inside->area == Approx(200.0));
    CHECK(inside->boundary == sorted(sides(r)));
    CHECK(inside->holes.empty());
    CHECK_FALSE(a.regionAt({25, 5}));
    CHECK(a.looseEnds().empty());
}

TEST_CASE("Islands are holes; clicking in one fills the island", "[regions]")
{
    Sketch s;
    rect(s, 0, 0, 40, 40);
    const Id c = s.addCircle(20, 20, 5);
    const Arrangement a(s, s.activeLayer());
    const auto ring = a.regionAt({5, 5});
    REQUIRE(ring);
    REQUIRE(ring->holes.size() == 1);
    CHECK_FALSE(ring->contains({20, 20}));
    CHECK(ring->contains({5, 5}));
    const auto disc = a.regionAt({20, 20});
    REQUIRE(disc);
    CHECK(disc->boundary == std::vector<Id>{c});
    CHECK(disc->area == Approx(std::numbers::pi * 25).epsilon(0.01));
}

TEST_CASE("Crossing curves cut areas", "[regions]")
{
    Sketch s;
    rect(s, 0, 0, 20, 20);
    rect(s, 10, 10, 30, 30);
    const Arrangement a(s, s.activeLayer());
    const auto overlap = a.regionAt({15, 15});
    REQUIRE(overlap);
    CHECK(overlap->area == Approx(100.0));
    CHECK(overlap->boundary.size() == 4);  // two sides of each square
    const auto corner = a.regionAt({5, 5});
    REQUIRE(corner);
    CHECK(corner->area == Approx(300.0));  // the L-shape left of the overlap
}

TEST_CASE("A line across splits an area; a stub doesn't", "[regions]")
{
    Sketch s;
    const auto r = rect(s, 0, 0, 20, 10);
    SECTION("across")
    {
        s.addLine(10, -5, 10, 15);
        const Arrangement a(s, s.activeLayer());
        REQUIRE(a.regionAt({5, 5}));
        CHECK(a.regionAt({5, 5})->area == Approx(100.0));
        CHECK(a.looseEnds().size() == 2);  // the bits sticking out
    }
    SECTION("stub inside")
    {
        const Id stub = s.addLine(10, 5, 10, 10.0);  // from inside to the top side
        const Arrangement a(s, s.activeLayer());
        const auto inside = a.regionAt({5, 5});
        REQUIRE(inside);
        CHECK(inside->area == Approx(200.0));
        CHECK(std::ranges::find(inside->boundary, stub) == inside->boundary.end());
        CHECK(a.looseEnds().size() == 1);
    }
    (void)r;
}

TEST_CASE("Open outlines enclose nothing and show their loose ends", "[regions]")
{
    Sketch s;
    const auto r = rect(s, 0, 0, 20, 10);
    s.remove(r.sides[3]);  // no left side
    const Arrangement a(s, s.activeLayer());
    CHECK_FALSE(a.regionAt({5, 5}));
    CHECK(a.looseEnds().size() == 2);
}

TEST_CASE("Lines ending where they touch a circle close an area (a drop shape)", "[regions]")
{
    // Tangent lines from a point down to a circle: each touches it at a double root.
    Sketch s;
    const double r = 5, d = 11;
    const double angle = std::acos(r / d);  // from straight up to each touching point
    s.addCircle(0, 0, r);
    for (double side : {-1.0, 1.0}) {
        s.addLine(0, d, side * r * std::sin(angle), r * std::cos(angle));
    }
    const Arrangement a(s, s.activeLayer());
    CHECK(a.looseEnds().empty());
    const auto tip = a.regionAt({0, 7});
    REQUIRE(tip);
    CHECK(tip->boundary.size() == 3);  // the two lines and the circle
    CHECK(a.regionAt({0, 0}));
}

TEST_CASE("Arcs bound areas too", "[regions]")
{
    Sketch s;
    const Id arc = s.addArc(0, 0, 10, 0, 180);
    s.addLine(s.arc(arc).end, s.arc(arc).start);  // close the half disc
    const Arrangement a(s, s.activeLayer());
    const auto half = a.regionAt({0, 3});
    REQUIRE(half);
    CHECK(half->area == Approx(std::numbers::pi * 50).epsilon(0.01));
    CHECK_FALSE(a.regionAt({0, -3}));
}

TEST_CASE("Construction geometry doesn't bound areas", "[regions]")
{
    Sketch s;
    const auto r = rect(s, 0, 0, 20, 10);
    const Id split = s.addLine(10, 0, 10, 10);
    s.entity(split).construction = true;
    REQUIRE(Arrangement(s, s.activeLayer()).regionAt({5, 5}));
    CHECK(Arrangement(s, s.activeLayer()).regionAt({5, 5})->area == Approx(200.0));
    (void)r;
}

TEST_CASE("Fills follow their outline, and come back when it's mended", "[regions][fill]")
{
    Sketch s;
    const auto r = rect(s, 0, 0, 20, 10);
    Fill f;
    Color::fromHex("#ff7f27", f.color);
    f.seed = Point{5, 5};
    const Id id = s.addFill(f);
    regions::refreshFills(s);
    CHECK(s.fill(id).boundary == sorted(sides(r)));

    // Moved away from its seed: found by its outline, and the seed moves inside.
    xform::apply(s, sides(r), Affine::translation(100, 0));
    REQUIRE(regions::resolve(Arrangement(s, s.activeLayer()), s.fill(id)));
    regions::refreshFills(s);
    CHECK(s.fill(id).seed.x > 100);

    // A side broken in two: the outline is different curves now, found by the seed.
    s.addLine(110, -5, 110, 2);  // crosses the bottom side
    modify::breakCurve(s, r.sides[0], {105, 0});
    REQUIRE(regions::resolve(Arrangement(s, s.activeLayer()), s.fill(id)));
    regions::refreshFills(s);
    CHECK(s.fill(id).boundary.size() == 5);

    // A side gone: lost, but kept; back when the outline closes again.
    Sketch before = s;
    s.remove(r.sides[2]);
    CHECK_FALSE(regions::resolve(Arrangement(s, s.activeLayer()), s.fill(id)));
    CHECK(s.hasFill(id));
    s = before;
    CHECK(regions::resolve(Arrangement(s, s.activeLayer()), s.fill(id)));
}

TEST_CASE("Fills are saved, and copied with their outline", "[regions][fill]")
{
    Sketch s;
    const auto r = rect(s, 0, 0, 20, 10);
    Fill f;
    Color::fromHex("#22b14c80", f.color);
    f.seed = Point{5, 5};
    const Id id = s.addFill(f);
    regions::refreshFills(s);

    const Sketch loaded = fromJson(toJson(s));
    REQUIRE(loaded.hasFill(id));
    CHECK(loaded.fill(id).color == s.fill(id).color);
    CHECK(loaded.fill(id).boundary == s.fill(id).boundary);

    const auto map = xform::copy(s, sides(r), Affine::translation(0, 50));
    REQUIRE(map.contains(id));
    const Fill& copy = s.fill(map.at(id));
    CHECK(copy.seed.y == Approx(55.0));
    const auto region = regions::resolve(Arrangement(s, s.activeLayer()), copy);
    REQUIRE(region);
    CHECK(region->contains({5, 55}));

    s.remove(id);
    CHECK_FALSE(s.hasFill(id));
}
