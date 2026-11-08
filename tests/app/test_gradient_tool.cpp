// The gradient tool: its panel, the sketch geometry that places a gradient (picked on the canvas),
// painting fills, strokes and text, and the colour bar feeding its stops.

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
#include <QLineEdit>
#include <QFormLayout>
#include <QLabel>
#include <QDoubleSpinBox>
#include <QSpinBox>
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
    Id corner = kNoId, opposite = kNoId;  // (10, 10) and (40, 30)
    Id circle = kNoId;                    // construction, round (25, 20), radius 8
    Id focal = kNoId;                     // a lone point at (22, 23)

    Fixture()
    {
        view.resize(900, 650);
        view.show();
        view.resetView();
        QObject::connect(&doc, &Document::recorded, [this](const QString& py) { recorded << py; });
        doc.modify("geometry", [&](Sketch& s) {
            corner = s.addPoint(10, 10);
            opposite = s.addPoint(40, 30);
            rect = build::rectangle(s, corner, opposite);
            circle = s.addCircle(s.addPoint(25, 20), 8);
            s.entity(circle).construction = true;
            focal = s.addPoint(22, 23);
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
    GradientTool& tool() const { return *view.gradientTool(); }
    template <typename T>
    T* input(const char* name) const
    {
        return tool().panel()->findChild<T*>(QLatin1String(name));
    }
    void pickLinear()  // start and end: the rectangle's corners
    {
        click(10, 10);
        click(40, 30);
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
    for (const char* name : {"gradientStart", "gradientEnd", "gradientShape", "gradientFocal", "gradientItems"}) {
        CHECK(f.input<SelectionButton>(name));
    }
    CHECK(f.input<GradientEditor>("gradientStops"));
    CHECK(f.input<QDoubleSpinBox>("stopPosition"));
    for (const char* name : {"stopColor", "gradientReverse", "gradientFromPaint"}) {
        CHECK(f.input<QToolButton>(name));
    }
    CHECK(!f.tool().panel()->findChild<QToolButton*>(QStringLiteral("gradientRemove")));  // (the Reset Fill tool does that)
    // Starts from Colour 1 to Colour 2, asking for its start point.
    const Gradient& g = f.view.gradient();
    REQUIRE(g.stops.size() == 2);
    CHECK(g.stops[0].color == f.view.paint().color1);
    CHECK(g.stops[1].color == f.view.paint().color2);
    CHECK(f.tool().currentInput() == GradientTool::Start);
    // Its panel goes with it.
    QPointer<CommandPanel> panel = f.tool().panel();
    f.view.setTool(ToolKind::Select);
    QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    CHECK(!panel);
}

TEST_CASE("A gradient is placed by sketch geometry picked on the canvas", "[gradient]")
{
    Fixture f;
    // Nothing to paint with until it has its points.
    f.tool().setCurrentInput(-1);
    f.click(25, 15);
    CHECK(f.sketch().fills().empty());
    CHECK(f.tool().currentInput() == GradientTool::Start);

    // Only points are taken for the ends (clicking a side's middle takes nothing).
    f.click(25, 10);
    CHECK(f.view.gradient().start == kNoId);
    f.pickLinear();
    CHECK(f.view.gradient().start == f.corner);
    CHECK(f.view.gradient().end == f.opposite);
    CHECK(f.tool().currentInput() == GradientTool::Items);

    f.click(25, 15);
    const Id fill = f.onlyFill();
    CHECK(f.fillGradient().start == f.corner);
    CHECK(f.fillGradient().end == f.opposite);
    CHECK(f.tool().current() == fill);
    const QString script = f.recorded.join('\n');
    CHECK(script.contains(QStringLiteral("color=drawapp.linear([(0, ")));
    CHECK(script.contains(QStringLiteral("start=p%1, end=p%2").arg(f.corner).arg(f.opposite)));

    // It follows the geometry.
    f.doc.beginDrag({f.opposite});
    f.doc.dragTo(30, 0);
    f.doc.endDrag();
    CHECK(gradient::place(f.fillGradient(), f.sketch())->end.x == Approx(70));

    // Radial: the circle, then (optional) a focal point; the picked fill changes with each.
    f.input<QComboBox>("gradientType")->setCurrentIndex(1);
    CHECK(f.tool().currentInput() == GradientTool::Shape);
    CHECK(f.fillGradient().kind == GradientKind::Linear);  // (not until it has its circle)
    f.click(25, 12);  // on the circle (not the point at its centre)
    CHECK(f.fillGradient().kind == GradientKind::Radial);
    CHECK(f.fillGradient().shape == f.circle);
    CHECK(f.tool().currentInput() == GradientTool::Items);
    f.input<SelectionButton>("gradientFocal")->click();
    CHECK(f.tool().currentInput() == GradientTool::Focal);
    f.click(22, 23);
    CHECK(f.fillGradient().focal == f.focal);
    const auto placed = gradient::place(f.fillGradient(), f.sketch());
    REQUIRE(placed);
    CHECK(placed->center.x == Approx(25));
    CHECK(placed->radius == Approx(8));
    CHECK(placed->focal.x == Approx(22));

    // The focal point's ×: back to the centre.
    emit f.input<SelectionButton>("gradientFocal")->cleared();
    CHECK(f.fillGradient().focal == kNoId);

    // Undo puts the fill back, and the panel follows.
    const Gradient radial = f.fillGradient();
    f.doc.undo();
    CHECK(!(f.fillGradient() == radial));
    CHECK(f.view.gradient() == f.fillGradient());

    // Ctrl+click takes it off: its plain colour again.
    f.click(25, 15, Qt::ControlModifier);
    CHECK(!f.fillGradient().active());
    CHECK(f.tool().current() == kNoId);
}

TEST_CASE("Gradient stops: the bar, the palette and the selected stop", "[gradient]")
{
    Fixture f;
    f.pickLinear();
    f.click(25, 15);
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
    f.input<QSpinBox>("stopOpacity")->setValue(50);
    CHECK(int(f.fillGradient().stops[2].color.a) == 128);
    CHECK(stops->removeStop(0));
    CHECK(!stops->removeStop(0));
    CHECK(f.fillGradient().stops.size() == 2);

    // Reverse.
    const Gradient g = f.fillGradient();
    f.input<QToolButton>("gradientReverse")->click();
    CHECK(f.fillGradient().stops.front().color == g.stops.back().color);
}

TEST_CASE("Gradient strokes: a curve, then its whole chain", "[gradient]")
{
    Fixture f;
    f.pickLinear();
    f.input<QComboBox>("gradientTarget")->setCurrentIndex(1);
    CHECK(f.tool().target() == GradientTool::Target::Stroke);
    f.click(25, 10);  // bottom side
    const Id bottom = f.rect.sides[0];
    REQUIRE(f.sketch().stroke(bottom));
    CHECK(f.sketch().stroke(bottom)->gradient == f.view.gradient());
    CHECK(f.sketch().strokes().size() == 1);

    QTest::mouseDClick(&f.view, Qt::LeftButton, {}, f.at(10, 20));  // the left side: all four
    CHECK(f.sketch().strokes().size() == 4);
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
    f.pickLinear();
    f.input<QComboBox>("gradientTarget")->setCurrentIndex(2);
    const QPolygonF box = f.view.textBox(f.sketch().text(text));
    const QPointF middle = box.boundingRect().center();
    QTest::mouseMove(&f.view, middle.toPoint());
    QTest::mouseClick(&f.view, Qt::LeftButton, {}, middle.toPoint());
    CHECK(f.sketch().text(text).hasColor);
    CHECK(f.sketch().text(text).gradient == f.view.gradient());
    f.view.setPresentation(true);
}

TEST_CASE("Double-clicking a gradient edits it in the gradient tool", "[gradient]")
{
    Fixture f;
    f.pickLinear();
    f.click(25, 15);  // a gradient fill
    const Id fill = f.onlyFill();
    f.input<QComboBox>("gradientTarget")->setCurrentIndex(1);
    f.click(25, 10);  // and a gradient stroke on the bottom side
    const Id bottom = f.rect.sides[0];
    REQUIRE(f.sketch().stroke(bottom));

    // From the select tool: the fill.
    f.view.setTool(ToolKind::Select);
    QTest::mouseDClick(&f.view, Qt::LeftButton, {}, f.at(25, 15));
    REQUIRE(f.view.toolKind() == ToolKind::Gradient);
    CHECK(f.tool().target() == GradientTool::Target::Fill);
    CHECK(f.tool().current() == fill);
    CHECK(f.tool().currentInput() == GradientTool::Items);  // (it has its geometry: clicks edit it)
    f.input<QComboBox>("gradientSpread")->setCurrentIndex(2);
    CHECK(f.fillGradient().spread == GradientSpread::Repeat);

    // The stroke.
    f.view.setTool(ToolKind::Select);
    QTest::mouseDClick(&f.view, Qt::LeftButton, {}, f.at(25, 10));
    REQUIRE(f.view.toolKind() == ToolKind::Gradient);
    CHECK(f.tool().target() == GradientTool::Target::Stroke);
    CHECK(f.tool().current() == bottom);

    // A plain fill isn't a gradient: nothing happens.
    f.view.tryEdit("plain", [&](Editor& ed) { ed.setFillGradient(fill, {}); });
    f.view.setTool(ToolKind::Select);
    QTest::mouseDClick(&f.view, Qt::LeftButton, {}, f.at(25, 15));
    CHECK(f.view.toolKind() == ToolKind::Select);
}

TEST_CASE("Reset fill takes a fill off", "[gradient][fill]")
{
    Fixture f;
    f.pickLinear();
    f.click(25, 15);
    REQUIRE(f.sketch().fills().size() == 1);
    f.view.setTool(ToolKind::ResetFill);
    f.click(60, 60);  // nothing there
    CHECK(f.sketch().fills().size() == 1);
    f.click(25, 15);
    CHECK(f.sketch().fills().empty());
    CHECK(f.recorded.join('\n').contains(QStringLiteral("s.delete(")));
    f.doc.undo();
    CHECK(f.sketch().fills().size() == 1);
}
TEST_CASE("The gradient goes on the areas listed in its panel", "[gradient]")
{
    Fixture f;
    build::Rectangle second;
    f.doc.modify("second", [&](Sketch& s) { second = build::rectangle(s, s.addPoint(50, 10), s.addPoint(70, 30)); });
    f.pickLinear();
    REQUIRE(f.tool().currentInput() == GradientTool::Items);
    SelectionButton* areas = f.input<SelectionButton>("gradientItems");
    REQUIRE(areas);
    f.click(25, 15);
    f.click(60, 15);
    REQUIRE(f.sketch().fills().size() == 2);
    CHECK(f.tool().items().size() == 2);
    CHECK(areas->text().contains(QLatin1Char('2')));

    // A change in the panel goes to both.
    f.input<QComboBox>("gradientSpread")->setCurrentIndex(1);
    for (const auto& [id, fill] : f.sketch().fills()) {
        CHECK(fill.gradient.spread == GradientSpread::Reflect);
    }
    // Clicked again: left out (it keeps its gradient, but changes no longer reach it).
    f.click(60, 15);
    CHECK(f.tool().items().size() == 1);
    f.input<QComboBox>("gradientSpread")->setCurrentIndex(2);
    int repeat = 0, reflect = 0;
    for (const auto& [id, fill] : f.sketch().fills()) {
        repeat += fill.gradient.spread == GradientSpread::Repeat;
        reflect += fill.gradient.spread == GradientSpread::Reflect;
    }
    CHECK(repeat == 1);
    CHECK(reflect == 1);
    // ×: none listed.
    emit areas->cleared();
    CHECK(f.tool().items().empty());
    // The row says what it lists.
    f.input<QComboBox>("gradientTarget")->setCurrentIndex(1);
    auto* form = f.tool().panel()->findChild<QFormLayout*>();
    REQUIRE(form);
    QWidget* field = areas->parentWidget();
    CHECK(qobject_cast<QLabel*>(form->labelForField(field))->text() == QStringLiteral("Curves"));
}

TEST_CASE("Enter closes the gradient tool", "[gradient]")
{
    Fixture f;
    f.pickLinear();
    f.click(25, 15);
    QTest::keyClick(&f.view, Qt::Key_Return);
    CHECK(f.view.toolKind() == ToolKind::Select);
    CHECK(f.sketch().fills().size() == 1);

    // And from its panel.
    f.view.setTool(ToolKind::Gradient);
    emit f.tool().panel()->accepted();
    CHECK(f.view.toolKind() == ToolKind::Select);
}

TEST_CASE("Gradient strokes: their width in the panel", "[gradient]")
{
    Fixture f;
    auto* width = f.input<QComboBox>("gradientWidth");
    CHECK(!width->isVisibleTo(f.tool().panel()));  // (fills: no width)
    f.pickLinear();
    f.input<QComboBox>("gradientTarget")->setCurrentIndex(1);
    CHECK(width->isVisibleTo(f.tool().panel()));
    f.click(25, 10);  // bottom side
    QTest::mouseDClick(&f.view, Qt::LeftButton, {}, f.at(10, 20));  // the whole outline
    REQUIRE(f.sketch().strokes().size() == 4);

    // Typed in: the outline takes it, gradients kept; new strokes get it too.
    width->lineEdit()->setText(QStringLiteral("1.5 mm"));
    emit width->lineEdit()->editingFinished();
    for (Id side : f.rect.sides) {
        REQUIRE(f.sketch().stroke(side));
        CHECK(f.sketch().stroke(side)->width == 1.5);
        CHECK(f.sketch().stroke(side)->gradient.active());
    }
    CHECK(f.view.paint().width == 1.5);
    CHECK(f.recorded.last().contains(QStringLiteral("width=1.5")));

    // Chosen from the list.
    width->setCurrentIndex(width->findData(0.5));
    emit width->activated(width->currentIndex());
    CHECK(f.sketch().stroke(f.rect.sides[2])->width == 0.5);

    // Not a size: nothing changes, and it shows what it is again.
    width->lineEdit()->setText(QStringLiteral("wide"));
    emit width->lineEdit()->editingFinished();
    CHECK(f.sketch().stroke(f.rect.sides[2])->width == 0.5);
    CHECK(width->currentText() == QStringLiteral("0.5 mm"));
    f.view.setPresentation(true);
}

TEST_CASE("Gradient strokes: a width typed in and Enter", "[gradient]")
{
    Fixture f;
    f.pickLinear();
    f.input<QComboBox>("gradientTarget")->setCurrentIndex(1);
    f.click(25, 10);  // bottom side
    const Id bottom = f.rect.sides[0];
    REQUIRE(f.sketch().stroke(bottom));
    auto* width = f.input<QComboBox>("gradientWidth");
    width->setFocus();
    width->lineEdit()->selectAll();
    QTest::keyClicks(width, QStringLiteral("1.7"));
    QTest::keyClick(width, Qt::Key_Return);
    CHECK(f.sketch().stroke(bottom)->width == 1.7);
    CHECK(f.view.toolKind() == ToolKind::Gradient);  // (Enter in the box applied it; it didn't close the panel)
    // Typed, then clicked away: applied too.
    width->setFocus();
    width->lineEdit()->selectAll();
    QTest::keyClicks(width, QStringLiteral("0.9"));
    f.view.setFocus();
    CHECK(f.sketch().stroke(bottom)->width == 0.9);
}