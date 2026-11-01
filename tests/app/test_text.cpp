// Text: letter shapes from fonts (outline modes, minimum thickness), the Text tool with its font
// search, and moving/editing text on the canvas.

#include "CommandPanel.h"
#include "Document.h"
#include "FontPicker.h"
#include "SketchView.h"
#include "Snapshot.h"
#include "TextShapes.h"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <QCoreApplication>
#include <QDoubleSpinBox>
#include <QLineEdit>
#include <QListWidget>
#include <QPushButton>
#include <QTest>

#include <algorithm>

using namespace core;
using Catch::Approx;

namespace
{
Text sample(const std::string& text, TextOutline outline = TextOutline::AsDesigned, double minThickness = 0.0)
{
    Text t;
    t.text = text;
    t.font = textshapes::defaultFamily().toStdString();
    t.height = 10;
    t.outline = outline;
    t.minThickness = minThickness;
    return t;
}

struct Bounds
{
    double x0 = 1e300, y0 = 1e300, x1 = -1e300, y1 = -1e300;
};
Bounds boundsOf(const std::vector<std::vector<Point>>& shape)
{
    Bounds b;
    for (const auto& c : shape) {
        for (const Point& p : c) {
            b.x0 = std::min(b.x0, p.x);
            b.y0 = std::min(b.y0, p.y);
            b.x1 = std::max(b.x1, p.x);
            b.y1 = std::max(b.y1, p.y);
        }
    }
    return b;
}

struct Fixture
{
    Document doc;
    SketchView view{doc};
    QStringList recorded;

    Fixture()
    {
        view.resize(1000, 700);
        view.show();
        view.resetView();
        QObject::connect(&doc, &Document::recorded, [this](const QString& py) { recorded << py; });
    }
    ~Fixture() { saveSnapshot(view); }

    QPoint at(double x, double y) const { return view.toScreen({x, y}).toPoint(); }
    void settle()
    {
        QCoreApplication::processEvents();
        QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    }
    template<typename T>
    T* field(const char* name)
    {
        settle();
        auto* panel = view.findChild<CommandPanel*>("commandPanel");
        REQUIRE(panel != nullptr);
        T* w = panel->findChild<T*>(name);
        REQUIRE(w != nullptr);
        return w;
    }
    Id onlyText() const
    {
        REQUIRE(doc.sketch().texts().size() == 1);
        return doc.sketch().texts().begin()->first;
    }
};
}  // namespace

TEST_CASE("Letters come from the font at the asked height", "[text]")
{
    REQUIRE_FALSE(textshapes::families().isEmpty());  // (the tests need some font installed)
    const auto shape = textshapes::make(sample("H"));
    REQUIRE_FALSE(shape.empty());
    const Bounds b = boundsOf(shape);
    CHECK(b.y0 == Approx(0.0).margin(0.05));  // on the baseline
    CHECK(b.y1 == Approx(10.0).margin(0.15));  // capital height
    CHECK(textshapes::make(sample("")).empty());
}

TEST_CASE("Outline modes: merged keeps holes, silhouette keeps only the outside", "[text]")
{
    // "o" is an outline and a hole; "8" an outline and two holes.
    CHECK(textshapes::make(sample("o", TextOutline::Merged)).size() == 2);
    CHECK(textshapes::make(sample("o", TextOutline::Silhouette)).size() == 1);
    CHECK(textshapes::make(sample("8", TextOutline::Merged)).size() == 3);
    CHECK(textshapes::make(sample("8", TextOutline::Silhouette)).size() == 1);
    CHECK(textshapes::make(sample("oo8", TextOutline::Silhouette)).size() == 3);  // one per letter
}

TEST_CASE("Minimum thickness widens narrow strokes to it, and leaves wide enough ones alone", "[text]")
{
    const Bounds plain = boundsOf(textshapes::make(sample("l")));
    const double stroke = plain.x1 - plain.x0;
    REQUIRE(stroke < 2.0);

    // Already thicker than this: untouched.
    const auto same = textshapes::make(sample("l", TextOutline::AsDesigned, stroke * 0.8));
    CHECK(boundsOf(same).x1 - boundsOf(same).x0 == Approx(stroke).margin(1e-6));

    // Thinner: made about 3 mm wide (at least that; a grid's worth over at most).
    const Bounds thick = boundsOf(textshapes::make(sample("l", TextOutline::AsDesigned, 3.0)));
    CHECK(thick.x1 - thick.x0 >= 3.0 - 0.05);
    CHECK(thick.x1 - thick.x0 <= 3.6);
    CHECK(thick.y1 - thick.y0 < 10.0 + 3.0);  // the ends grow less than the full width
}

