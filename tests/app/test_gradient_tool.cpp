// The gradient tool: its panel, painting fills, strokes and text, dragging out a direction,
// handles, and the colour bar feeding its stops.

#include "ColorBar.h"
#include "CommandPanel.h"
#include "Document.h"
#include "GradientEditor.h"
#include "SketchView.h"
#include "Snapshot.h"
#include "tools/GradientTool.h"

#include <core/Builders.h>
#include <core/Gradient.h>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <QComboBox>
#include <QDoubleSpinBox>
#include <QTest>
#include <QToolButton>

using namespace core;
using Catch::Approx;

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
        view.setTool(ToolKind::Gradient);
    }
    ~Fixture() { saveSnapshot(view); }

    QPoint at(double x, double y) const { return view.toScreen({x, y}).toPoint(); }
    void move(double x, double y) { QTest::mouseMove(&view, at(x, y)); }
    void click(double x, double y, Qt::KeyboardModifiers mods = {})
    {
        move(x, y);
        QTest::mouseClick(&view, Qt::LeftButton, mods, at(x, y));
    }
    void drag(double x0, double y0, double x1, double y1)
    {
        move(x0, y0);
        QTest::mousePress(&view, Qt::LeftButton, {}, at(x0, y0));
        for (int i = 1; i <= 6; ++i) {
            move(x0 + (x1 - x0) * i / 6.0, y0 + (y1 - y0) * i / 6.0);
        }
        QTest::mouseRelease(&view, Qt::LeftButton, {}, at(x1, y1));
    }
    GradientTool& tool() const { return *view.gradientTool(); }
    template <typename T>
    T* input(const char* name) const
    {
        return tool().panel()->findChild<T*>(QLatin1String(name));
    }
    const Sketch& sketch() const { return doc.sketch(); }
    Id onlyFill() const
    {
        REQUIRE(sketch().fills().size() == 1);
        return sketch().fills().begin()->first;
    }
    const Gradient& fillGradient() const { return sketch().fill(onlyFill()).gradient; }
    static Color hex(const char* text)
    {
        Color c;
        Color::fromHex(text, c);
        return c;
    }
};

}  // namespace

TEST_CASE("The gradient tool has its own panel", "[gradient]")
{
    Fixture f;
    REQUIRE(f.view.gradientTool());
    REQUIRE(f.tool().panel());
    for (const char* name : {"gradientTarget", "gradientType", "gradientSpread"}) {
        CHECK(f.input<QComboBox>(name));
    }
    CHECK(f.input<GradientEditor>("gradientStops"));
    CHECK(f.input<QDoubleSpinBox>("stopPosition"));
    for (const char* name : {"stopColor", "gradientReverse", "gradientFromPaint", "gradientRemove"}) {
        CHECK(f.input<QToolButton>(name));
    }
    // Starts from Colour 1 to Colour 2.
    const Gradient& g = f.view.gradient();
    REQUIRE(g.stops.size() == 2);
    CHECK(g.stops[0].color == f.view.paint().color1);
    CHECK(g.stops[1].color == f.view.paint().color2);
    // Its panel goes with it.
    QPointer<CommandPanel> panel = f.tool().panel();
    f.view.setTool(ToolKind::Select);
    QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    CHECK(!panel);
}

TEST_CASE("Clicking an area fills it with the gradient; dragging sets where it runs", "[gradient]")
{
    Fixture f;
    f.click(25, 20);
    const Id fill = f.onlyFill();
    CHECK(f.fillGradient() == f.view.gradient());
    CHECK(f.tool().current() == fill);
    CHECK(f.recorded.join('\n').contains("s.fill(at="));
    CHECK(f.recorded.join('\n').contains("color=drawapp.linear("));

    // Drag across it, bottom to top: the gradient runs that way, in fractions of its box.
    f.drag(25, 12, 25, 28);
    const Gradient& g = f.fillGradient();
    CHECK(g.x1 == Approx(0.5).margin(0.02));
    CHECK(g.y1 == Approx(0.1).margin(0.02));
    CHECK(g.x2 == Approx(0.5).margin(0.02));
    CHECK(g.y2 == Approx(0.9).margin(0.02));
    CHECK(f.sketch().fills().size() == 1);

    // Radial from the panel: the picked fill changes with it.
    f.input<QComboBox>("gradientType")->setCurrentIndex(1);
    CHECK(f.fillGradient().kind == GradientKind::Radial);
    f.input<QComboBox>("gradientSpread")->setCurrentIndex(2);
    CHECK(f.fillGradient().spread == GradientSpread::Repeat);

    // Its edge handle: drag it out to the right side.
    const gradient::Placed before = gradient::place(f.fillGradient(), gradient::Frame::around({{10, 10}, {40, 30}}));
    f.drag(before.end.x, before.end.y, 40, before.center.y);
    const gradient::Placed after = gradient::place(f.fillGradient(), gradient::Frame::around({{10, 10}, {40, 30}}));
    CHECK(after.center.x == Approx(before.center.x).margin(0.01));
    CHECK(after.radius == Approx(40 - before.center.x).margin(0.3));

    // Undo puts the fill back, and the panel follows.
    const Gradient radial = f.fillGradient();
    f.doc.undo();
    CHECK(!(f.fillGradient() == radial));
    CHECK(f.view.gradient() == f.fillGradient());

    // Ctrl+click takes it off: its plain colour again.
    f.click(25, 20, Qt::ControlModifier);
    CHECK(!f.fillGradient().active());
    CHECK(f.tool().current() == kNoId);
}

