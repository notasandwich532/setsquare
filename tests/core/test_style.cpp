// Strokes: colours, line styles, and how they follow the geometry.

#include <core/Features.h>
#include <core/Modify.h>
#include <core/Serialize.h>
#include <core/Style.h>
#include <core/Transform.h>

#include <catch2/catch_test_macros.hpp>

using namespace core;

namespace
{
StrokeStyle red()
{
    StrokeStyle s;
    Color::fromHex("#ed1c24", s.color);
    s.width = 0.5;
    s.dash = Dash::DashDot;
    return s;
}
}  // namespace

TEST_CASE("Colours read and write as hex", "[style]")
{
    Color c;
    REQUIRE(Color::fromHex("#ff8000", c));
    CHECK(c == Color{255, 128, 0, 255});
    CHECK(c.hex() == "#ff8000");
    REQUIRE(Color::fromHex("0f08", c) == false);
    REQUIRE(Color::fromHex("#0f0", c));
    CHECK(c.hex() == "#00ff00");
    REQUIRE(Color::fromHex("#11223380", c));
    CHECK(c.a == 0x80);
    CHECK(c.hex() == "#11223380");
    CHECK_FALSE(Color::fromHex("#12345g", c));
}

TEST_CASE("Strokes belong to curves", "[style]")
{
    Sketch s;
    const Id l = s.addLine(0, 0, 10, 0);
    const Id p = s.addPoint(5, 5);
    s.setStroke(l, red());
    REQUIRE(s.stroke(l) != nullptr);
    CHECK(*s.stroke(l) == red());
    CHECK_THROWS_AS(s.setStroke(p, red()), SketchError);  // points have no line
    s.remove(s.line(l).p1);                                 // takes the line, and its stroke
    CHECK(s.strokes().empty());
}

TEST_CASE("Strokes survive a save and load", "[style][serialize]")
{
    Sketch s;
    const Id c = s.addCircle(0, 0, 5);
    StrokeStyle hair = red();
    hair.width = 0.0;
    hair.color.a = 128;
    s.setStroke(c, hair);
    const Sketch loaded = fromJson(toJson(s));
    REQUIRE(loaded.stroke(c) != nullptr);
    CHECK(*loaded.stroke(c) == hair);
}

TEST_CASE("Copies, mirrors, patterns and offsets look like their originals", "[style]")
{
    Sketch s;
    s.ensureOrigin();
    const Id l = s.addLine(10, 0, 20, 5);
    s.setStroke(l, red());

    const auto copied = xform::copy(s, {l}, Affine::translation(0, 10));
    CHECK(*s.stroke(copied.at(l)) == red());
    const auto mirrored = xform::mirror(s, {l}, s.yAxis());
    CHECK(*s.stroke(mirrored.at(l)) == red());
    const Id k = feature::circularPattern(s, {l}, s.origin(), {3, 360});
    for (const auto& instance : feature::pattern(s.constraint(k)).copies) {
        CHECK(*s.stroke(instance[0]) == red());
    }
    const Id o = feature::offset(s, {l}, 1, {15, 10});
    CHECK(*s.stroke(feature::offsetRefs(s.constraint(o)).offsets[0]) == red());
}

TEST_CASE("Trimmed and filleted outlines keep their stroke", "[style][modify]")
{
    Sketch s;
    const Id a = s.addLine(0, 0, 20, 0);
    const Id b = s.addLine(s.line(a).p2, s.addPoint(20, 20));  // a corner shared with a
    s.addLine(10, -5, 10, 5);  // crosses a
    s.setStroke(a, red());
    s.setStroke(b, red());

    const std::vector<Id> pieces = modify::breakCurve(s, a, {5, 0});
    for (Id piece : pieces) {
        CHECK(*s.stroke(piece) == red());
    }
    Id ca = kNoId, cb = kNoId;
    Vec2 na, nb;
    REQUIRE(modify::cornerAt(s, s.line(b).p1, ca, na, cb, nb));
    const modify::Corner corner = modify::fillet(s, ca, na, cb, nb, 3);
    CHECK(*s.stroke(corner.joint) == red());
    for (Id piece : corner.pieces) {
        CHECK(*s.stroke(piece) == red());
    }
}
