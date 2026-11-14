// Drawn dimensions: what they measure and say, how they're laid out, and that they follow (and go
// with) their geometry, save and load.

#include <core/Annotations.h>
#include <core/Builders.h>
#include <core/Serialize.h>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <numbers>

using namespace core;
using Catch::Approx;

namespace
{
DrawnDimension make(DrawnDimensionKind kind, std::vector<Id> refs, std::string text = {})
{
    DrawnDimension d;
    d.kind = kind;
    d.refs = std::move(refs);
    d.text = text.empty() ? annotation::defaultText(kind) : text;
    return d;
}

double dist(Vec2 a, Vec2 b)
{
    return std::hypot(a.x - b.x, a.y - b.y);
}
}  // namespace

TEST_CASE("Drawn dimensions measure, and say it with their decimals and text", "[annotations]")
{
    Sketch s;
    const Id line = s.addLine(s.addPoint(0, 0), s.addPoint(30, 0));
    const Id d = s.addDrawnDimension(make(DrawnDimensionKind::Aligned, {line}));
    CHECK(annotation::label(s, s.drawnDimension(d)) == "30.00");  // (the drawing's 2 places)

    DrawnDimension one = s.drawnDimension(d);
    one.decimals = 1;
    one.text = "<> TYP";
    s.setDrawnDimension(one);
    CHECK(annotation::label(s, s.drawnDimension(d)) == "30.0 TYP");

    s.setDimensionDecimals(0);
    one.decimals = -1;
    one.text = "<> / <>";  // (each <>)
    s.setDrawnDimension(one);
    CHECK(annotation::label(s, s.drawnDimension(d)) == "30 / 30");
    CHECK_THROWS(s.setDimensionDecimals(7));
}

TEST_CASE("Each kind measures what it should", "[annotations]")
{
    Sketch s;
    s.setDimensionDecimals(2);
    const Id a = s.addPoint(10, 10), b = s.addPoint(40, 50);
    CHECK(annotation::label(s, make(DrawnDimensionKind::Aligned, {a, b})) == "50.00");
    CHECK(annotation::label(s, make(DrawnDimensionKind::Horizontal, {a, b})) == "30.00");
    CHECK(annotation::label(s, make(DrawnDimensionKind::Vertical, {a, b})) == "40.00");

    const Id circle = s.addCircle(0, 0, 6);
    CHECK(annotation::label(s, make(DrawnDimensionKind::Radius, {circle})) == "R6.00");
    CHECK(annotation::label(s, make(DrawnDimensionKind::Diameter, {circle})) == "\xC3\x98" "12.00");

    const Id l1 = s.addLine(s.addPoint(100, 0), s.addPoint(120, 0));
    const Id l2 = s.addLine(s.addPoint(100, 0), s.addPoint(100, 20));
    CHECK(annotation::label(s, make(DrawnDimensionKind::Angle, {l1, l2})) == "90.00\xC2\xB0");

    const Id arc = s.addArc(0, 100, 10, 0, 90);
    CHECK(annotation::value(s, make(DrawnDimensionKind::ArcLength, {arc})) == Approx(5 * std::numbers::pi));

    const Id origin = s.addPoint(5, 0);
    DrawnDimension ord = make(DrawnDimensionKind::Ordinate, {a, origin});
    CHECK(annotation::label(s, ord) == "5.00");
    ord.alongY = true;
    CHECK(annotation::label(s, ord) == "10.00");
    CHECK(annotation::label(s, make(DrawnDimensionKind::Ordinate, {origin, a})) == "-5.00");  // (signed)

    CHECK(annotation::label(s, make(DrawnDimensionKind::Leader, {a})) == "Note");  // (no number)

    // What can't be measured that way is refused.
    CHECK_THROWS(s.addDrawnDimension(make(DrawnDimensionKind::ArcLength, {circle})));
    CHECK_THROWS(s.addDrawnDimension(make(DrawnDimensionKind::Radius, {a})));
    CHECK_THROWS(s.addDrawnDimension(make(DrawnDimensionKind::Ordinate, {a, circle})));
}

