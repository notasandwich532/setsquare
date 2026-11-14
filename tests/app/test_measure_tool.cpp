// The Measure tool: a curve's length (Shift+click adds more), the distance between two points
// with dx and dy, an area; starting from the selection; numbers that follow edits.

#include "CommandPanel.h"
#include "Document.h"
#include "SketchView.h"
#include "Snapshot.h"

#include <core/Builders.h>
#include <core/Measure.h>

#include <catch2/catch_test_macros.hpp>

#include <QLabel>
#include <QTest>

using namespace core;

namespace
{
struct Fixture
{
    Document doc;
    SketchView view{doc};
    build::Rectangle box;
    Id hole = kNoId;

    Fixture()
    {
        view.resize(900, 650);
        view.show();
        view.resetView();
        doc.modify("geometry", [&](Sketch& s) {
            box = build::rectangle(s, s.addPoint(10, 10), s.addPoint(40, 30));
            hole = s.addCircle(20, 20, 4);
        });
    }
    ~Fixture() { saveSnapshot(view); }

    QPoint at(double x, double y) const { return view.toScreen({x, y}).toPoint(); }
    void click(double x, double y, Qt::KeyboardModifiers mods = {})
    {
        QTest::mouseMove(&view, at(x, y));
        QTest::mouseClick(&view, Qt::LeftButton, mods, at(x, y));
    }
    // A row of the panel as shown ("" when it isn't).
    QString shown(const char* name) const
    {
        const QLabel* label = view.findChild<QLabel*>(QLatin1String(name));
        REQUIRE(label);
        const QWidget* panel = view.findChild<CommandPanel*>();
        return panel && label->isVisibleTo(panel) ? label->text() : QString();
    }
};
}  // namespace

TEST_CASE("Measure: a curve's length, and Shift+click adds more up", "[measure]")
{
    Fixture f;
    f.view.setTool(ToolKind::Measure);
    REQUIRE(f.view.findChild<CommandPanel*>());
    CHECK(f.shown("measureLength").isEmpty());

    f.click(25, 10);  // the bottom side
    CHECK(f.shown("measureLength") == QStringLiteral("30 mm"));
    CHECK(f.shown("measureArea").isEmpty());
    f.click(10, 20, Qt::ShiftModifier);  // and the left one
    CHECK(f.shown("measureLength") == QStringLiteral("50 mm"));
    f.click(10, 20, Qt::ShiftModifier);  // (out again)
    CHECK(f.shown("measureLength") == QStringLiteral("30 mm"));
    f.click(24, 20);  // the circle
    CHECK(f.shown("measureLength") == QStringLiteral("25.133 mm"));  // 8 pi
}

TEST_CASE("Measure: two points, the distance and the differences in x and y", "[measure]")
{
    Fixture f;
    f.view.setTool(ToolKind::Measure);
    f.click(10, 10);  // a corner
    CHECK(f.shown("measureDistance").isEmpty());  // (waiting for the second)
    f.click(20, 20);  // the circle's centre
    CHECK(f.shown("measureDistance") == QStringLiteral("14.142 mm"));
    CHECK(f.shown("measureDx") == QStringLiteral("10 mm"));
    CHECK(f.shown("measureDy") == QStringLiteral("10 mm"));
    f.click(40, 30);  // a new pair, from here
    CHECK(f.shown("measureDistance").isEmpty());
    f.click(10, 10);
    CHECK(f.shown("measureDistance") == QStringLiteral("36.056 mm"));
    CHECK(f.shown("measureDx") == QStringLiteral("-30 mm"));
    CHECK(f.shown("measureDy") == QStringLiteral("-20 mm"));
}

TEST_CASE("Measure: inside an area, its size less its holes", "[measure]")
{
    Fixture f;
    f.view.setTool(ToolKind::Measure);
    f.click(35, 25);
    CHECK(f.shown("measureArea") == QStringLiteral("549.735 mm²"));  // 600 - 16 pi
    f.click(21.5, 21.5);  // inside the circle (away from its centre point, which a click would measure from)
    CHECK(f.shown("measureArea") == QStringLiteral("50.265 mm²"));
    f.click(100, 100);  // nothing there
    CHECK(f.shown("measureArea").isEmpty());
}

TEST_CASE("Measure: starts with what's selected, and keeps up with edits", "[measure]")
{
    Fixture f;
    f.view.setSelection({f.box.sides[0], f.box.sides[1]});
    f.view.setTool(ToolKind::Measure);
    CHECK(f.shown("measureLength") == QStringLiteral("50 mm"));

    // Made longer (by an edit elsewhere: undo, a script): the number follows.
    f.doc.modify("wider", [&](Sketch& s) {
        const Line& bottom = std::get<Line>(s.entity(f.box.sides[0]).geom);
        s.point(bottom.p2).x = 50;
    });
    f.view.repaint();
    const double now = measuring::length(f.doc.sketch(), {f.box.sides[0], f.box.sides[1]});
    INFO("now " << now << ", shown " << f.shown("measureLength").toStdString());
    CHECK(now > 50.5);
    CHECK(f.shown("measureLength") == QStringLiteral("%1 mm").arg(now));
}
