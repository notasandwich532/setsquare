// The eyedropper: what it takes (true colours, not as shown faded), the Colour 1 / 2 choice, and
// going back to the tool before. Also where a point is along a gradient (what it reads).

#include "ColorBar.h"
#include "Document.h"
#include "Images.h"
#include "SketchView.h"
#include "Snapshot.h"
#include "tools/StyleTools.h"

#include <core/Builders.h>
#include <core/Gradient.h>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <QTemporaryDir>
#include <QTest>

using namespace core;
using Catch::Approx;

namespace
{
Color hex(const char* text)
{
    Color c;
    Color::fromHex(text, c);
    return c;
}

struct Fixture
{
    Document doc;
    SketchView view{doc};
    ColorBar bar{view};
    build::Rectangle rect;
    std::optional<bool> answer = false;  // what the prompt answers
    int asked = 0;
    EyedropperTool::Chooser saved = EyedropperTool::chooser();

    Fixture()
    {
        view.resize(800, 600);
        view.show();
        view.resetView();
        doc.modify("rect", [&](Sketch& s) { rect = build::rectangle(s, s.addPoint(0, 0), s.addPoint(40, 20)); });
        EyedropperTool::chooser() = [this](const EyedropperTool::Sample&, QPoint) {
            ++asked;
            return answer;
        };
    }
    ~Fixture()
    {
        EyedropperTool::chooser() = saved;
        saveSnapshot(view);
    }
    void click(double x, double y)
    {
        const QPoint at = view.toScreen({x, y}).toPoint();
        QTest::mouseMove(&view, at);
        QTest::mouseClick(&view, Qt::LeftButton, {}, at);
    }
};
}  // namespace

TEST_CASE("Eyedropper: a fill's own colour, as Colour 2, then back to the tool before", "[eyedropper]")
{
    Fixture f;
    REQUIRE(f.view.tryEdit("fill", [&](Editor& ed) { ed.fill({10, 10}, hex("#ff7f27")); }));
    f.view.setTool(ToolKind::Fill);
    f.view.setTool(ToolKind::Eyedropper);
    f.answer = true;
    f.click(10, 10);  // shown faded while sketching, but it's #ff7f27
    CHECK(f.asked == 1);
    CHECK(f.view.paint().color2 == hex("#ff7f27"));
    CHECK(f.view.toolKind() == ToolKind::Fill);
}

TEST_CASE("Eyedropper: a stroke, as Colour 1; cancelled or nothing there: no change", "[eyedropper]")
{
    Fixture f;
    REQUIRE(f.view.tryEdit("stroke", [&](Editor& ed) {
        core::StrokeStyle style;
        style.color = hex("#22b14c");
        style.width = 1.0;
        ed.setStroke({f.rect.sides[0]}, style);
    }));
    const Color before = f.view.paint().color1;
    f.view.setTool(ToolKind::Eyedropper);
    f.click(60, 60);  // nothing
    CHECK(f.asked == 0);
    CHECK(f.view.toolKind() == ToolKind::Eyedropper);

    f.answer = std::nullopt;  // the prompt closed
    f.click(20, 0);
    CHECK(f.asked == 1);
    CHECK(f.view.paint().color1 == before);
    CHECK(f.view.toolKind() == ToolKind::Eyedropper);

    f.answer = false;
    f.click(20, 0);
    CHECK(f.view.paint().color1 == hex("#22b14c"));
    CHECK(f.view.toolKind() == ToolKind::Select);
}

TEST_CASE("Eyedropper: a gradient's colour where it's clicked", "[eyedropper]")
{
    Fixture f;
    Gradient g;
    g.stops = {{0.0, hex("#000000")}, {1.0, hex("#ffffff")}};
    const core::Line bottom = f.doc.sketch().line(f.rect.sides[0]);  // (0, 0) to (40, 0)
    const bool forward = f.doc.sketch().point(bottom.p1).x < f.doc.sketch().point(bottom.p2).x;
    g.start = forward ? bottom.p1 : bottom.p2;
    g.end = forward ? bottom.p2 : bottom.p1;
    REQUIRE(f.view.tryEdit("fill", [&](Editor& ed) { ed.fill({10, 10}, hex("#ff0000"), g); }));
    f.view.setTool(ToolKind::Eyedropper);

    f.answer = true;
    f.click(10, 10);  // a quarter of the way along
    const Color c = f.view.paint().color2;
    CHECK(int(c.r) == Approx(64).margin(3));
    CHECK(c.r == c.g);
}

TEST_CASE("Eyedropper: a reference image's own pixel", "[eyedropper]")
{
    Fixture f;
    QTemporaryDir dir;
    QImage picture(20, 10, QImage::Format_RGB32);
    picture.fill(QColor(0x3f, 0x48, 0xcc));
    for (int y = 0; y < 10; ++y) {
        for (int x = 0; x < 10; ++x) {
            picture.setPixelColor(x, y, QColor(0xed, 0x1c, 0x24));  // the left half
        }
    }
    const QString path = dir.filePath(QStringLiteral("swatches.png"));
    REQUIRE(picture.save(path));
    core::Image im = images::load(path);
    im.x = 100;
    im.y = 0;
    im.width = 40;
    im.opacity = 0.3;  // (faint on screen; its own colour is what's taken)
    REQUIRE(f.view.tryEdit("image", [&](Editor& ed) { ed.addImage(im); }));
    f.view.setTool(ToolKind::Eyedropper);
    f.answer = false;
    f.click(90, 3);
    CHECK(f.view.paint().color1 == hex("#ed1c24"));
    f.view.setTool(ToolKind::Eyedropper);
    f.click(110, -3);
    CHECK(f.view.paint().color1 == hex("#3f48cc"));
}

TEST_CASE("Where a point is along a gradient: linear, radial with a focal point, and spread", "[eyedropper]")
{
    gradient::Placed lin;
    lin.start = {0, 0};
    lin.end = {10, 0};
    CHECK(gradient::offsetAt(lin, {2.5, 7}) == Approx(0.25));
    CHECK(gradient::offsetAt(lin, {15, 0}) == Approx(1.0));  // pad
    lin.spread = GradientSpread::Repeat;
    CHECK(gradient::offsetAt(lin, {12.5, 0}) == Approx(0.25));
    lin.spread = GradientSpread::Reflect;
    CHECK(gradient::offsetAt(lin, {12.5, 0}) == Approx(0.75));

    gradient::Placed rad;
    rad.kind = GradientKind::Radial;
    rad.center = {0, 0};
    rad.radius = 10;
    CHECK(gradient::offsetAt(rad, {0, 5}) == Approx(0.5));
    // Focal point at (-5, 0): halfway from it to the edge on the line through both.
    rad.focalLocal = {-5, 0};
    rad.focal = {-5, 0};
    CHECK(gradient::offsetAt(rad, {2.5, 0}) == Approx(0.5));    // between (-5, 0) and (10, 0)
    CHECK(gradient::offsetAt(rad, {-7.5, 0}) == Approx(0.5));   // between (-5, 0) and (-10, 0)
    CHECK(gradient::offsetAt(rad, {-5, 0}) == Approx(0.0).margin(1e-12));
    // An ellipse twice as wide across: (0, 10) is on its edge.
    rad.focalLocal = {0, 0};
    rad.focal = {0, 0};
    rad.aspect = 2.0;
    CHECK(gradient::offsetAt(rad, {0, 10}) == Approx(0.5));
    CHECK(gradient::offsetAt(rad, {0, 20}) == Approx(1.0));
}