TEST_CASE("Text tool: click a place, type, search for a font, OK", "[text][tool]")
{
    Fixture f;
    f.view.setTool(ToolKind::Text);
    QTest::mouseMove(&f.view, f.at(10, 20));
    QTest::mouseClick(&f.view, Qt::LeftButton, {}, f.at(10, 20));
    auto* text = f.field<QLineEdit>("text");
    text->selectAll();
    QTest::keyClicks(text, "Hi there");
    f.field<QDoubleSpinBox>("height")->setValue(8);

    // Search for a font: the list shows only matches, and picking one uses it.
    const QString family = textshapes::defaultFamily();
    auto* search = f.field<QLineEdit>("fontSearch");
    QTest::keyClicks(search, family.left(4).toLower());
    auto* fonts = f.field<QListWidget>("fonts");
    REQUIRE(fonts->count() >= 1);
    for (int i = 0; i < fonts->count(); ++i) {
        CHECK(fonts->item(i)->text().contains(family.left(4), Qt::CaseInsensitive));
    }
    const auto exact = fonts->findItems(family, Qt::MatchExactly);
    REQUIRE_FALSE(exact.isEmpty());
    fonts->setCurrentItem(exact.first());
    saveSnapshot(f.view, QStringLiteral("-panel"));

    f.field<QPushButton>("ok")->click();
    f.settle();
    const Text& t = f.doc.sketch().text(f.onlyText());
    CHECK(t.text == "Hi there");
    CHECK(QString::fromStdString(t.font) == family);
    CHECK(t.height == Approx(8.0));
    CHECK_FALSE(t.shape.empty());
    const Point& anchor = f.doc.sketch().point(t.anchor);
    CHECK(anchor.x == Approx(10.0).margin(0.5));
    CHECK(anchor.y == Approx(20.0).margin(0.5));
    REQUIRE_FALSE(f.recorded.isEmpty());
    CHECK(f.recorded.last().contains("s.text(\"Hi there\""));
}

TEST_CASE("Text: drag it by its letters; double-click to edit; Delete removes it", "[text][tool]")
{
    Fixture f;
    Id id = kNoId;
    f.doc.modify("text", [&](Sketch& s) {
        Text t = sample("Move me");
        t.anchor = s.addPoint(0, 0);
        textshapes::update(t);
        id = s.addText(t);
    });
    const Id anchor = f.doc.sketch().text(id).anchor;
    // On a letter's box: it's the text that's picked.
    const QPoint grab = f.at(5, 4);
    CHECK(f.view.pickAt(QPointF(grab)) == id);
    QTest::mousePress(&f.view, Qt::LeftButton, {}, grab);
    for (int i = 1; i <= 4; ++i) {
        QTest::mouseMove(&f.view, grab + QPoint(10 * i, -5 * i));
    }
    QTest::mouseRelease(&f.view, Qt::LeftButton, {}, grab + QPoint(40, -20));
    f.settle();
    CHECK(f.doc.sketch().point(anchor).x == Approx(40.0 / f.view.zoom()).margin(0.3));
    CHECK(f.doc.sketch().point(anchor).y == Approx(20.0 / f.view.zoom()).margin(0.3));

    // Double-click: the Text tool, on it.
    const Point& a = f.doc.sketch().point(anchor);
    const QPoint letters = f.at(a.x + 5, a.y + 4);
    QTest::mouseDClick(&f.view, Qt::LeftButton, {}, letters);
    f.settle();
    CHECK(f.view.toolKind() == ToolKind::Text);
    auto* text = f.field<QLineEdit>("text");
    CHECK(text->text() == "Move me");
    text->setText("Moved");
    f.field<QPushButton>("ok")->click();
    f.settle();
    CHECK(f.doc.sketch().text(id).text == "Moved");
    CHECK(f.recorded.last().startsWith(QStringLiteral("s.edit_text(t%1").arg(id)));

    f.view.setSelection({id});
    f.view.deleteSelection();
    CHECK(f.doc.sketch().texts().empty());
    CHECK_FALSE(f.doc.sketch().hasEntity(anchor));
}
