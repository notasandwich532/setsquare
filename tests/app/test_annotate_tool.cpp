// The Annotate tool: dimensions and notes made by clicking, edited from its panel, recoloured by
// the stroke brush; selected, dragged and deleted from the Select tool.

#include "ColorBar.h"
#include "CommandPanel.h"
#include "Document.h"
#include "SketchView.h"
#include "Snapshot.h"
#include "tools/AnnotateTool.h"

#include <core/Annotations.h>
#include <core/Builders.h>

#include <catch2/catch_test_macros.hpp>

#include <QComboBox>
#include <QDoubleSpinBox>
#include <QLineEdit>
#include <QSpinBox>
#include <QTest>

using namespace core;

namespace
{
struct Fixture
{
    Document doc;
    SketchView view{doc};
    ColorBar bar{view};
    build::Rectangle box;
    Id circle = kNoId;

    Fixture()
    {
        view.resize(900, 650);
        view.show();
        view.resetView();
        doc.modify("geometry", [&](Sketch& s) {
            box = build::rectangle(s, s.addPoint(10, 10), s.addPoint(40, 30));
            circle = s.addCircle(60, 20, 5);
        });
        SketchView::Paint paint = view.paint();
        paint.color1 = Color{0x11, 0x22, 0x33, 255};
        view.setPaint(paint);
        view.setTool(ToolKind::Annotate);
        // (The tool keeps its last settings between uses: start from the usual ones.)
        tool().setKind(AnnotateTool::Kind::Auto);
        input<QDoubleSpinBox>("annotateTextHeight")->setValue(3.5);
        input<QDoubleSpinBox>("annotateWidth")->setValue(0.25);
    }
    ~Fixture() { saveSnapshot(view); }

    QPoint at(double x, double y) const { return view.toScreen({x, y}).toPoint(); }
    void click(double x, double y, Qt::KeyboardModifiers mods = {})
    {
        QTest::mouseMove(&view, at(x, y));
        QTest::mouseClick(&view, Qt::LeftButton, mods, at(x, y));
    }
    AnnotateTool& tool() const { return *view.annotateTool(); }
    template <typename T>
    T* input(const char* name) const
    {
        T* w = tool().panel()->findChild<T*>(QLatin1String(name));
        REQUIRE(w);
        return w;
    }
    const Sketch& sketch() const { return doc.sketch(); }
    const DrawnDimension& only() const
    {
        REQUIRE(sketch().drawnDimensions().size() == 1);
        return sketch().drawnDimensions().begin()->second;
    }
    std::string label(const DrawnDimension& d) const { return annotation::label(sketch(), d); }
    // Where its text is (to click it).
    Vec2 textAt(const DrawnDimension& d) const { return annotation::graphics(sketch(), d).text.at; }
};
}  // namespace

TEST_CASE("Annotate: pick, then place; in Colour 1, with the panel's settings", "[annotate]")
{
    Fixture f;
    f.click(25, 10);  // the bottom side
    CHECK(f.sketch().drawnDimensions().empty());  // (not yet: where does it go?)
    f.click(25, 3);   // below it (clear of the x axis, which it would take as a second pick)
    const DrawnDimension& d = f.only();
    CHECK((d.kind == DrawnDimensionKind::Horizontal || d.kind == DrawnDimensionKind::Aligned));
    CHECK(f.label(d) == "30.00");
    CHECK(d.style.color == (Color{0x11, 0x22, 0x33, 255}));
    CHECK(d.textHeight == 3.5);
    CHECK(f.tool().current() == d.id);  // (the panel now edits it)

    // The panel changes it: decimals, its text, sizes.
    f.input<QComboBox>("annotateDecimals")->setCurrentIndex(2);  // 1 place
    CHECK(f.label(f.only()) == "30.0");
    auto* text = f.input<QLineEdit>("annotateText");
    text->setFocus();
    QTest::keyClicks(text, "<> TYP");
    CHECK(f.label(f.only()) == "30.0 TYP");
    f.input<QDoubleSpinBox>("annotateTextHeight")->setValue(5);
    f.input<QDoubleSpinBox>("annotateWidth")->setValue(0.5);
    CHECK(f.only().textHeight == 5);
    CHECK(f.only().style.width == 0.5);

    // The drawing's decimals: for the ones that don't say.
    f.input<QComboBox>("annotateDecimals")->setCurrentIndex(0);
    f.input<QSpinBox>("annotateDrawingDecimals")->setValue(3);
    CHECK(f.sketch().dimensionDecimals() == 3);
    CHECK(f.label(f.only()) == "30.000 TYP");
}