TEST_CASE("Drawn dimensions are laid out as lines, arrowheads and text, in mm", "[annotations]")
{
    Sketch s;
    const Id line = s.addLine(s.addPoint(0, 0), s.addPoint(40, 0));
    DrawnDimension d = make(DrawnDimensionKind::Aligned, {line});
    d.textHeight = 3.5;
    // The label 10 below the middle: the dimension line there, its text along it.
    const Vec2 off = annotation::labelOffsetFor(s, d, {20, -10});
    d.labelX = off.x;
    d.labelY = off.y;
    const annotation::Graphics g = annotation::graphics(s, d);
    REQUIRE(g.valid);
    CHECK(g.arrows.size() == 2);
    CHECK(g.lines.size() == 3);  // two extension lines and the dimension line
    CHECK(g.label.x == Approx(20));
    CHECK(g.label.y == Approx(-10));
    CHECK(g.text.text == "40.00");
    CHECK(g.text.angle == Approx(0));
    CHECK(g.text.align == 1);
    CHECK(g.text.at.x == Approx(20));
    CHECK(g.text.at.y > -10);  // (above the line)
    // The arrowheads' tips on the dimension line's ends, 0.8 x the text high long.
    for (const auto& arrow : g.arrows) {
        CHECK(arrow[0].y == Approx(-10));
        CHECK(dist(arrow[0], {(arrow[1].x + arrow[2].x) / 2, (arrow[1].y + arrow[2].y) / 2}) == Approx(0.8 * 3.5));
    }

    // A vertical one's text reads up the page, not down.
    const Id up = s.addLine(s.addPoint(100, 0), s.addPoint(100, 30));
    DrawnDimension v = make(DrawnDimensionKind::Aligned, {up});
    CHECK(annotation::graphics(s, v).text.angle == Approx(90));

    // Every kind lays out.
    const Id circle = s.addCircle(0, 50, 8);
    const Id arc = s.addArc(50, 50, 10, 0, 120);
    const Id l2 = s.addLine(s.addPoint(0, 0), s.addPoint(0, 30));
    for (const DrawnDimension& k : {make(DrawnDimensionKind::Radius, {circle}), make(DrawnDimensionKind::Diameter, {circle}),
                                    make(DrawnDimensionKind::Angle, {line, l2}), make(DrawnDimensionKind::ArcLength, {arc}),
                                    make(DrawnDimensionKind::Ordinate, {s.line(line).p2, s.line(line).p1}),
                                    make(DrawnDimensionKind::Leader, {circle})}) {
        INFO(toString(k.kind));
        const annotation::Graphics kg = annotation::graphics(s, k);
        CHECK(kg.valid);
        CHECK(!kg.text.text.empty());
    }
}

TEST_CASE("Drawn dimensions go in their layer's own sublayer, made with the first", "[annotations]")
{
    Sketch s;
    const Id working = s.activeSublayer();
    const Id line = s.addLine(s.addPoint(0, 0), s.addPoint(30, 0));
    CHECK(s.annotationSublayer(s.activeLayer()) == kNoId);
    const Id a = s.addDrawnDimension(make(DrawnDimensionKind::Aligned, {line}));
    const Id sub = s.annotationSublayer(s.activeLayer());
    REQUIRE(sub != kNoId);
    CHECK(s.sublayer(sub).name == "Annotations");
    CHECK(s.sublayer(sub).annotations);
    CHECK(s.sublayerOf(a) == sub);
    CHECK(s.activeSublayer() == working);  // (still drawing where you were)
    CHECK(s.sublayerOf(line) == working);

    // The next goes in the same one.
    const Id b = s.addDrawnDimension(make(DrawnDimensionKind::Leader, {line}, "x"));
    CHECK(s.sublayerOf(b) == sub);
    CHECK(s.sublayersOf(s.activeLayer()).size() == 2);

    // Hiding it hides them.
    s.setSublayerVisible(sub, false);
    CHECK_FALSE(s.isShown(a));

    // Another layer: its own (named apart from a sublayer already called that).
    const Id other = s.addLayer("Layer 2", "Annotations");
    s.setActiveLayer(other);
    const Id l2 = s.addLine(s.addPoint(0, 10), s.addPoint(30, 10));
    const Id c = s.addDrawnDimension(make(DrawnDimensionKind::Aligned, {l2}));
    const Id sub2 = s.annotationSublayer(other);
    REQUIRE(sub2 != kNoId);
    CHECK(sub2 != sub);
    CHECK(s.sublayer(sub2).name == "Annotations 2");
    CHECK(s.sublayerOf(c) == sub2);

    // Saved: it's still theirs (and renaming it doesn't matter).
    s.renameSublayer(sub, "Dims");
    const Sketch loaded = fromJson(toJson(s));
    CHECK(loaded.annotationSublayer(s.layerOfSublayer(sub)) == sub);
    CHECK(loaded.annotationSublayer(other) == sub2);
}

TEST_CASE("Drawn dimensions follow their geometry, go with it, and save", "[annotations]")
{
    Sketch s;
    const auto box = build::rectangle(s, s.addPoint(0, 0), s.addPoint(30, 20));
    DrawnDimension width = make(DrawnDimensionKind::Horizontal, {box.sides[0]});
    width.decimals = 1;
    width.style.color = Color{0x12, 0x34, 0x56, 255};
    width.style.width = 0.35;
    width.textHeight = 5;
    width.font = "Arial";
    width.text = "W <>";
    const Id w = s.addDrawnDimension(width);
    const Id note = s.addDrawnDimension(make(DrawnDimensionKind::Leader, {box.sides[1]}, "Polish"));

    // Saved and loaded, as it was.
    const Sketch loaded = fromJson(toJson(s));
    REQUIRE(loaded.hasDrawnDimension(w));
    const DrawnDimension& back = loaded.drawnDimension(w);
    CHECK(back.kind == DrawnDimensionKind::Horizontal);
    CHECK(back.refs == width.refs);
    CHECK(back.decimals == 1);
    CHECK(back.style.color == width.style.color);
    CHECK(back.style.width == Approx(0.35));
    CHECK(back.textHeight == Approx(5));
    CHECK(back.font == "Arial");
    CHECK(annotation::label(loaded, back) == "W 30.0");
    CHECK(loaded.drawnDimension(note).text == "Polish");

    // Deleting what it measures deletes it (and only it).
    s.remove(box.sides[0]);
    CHECK_FALSE(s.hasDrawnDimension(w));
    CHECK(s.hasDrawnDimension(note));
    s.remove(note);
    CHECK(s.drawnDimensions().empty());
}
