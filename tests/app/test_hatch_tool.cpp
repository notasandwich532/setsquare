// The hatch tool: its panel (pattern, angle, scale, width), hatching areas, editing them as a list,
// picking a hatch up, taking it off, palette colours, and double-click from the select tool.

#include "ColorBar.h"
#include "CommandPanel.h"
#include "Document.h"
#include "SketchView.h"
#include "Snapshot.h"
#include "tools/HatchTool.h"

#include <core/Builders.h>
#include <core/Hatch.h>

#include <catch2/catch_test_macros.hpp>

#include <QComboBox>
#include <QDoubleSpinBox>
#include <QLineEdit>
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

    Fixture()
    {
        view.resize(900, 650);
        view.show();
        view.resetView();
        QObject::connect(&doc, &Document::recorded, [this](const QString& py) { recorded << py; });
        doc.modify("geometry", [&](Sketch& s) {
            build::rectangle(s, s.addPoint(10, 10), s.addPoint(40, 30));  // two areas side by side
            build::rectangle(s, s.addPoint(50, 10), s.addPoint(80, 30));
        });
        view.setTool(ToolKind::Hatch);
    }
    ~Fixture() { saveSnapshot(view); }

    QPoint at(double x, double y) const { return view.toScreen({x, y}).toPoint(); }
    void click(double x, double y, Qt::KeyboardModifiers mods = {})
    {
        QTest::mouseMove(&view, at(x, y));
        QTest::mouseClick(&view, Qt::LeftButton, mods, at(x, y));
    }
    HatchTool& tool() const { return *view.hatchTool(); }
    template <typename T>
    T* input(const char* name) const
    {
        T* w = tool().panel()->findChild<T*>(QLatin1String(name));
        REQUIRE(w);
        return w;
    }
    const Sketch& sketch() const { return doc.sketch(); }
    const Fill& fillAt(double x, double y) const
    {
        for (const auto& [id, f] : sketch().fills()) {
            if (const auto r = regions::resolve(regions::Arrangements(sketch()), f); r && r->contains({x, y})) {
                return f;
            }
        }
        FAIL("no fill there");
        throw;
    }
    // Typed in, as a person would, then left (what leaving the box does: its text taken).
    void setNumber(const char* name, double v)
    {
        auto* box = input<QDoubleSpinBox>(name);
        box->setFocus();
        box->selectAll();
        QTest::keyClicks(box, QString::number(v));
        box->interpretText();
    }
};
}  // namespace

TEST_CASE("Hatch tool: hatching areas, and changing them all from the panel", "[hatch]")
{
    Fixture f;
    REQUIRE(f.view.toolKind() == ToolKind::Hatch);
    CHECK(f.input<QComboBox>("hatchPattern")->currentText().startsWith(QStringLiteral("ANSI31")));

    // A click hatches the area (in Colour 2), and lists it; a second area too.
    f.click(20, 20);
    REQUIRE(f.sketch().fills().size() == 1);
    CHECK(f.fillAt(20, 20).hatch == *hatch::builtin("ANSI31"));
    CHECK(f.fillAt(20, 20).color == f.view.paint().color2);
    f.click(60, 20);
    CHECK(f.tool().items().size() == 2);
    CHECK(f.recorded.last().contains(QStringLiteral("hatch=setsquare.hatch(\"ANSI31\")")));

    // The panel changes them both: pattern, angle, scale, width.
    auto* pattern = f.input<QComboBox>("hatchPattern");
    const int net = pattern->findText(QStringLiteral("NET —"), Qt::MatchStartsWith);
    REQUIRE(net >= 0);
    pattern->setCurrentIndex(net);
    emit pattern->activated(net);
    f.setNumber("hatchAngle", 30);
    f.setNumber("hatchScale", 2);
    auto* width = f.input<QComboBox>("hatchWidth");
    width->setEditText(QStringLiteral("0.4 mm"));
    emit width->lineEdit()->editingFinished();
    for (double x : {20.0, 60.0}) {
        const Hatch& h = f.fillAt(x, 20).hatch;
        CHECK(h.pattern == "NET");
        CHECK(h.angle == 30);
        CHECK(h.scale == 2);
        CHECK(h.width == 0.4);
    }
    // A palette colour recolours the listed areas.
    CHECK(f.tool().takeColor({0x22, 0xb1, 0x4c, 255}, true));
    CHECK(f.fillAt(60, 20).color == Color{0x22, 0xb1, 0x4c, 255});

    // Clicked again: left out (keeping its hatch); the panel's changes don't reach it any more.
    f.click(60, 20);
    CHECK(f.tool().items().size() == 1);
    f.setNumber("hatchAngle", 45);
    CHECK(f.fillAt(20, 20).hatch.angle == 45);
    CHECK(f.fillAt(60, 20).hatch.angle == 30);

    // The arrows step it (no Enter needed).
    auto* scale = f.input<QDoubleSpinBox>("hatchScale");
    scale->setFocus();
    QTest::keyClick(scale, Qt::Key_Up);
    CHECK(f.fillAt(20, 20).hatch.scale == 3);

    // Typed, then Enter: it's taken, and the tool's done.
    auto* angle = f.input<QDoubleSpinBox>("hatchAngle");
    angle->setFocus();
    angle->selectAll();
    QTest::keyClicks(angle, QStringLiteral("60"));
    QTest::keyClick(angle, Qt::Key_Return);
    CHECK(f.fillAt(20, 20).hatch.angle == 60);
    CHECK(f.view.toolKind() == ToolKind::Select);
}

TEST_CASE("Hatch tool: picking a hatch up, taking it off, double-click to edit", "[hatch]")
{
    Fixture f;
    f.click(20, 20);
    f.setNumber("hatchAngle", 15);
    QTest::keyClick(&f.view, Qt::Key_Escape);  // (the list emptied)
    REQUIRE(f.tool().items().empty());
    f.setNumber("hatchAngle", 70);  // (nothing listed: just the panel's)
    CHECK(f.fillAt(20, 20).hatch.angle == 15);

    // A click on a hatched area with nothing listed picks it up.
    f.click(20, 20);
    CHECK(f.tool().items().size() == 1);
    CHECK(f.input<QDoubleSpinBox>("hatchAngle")->value() == 15);
    // Ctrl+click: filled solid again (its colour kept).
    f.click(20, 20, Qt::ControlModifier);
    CHECK(!f.fillAt(20, 20).hatch.active());
    CHECK(f.recorded.last().endsWith(QStringLiteral(".hatch = None")));

    // A hatch from a file (a pattern of its own) shows as its own.
    Hatch own;
    own.pattern = "TILES";
    own.lines = {{0, 0, 0, 0, 5, {4, -1}}};
    f.view.setTool(ToolKind::Select);
    REQUIRE(f.view.tryEdit("own", [&](Editor& ed) { ed.fill({60, 20}, {200, 200, 200, 255}, {}, own); }));
    // Double-click it with the select tool: into the hatch tool, editing it.
    const QPoint p = f.at(60, 20);
    QTest::mouseDClick(&f.view, Qt::LeftButton, {}, p);
    REQUIRE(f.view.toolKind() == ToolKind::Hatch);
    CHECK(f.tool().items().size() == 1);
    CHECK(f.input<QComboBox>("hatchPattern")->currentText() == QStringLiteral("TILES (its own)"));
    f.setNumber("hatchScale", 3);  // (keeping its own lines)
    CHECK(f.fillAt(60, 20).hatch.lines == own.lines);
    CHECK(f.fillAt(60, 20).hatch.scale == 3);
}
