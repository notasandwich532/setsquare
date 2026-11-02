// Text items in the sketch model: anchoring, removal, saving and export (shapes made by hand here;
// making them from fonts is the app's job, tested there).

#include <core/Export.h>
#include <core/Serialize.h>
#include <core/Sketch.h>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <string>

using namespace core;
using Catch::Approx;

namespace
{
Text square(Id anchor)
{
    Text t;
    t.anchor = anchor;
    t.text = "I";
    t.font = "Test";
    t.height = 10;
    t.shape = {{{0, 0}, {2, 0}, {2, 10}, {0, 10}}};
    return t;
}
}  // namespace

TEST_CASE("A text sits on its anchor point, turned by its angle", "[text]")
{
    const Point w = textToWorld({10, 5}, 90.0, {2, 0});
    CHECK(w.x == Approx(10.0).margin(1e-12));
    CHECK(w.y == Approx(7.0));
    Sketch s;
    CHECK_THROWS_AS(s.addText(square(42)), SketchError);  // no such point
    const Id p = s.addPoint(0, 0);
    Text bad = square(p);
    bad.height = 0;
    CHECK_THROWS_AS(s.addText(bad), SketchError);
    const Id t = s.addText(square(p));
    CHECK(s.hasText(t));
    CHECK(s.isPointUsed(p));  // (so tidying up after trims leaves it)
}

TEST_CASE("Deleting a text takes its lone anchor; deleting the anchor takes the text", "[text]")
{
    Sketch s;
    const Id p = s.addPoint(0, 0);
    const Id t = s.addText(square(p));
    s.remove(t);
    CHECK_FALSE(s.hasText(t));
    CHECK_FALSE(s.hasEntity(p));

    // An anchor that's a line's end stays.
    const Id l = s.addLine(0, 0, 10, 0);
    const Id t2 = s.addText(square(s.line(l).p1));
    s.remove(t2);
    CHECK(s.hasEntity(s.line(l).p1));
    const Id t3 = s.addText(square(s.line(l).p2));
    s.remove(s.line(l).p2);  // the line goes, and the text with it
    CHECK_FALSE(s.hasText(t3));
}

TEST_CASE("Texts are saved with their shape, and exported as filled outlines", "[text][export]")
{
    Sketch s;
    const Id p = s.addPoint(5, 5);
    Text t = square(p);
    t.text = "Say \"hi\"";
    t.bold = true;
    t.angle = 30;
    t.outline = TextOutline::Silhouette;
    t.minThickness = 0.4;
    t.spacing = 12.5;
    t.align = TextAlign::Right;
    t.valign = TextVAlign::Middle;
    t.flipV = true;
    t.hasColor = true;
    Color::fromHex("#ff0000", t.color);
    const Id id = s.addText(t);

    const Sketch back = fromJson(toJson(s));
    REQUIRE(back.hasText(id));
    const Text& r = back.text(id);
    CHECK(r.text == "Say \"hi\"");
    CHECK(r.bold);
    CHECK(r.angle == 30.0);
    CHECK(r.outline == TextOutline::Silhouette);
    CHECK(r.minThickness == 0.4);
    CHECK(r.spacing == 12.5);
    CHECK(r.align == TextAlign::Right);
    CHECK(r.valign == TextVAlign::Middle);
    CHECK(r.flipV);
    CHECK_FALSE(r.flipH);
    CHECK(r.hasColor);
    CHECK(r.color.hex() == "#ff0000");
    REQUIRE(r.shape.size() == 1);
    CHECK(r.shape[0].size() == 4);
    CHECK(r.shape[0][2].y == 10.0);

    const std::string svg = exporter::toSvg(s, {});
    CHECK(svg.find("id=\"text\"") != std::string::npos);
    CHECK(svg.find("fill=\"#ff0000\"") != std::string::npos);
    CHECK(svg.find("fill-rule=\"nonzero\"") != std::string::npos);
    const std::string dxf = exporter::toDxf(s, {});
    CHECK(dxf.find("POLYLINE") != std::string::npos);
    CHECK(dxf.find("TEXT") != std::string::npos);
}
