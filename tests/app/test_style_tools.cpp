// Strokes in the UI: the stroke brush, the colour bar and presentation view.

#include "ColorBar.h"
#include "Document.h"
#include "PythonHost.h"
#include "SketchView.h"
#include "Snapshot.h"

#include <core/Builders.h>

#include <catch2/catch_test_macros.hpp>

#include <QTest>

using namespace core;

namespace
{

struct Fixture
{
    Document doc;
    SketchView view{doc};
    ColorBar bar{view};
    QStringList recorded;
    build::Rectangle rect;

    Fixture()
    {
        view.resize(900, 650);
        view.show();
        view.resetView();
        QObject::connect(&doc, &Document::recorded, [this](const QString& py) { recorded << py; });
        doc.modify("rectangle", [&](Sketch& s) {
            const Id a = s.addPoint(10, 10);
            rect = build::rectangle(s, a, s.addPoint(40, 30));
        });
    }
    ~Fixture() { saveSnapshot(view); }

    QPoint at(double x, double y) const { return view.toScreen({x, y}).toPoint(); }
    void click(double x, double y, Qt::KeyboardModifiers mods = {})
    {
        QTest::mouseMove(&view, at(x, y));
        QTest::mouseClick(&view, Qt::LeftButton, mods, at(x, y));
    }
    const Sketch& sketch() const { return doc.sketch(); }
    static Color hex(const char* text)
    {
        Color c;
        Color::fromHex(text, c);
        return c;
    }
};

}  // namespace

TEST_CASE("The stroke brush paints curves with the current colour and style", "[style][brush]")
{
    Fixture f;
    f.bar.pickColor(Fixture::hex("#ed1c24"));
    f.bar.setWidth(0.7);
    f.bar.setDash(Dash::Dashed);
    f.view.setTool(ToolKind::StrokeBrush);
    f.click(25, 10);  // bottom side
    const Id bottom = f.rect.sides[0];
    REQUIRE(f.sketch().stroke(bottom) != nullptr);
    CHECK(f.sketch().stroke(bottom)->color == Fixture::hex("#ed1c24"));
    CHECK(f.sketch().stroke(bottom)->width == 0.7);
    CHECK(f.sketch().stroke(bottom)->dash == Dash::Dashed);
    CHECK(f.sketch().strokes().size() == 1);

    // Alt+click picks the style up; Ctrl+click takes it off.
    f.bar.pickColor(Fixture::hex("#00a2e8"));
    f.click(25, 10, Qt::AltModifier);
    CHECK(f.view.paint().color1 == Fixture::hex("#ed1c24"));
    f.click(25, 10, Qt::ControlModifier);
    CHECK(f.sketch().strokes().empty());

    // Double-click: the whole outline.
    QTest::mouseDClick(&f.view, Qt::LeftButton, {}, f.at(40, 20));
    CHECK(f.sketch().strokes().size() == 4);
}

TEST_CASE("Colour bar changes apply to the selection, part by part", "[style][colorbar]")
{
    Fixture f;
    f.bar.setWidth(1.0);
    f.view.setSelection({f.rect.sides[0], f.rect.sides[1]});
    f.bar.pickColor(Fixture::hex("#22b14c"));  // stroke them green at the current width
    for (Id side : {f.rect.sides[0], f.rect.sides[1]}) {
        REQUIRE(f.sketch().stroke(side) != nullptr);
        CHECK(f.sketch().stroke(side)->color == Fixture::hex("#22b14c"));
        CHECK(f.sketch().stroke(side)->width == 1.0);
    }
    f.view.setSelection({f.rect.sides[1]});
    f.bar.setDash(Dash::Dotted);  // only the dash changes, only on the selected side
    CHECK(f.sketch().stroke(f.rect.sides[1])->dash == Dash::Dotted);
    CHECK(f.sketch().stroke(f.rect.sides[1])->color == Fixture::hex("#22b14c"));
    CHECK(f.sketch().stroke(f.rect.sides[0])->dash == Dash::Solid);
    // Right-click colours (Colour 2) are for fills: they don't touch strokes.
    f.bar.pickColor(Fixture::hex("#fff200"), true);
    CHECK(f.sketch().stroke(f.rect.sides[1])->color == Fixture::hex("#22b14c"));
    CHECK(f.view.paint().color2 == Fixture::hex("#fff200"));

    // Selecting a decorated curve shows its style in the bar.
    f.bar.pickColor(Fixture::hex("#000000"), false);  // (applies to side 1 too)
    f.view.setSelection({});
    f.bar.setWidth(0.25);
    f.view.setSelection({f.rect.sides[0]});
    CHECK(f.view.paint().width == 1.0);
    CHECK(f.view.paint().color1 == Fixture::hex("#22b14c"));
}

