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