TEST_CASE("Annotate: circles, kinds from the panel, leaders", "[annotate]")
{
    Fixture f;
    f.click(65, 20);  // the circle
    f.click(75, 30);
    CHECK(f.label(f.only()) == "\xC3\x98" "10.00");  // a diameter
    f.doc.undo();

    f.tool().setKind(AnnotateTool::Kind::Radius);
    f.click(65, 20);
    f.click(75, 30);
    CHECK(f.label(f.only()) == "R5.00");
    f.doc.undo();

    f.tool().setKind(AnnotateTool::Kind::OrdinateX);
    f.click(40, 30);  // a corner, from the origin
    f.click(40, 45);
    CHECK(f.label(f.only()) == "40.00");
    f.doc.undo();

    // A leader: where it points, then where its note goes; then the note is typed.
    f.tool().setKind(AnnotateTool::Kind::Leader);
    f.click(10, 30);  // a corner
    f.click(0, 45);
    CHECK(f.only().kind == DrawnDimensionKind::Leader);
    CHECK(f.label(f.only()) == "Note");
    auto* text = f.input<QLineEdit>("annotateText");
    CHECK(text->hasFocus());
    QTest::keyClicks(text, "Polish this");
    CHECK(f.label(f.only()) == "Polish this");
    // (The typing is one undo step, and one recorded command.)
    const QStringList history = f.doc.history();
    CHECK(history.filter(QStringLiteral("edit_annotation")).size() == 1);
    f.doc.undo();
    CHECK(f.label(f.only()) == "Note");

    // What a kind can't measure isn't picked.
    f.doc.undo();
    f.tool().setKind(AnnotateTool::Kind::ArcLength);
    f.click(25, 10);  // a line: no
    f.click(25, 3);
    CHECK(f.sketch().drawnDimensions().empty());
}

TEST_CASE("Drawn dimensions: the stroke brush recolours them; Select picks, drags and deletes them", "[annotate]")
{
    Fixture f;
    f.click(25, 10);
    f.click(25, 3);
    const Id id = f.only().id;

    SketchView::Paint paint = f.view.paint();
    paint.color1 = Color{0xff, 0, 0, 255};
    paint.width = 0.7;
    f.view.setPaint(paint);
    f.view.setTool(ToolKind::StrokeBrush);
    const Vec2 t = f.textAt(f.only());
    f.click(t.x, t.y + 1);
    CHECK(f.only().style.color == (Color{0xff, 0, 0, 255}));
    CHECK(f.only().style.width == 0.7);

    f.view.setTool(ToolKind::Select);
    f.click(t.x, t.y + 1);
    CHECK(f.view.selection() == std::set<Id>{id});
    // Dragged: its label moves (what it measures doesn't).
    const Vec2 before = annotation::graphics(f.sketch(), f.only()).label;
    QTest::mousePress(&f.view, Qt::LeftButton, {}, f.at(t.x, t.y + 1));
    QTest::mouseMove(&f.view, f.at(t.x, t.y - 4));
    QTest::mouseMove(&f.view, f.at(t.x, t.y - 9));
    QTest::mouseRelease(&f.view, Qt::LeftButton, {}, f.at(t.x, t.y - 9));
    const Vec2 after = annotation::graphics(f.sketch(), f.only()).label;
    CHECK(after.y < before.y - 5);
    CHECK(f.label(f.only()) == "30.00");
    // Double-click: into the tool, editing it.
    const Vec2 t2 = f.textAt(f.only());
    QTest::mouseDClick(&f.view, Qt::LeftButton, {}, f.at(t2.x, t2.y + 1));
    REQUIRE(f.view.toolKind() == ToolKind::Annotate);
    CHECK(f.tool().current() == id);
    // Deleted.
    f.view.setTool(ToolKind::Select);
    f.view.setSelection({id});
    f.view.deleteSelection();
    CHECK(f.sketch().drawnDimensions().empty());
}