TEST_CASE("Presentation view hides the sketching aids and turns off for drawing tools", "[style][presentation]")
{
    Fixture f;
    f.doc.modify("dimension", [&](Sketch& s) { s.addConstraint(ConstraintType::Distance, {f.rect.sides[0]}, 30.0); });
    f.view.setPresentation(true);
    // Points and constraint symbols aren't shown, so they aren't picked: a click at a corner
    // takes the line.
    const Id hit = f.view.pickAt(f.at(10, 10));
    REQUIRE(hit != kNoId);
    CHECK(f.sketch().entity(hit).kind() == EntityKind::Line);
    f.view.setTool(ToolKind::StrokeBrush);
    CHECK(f.view.presentation());
    f.view.setTool(ToolKind::Line);
    CHECK_FALSE(f.view.presentation());
}

TEST_CASE("Construction geometry is hidden in presentation view", "[style][presentation]")
{
    Fixture f;
    Id helper = kNoId;
    f.doc.modify("construction", [&](Sketch& s) {
        helper = s.addLine(0, 50, 60, 50);
        s.entity(helper).construction = true;
    });
    CHECK(f.view.pickAt(f.at(30, 50)) == helper);
    f.view.setPresentation(true);
    CHECK(f.view.pickAt(f.at(30, 50)) == kNoId);  // not drawn, so not picked
    CHECK(f.view.pickAt(f.at(0, 25)) == kNoId);   // nor the Y axis
    CHECK(f.view.pickAt(f.at(25, 10)) == f.rect.sides[0]);

    // A box over everything takes only the drawing.
    f.view.setTool(ToolKind::Select);
    QTest::mousePress(&f.view, Qt::LeftButton, {}, f.at(-5, 60));
    QTest::mouseMove(&f.view, f.at(30, 30));
    QTest::mouseMove(&f.view, f.at(70, 0));
    QTest::mouseRelease(&f.view, Qt::LeftButton, {}, f.at(70, 0));
    CHECK(f.view.selection().size() == 4);
    CHECK_FALSE(f.view.selection().contains(helper));
}

TEST_CASE("Replaying recorded strokes reproduces them", "[style][recorder]")
{
    Fixture f;
    f.recorded.clear();
    f.recorded << "s.rectangle((10, 10), (40, 30))";
    f.bar.pickColor(Fixture::hex("#3f48cc"));
    f.bar.setWidth(0.5);
    f.view.setTool(ToolKind::StrokeBrush);
    f.click(25, 10);
    QTest::mouseDClick(&f.view, Qt::LeftButton, {}, f.at(40, 20));
    f.view.setSelection({f.rect.sides[2]});
    f.bar.setDash(Dash::DashDotDot);

    Document replay;
    script::setDocument(&replay);
    script::resetConsole();
    QString err;
    script::setOutput([&](const QString& t, bool e) {
        if (e) {
            err += t;
        }
    });
    const script::RunResult r = script::runConsole(f.recorded.join('\n'));
    script::setOutput(nullptr);
    script::setDocument(nullptr);
    INFO(f.recorded.join('\n').toStdString());
    INFO(err.toStdString());
    REQUIRE(r.ok);
    CHECK(replay.sketch().strokes() == f.sketch().strokes());
}