TEST_CASE("Gradient stops: the bar, the palette and the selected stop", "[gradient]")
{
    Fixture f;
    f.click(25, 20);
    GradientEditor* stops = f.input<GradientEditor>("gradientStops");
    REQUIRE(stops);

    // A stop added in the middle, in the colour already there.
    const Color middle = f.fillGradient().colorAt(0.5);
    const int i = stops->addStop(0.5);
    CHECK(i == 1);
    REQUIRE(f.fillGradient().stops.size() == 3);
    CHECK(f.fillGradient().stops[1].color == middle);

    // A palette colour goes to the selected stop, not to Colour 1.
    const Color colour1 = f.view.paint().color1;
    f.bar.pickColor(Fixture::hex("#ed1c24"));
    CHECK(f.fillGradient().stops[1].color == Fixture::hex("#ed1c24"));
    CHECK(f.view.paint().color1 == colour1);

    // Moved past the last one: the order follows.
    stops->moveStop(1, 1.0);
    CHECK(f.fillGradient().stops.back().color == Fixture::hex("#ed1c24"));
    CHECK(stops->selectedStop() == 2);

    // Opacity, and removing (but never below two).
    f.tool().panel()->findChild<QSpinBox*>(QStringLiteral("stopOpacity"))->setValue(50);
    CHECK(int(f.fillGradient().stops[2].color.a) == 128);
    CHECK(stops->removeStop(0));
    CHECK(!stops->removeStop(0));
    CHECK(f.fillGradient().stops.size() == 2);

    // Reverse.
    const Gradient g = f.fillGradient();
    f.input<QToolButton>("gradientReverse")->click();
    CHECK(f.fillGradient().stops.front().color == g.stops.back().color);
}

TEST_CASE("Gradient strokes: a curve, then its whole chain sharing one box", "[gradient]")
{
    Fixture f;
    f.input<QComboBox>("gradientTarget")->setCurrentIndex(1);
    CHECK(f.tool().target() == GradientTool::Target::Stroke);
    f.click(25, 10);  // bottom side
    const Id bottom = f.rect.sides[0];
    REQUIRE(f.sketch().stroke(bottom));
    CHECK(f.sketch().stroke(bottom)->gradient == f.view.gradient());
    CHECK(f.sketch().strokes().size() == 1);

    QTest::mouseDClick(&f.view, Qt::LeftButton, {}, f.at(10, 20));  // the left side: all four
    CHECK(f.sketch().strokes().size() == 4);
    const auto frames = gradient::strokeFrames(f.sketch());
    REQUIRE(frames.size() == 4);
    for (const auto& [id, frame] : frames) {
        CHECK(frame.u.x == Approx(frames.begin()->second.u.x));
        CHECK(frame.origin.y == Approx(frames.begin()->second.origin.y));
    }
    CHECK(gradient::strokeGroup(f.sketch(), bottom).size() == 4);

    // A change made in the panel goes to the whole outline.
    f.input<QComboBox>("gradientSpread")->setCurrentIndex(1);
    for (Id side : f.rect.sides) {
        CHECK(f.sketch().stroke(side)->gradient.spread == GradientSpread::Reflect);
    }
    f.view.setPresentation(true);
}

TEST_CASE("Gradient text", "[gradient]")
{
    Fixture f;
    Id text = kNoId;
    Id anchor = kNoId;
    f.doc.modify("point", [&](Sketch& s) { anchor = s.addPoint(60, 10); });
    f.view.tryEdit("text", [&](Editor& ed) {
        core::Text t;
        t.text = "Hi";
        t.height = 10;
        t.anchor = anchor;
        text = ed.text(t);
    });
    f.input<QComboBox>("gradientTarget")->setCurrentIndex(2);
    const QPolygonF box = f.view.textBox(f.sketch().text(text));
    const QPointF middle = box.boundingRect().center();
    QTest::mouseMove(&f.view, middle.toPoint());
    QTest::mouseClick(&f.view, Qt::LeftButton, {}, middle.toPoint());
    CHECK(f.sketch().text(text).hasColor);
    CHECK(f.sketch().text(text).gradient == f.view.gradient());
    f.view.setPresentation(true);
}