TEST_CASE("The stroke brush paints a box of curves; Ctrl takes them off; Reset clears the selection",
          "[style][brush]")
{
    Fixture f;
    f.bar.pickColor(Fixture::hex("#ff7f27"));
    f.view.setTool(ToolKind::StrokeBrush);
    auto box = [&](QPoint from, QPoint to, Qt::KeyboardModifiers mods) {
        QTest::mousePress(&f.view, Qt::LeftButton, mods, from);
        QTest::mouseMove(&f.view, (from + to) / 2);
        QTest::mouseMove(&f.view, to);
        QTest::mouseRelease(&f.view, Qt::LeftButton, mods, to);
    };
    box(f.at(0, 40), f.at(50, 0), {});  // window over the whole rectangle
    CHECK(f.sketch().strokes().size() == 4);
    box(f.at(28, 35), f.at(22, 25), Qt::ControlModifier);  // crossing: just the top side
    CHECK(f.sketch().strokes().size() == 3);
    CHECK(f.sketch().stroke(f.rect.sides[2]) == nullptr);

    f.view.setTool(ToolKind::Select);
    f.view.setSelection({f.rect.sides[0], f.rect.sides[1]});
    f.bar.resetSelection();
    CHECK(f.sketch().strokes().size() == 1);
}

TEST_CASE("The fill tool fills closed areas with Colour 2", "[style][fill]")
{
    Fixture f;
    f.bar.pickColor(Fixture::hex("#ffc90e"), true);  // right-click: Colour 2
    f.view.setTool(ToolKind::Fill);
    f.click(20, 20);  // inside the rectangle
    REQUIRE(f.sketch().fills().size() == 1);
    const Id fill = f.sketch().fills().begin()->first;
    CHECK(f.sketch().fill(fill).color == Fixture::hex("#ffc90e"));
    CHECK(f.sketch().fill(fill).boundary.size() == 4);

    // Again with another colour: recoloured, not stacked.
    f.bar.pickColor(Fixture::hex("#22b14c"), true);
    f.click(30, 25);
    CHECK(f.sketch().fills().size() == 1);
    CHECK(f.sketch().fill(fill).color == Fixture::hex("#22b14c"));

    // Alt+click picks the colour up; Ctrl+click removes the fill.
    f.bar.pickColor(Fixture::hex("#000000"), true);
    f.click(20, 20, Qt::AltModifier);
    CHECK(f.view.paint().color2 == Fixture::hex("#22b14c"));
    f.click(20, 20, Qt::ControlModifier);
    CHECK(f.sketch().fills().empty());

    // Outside anything: nothing filled, and the hint says why.
    f.click(80, 80);
    CHECK(f.sketch().fills().empty());
}

TEST_CASE("An open outline shows its loose ends", "[style][fill]")
{
    Fixture f;
    f.doc.modify("open it", [&](Sketch& s) { s.remove(f.rect.sides[3]); });
    f.view.setTool(ToolKind::Fill);
    f.click(20, 20);
    CHECK(f.sketch().fills().empty());
    CHECK(f.view.hint().contains("loose ends"));
}

TEST_CASE("Replaying recorded fills reproduces them", "[style][fill][recorder]")
{
    Fixture f;
    f.recorded.clear();
    f.recorded << "s.rectangle((10, 10), (40, 30))";
    f.bar.pickColor(Fixture::hex("#ed1c24"), true);
    f.view.setTool(ToolKind::Fill);
    f.click(20, 20);
    f.bar.pickColor(Fixture::hex("#3f48cc"), true);
    f.click(25, 15);

    Document replay;
    script::setDocument(&replay);
    script::resetConsole();
    const script::RunResult r = script::runConsole(f.recorded.join('\n'));
    script::setDocument(nullptr);
    INFO(f.recorded.join('\n').toStdString());
    REQUIRE(r.ok);
    REQUIRE(replay.sketch().fills().size() == 1);
    CHECK(replay.sketch().fills().begin()->second.color == Fixture::hex("#3f48cc"));
}
