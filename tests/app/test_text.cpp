// Text: letter shapes from fonts (outline modes, minimum thickness), the Text tool with its font
// search, and moving/editing text on the canvas.

#include "CommandPanel.h"
#include "Document.h"
#include "FontPicker.h"
#include "SketchView.h"
#include "Snapshot.h"
#include "TextShapes.h"

#include <core/Serialize.h>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <QComboBox>
#include <QCoreApplication>
#include <QDoubleSpinBox>
#include <QLineEdit>
#include <QListWidget>
#include <QPlainTextEdit>
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

TEST_CASE("Thickened letters are smooth curves, not a traced outline's steps", "[text]")
{
    // Strokes made thicker (and the letters' own outline where it's still the edge) are curves:
    // a few dozen pieces a letter, mostly curved, not hundreds of short straight lines.
    for (const char* letters : {"Rag 8", "S", "o"}) {
        INFO(letters);
        const auto contours = textshapes::makeLetters(sample(letters, TextOutline::AsDesigned, 1.2));
        REQUIRE_FALSE(contours.empty());
        size_t curves = 0, lines = 0;
        for (const TextContour& c : contours) {
            curves += static_cast<size_t>(std::ranges::count(c.ops, 'C'));
            lines += static_cast<size_t>(std::ranges::count(c.ops, 'L'));
        }
        CHECK(curves > 3 * lines);
        CHECK(curves + lines < 160 * std::string(letters).size());
    }
    // An "o" thickened is still a ring: an outline and its hole.
    CHECK(textshapes::makeLetters(sample("o", TextOutline::AsDesigned, 1.2)).size() == 2);
}

TEST_CASE("Text tool: drag out a box, type, search for a font, OK", "[text][tool]")
{
    Fixture f;
    f.view.setTool(ToolKind::Text);
    QTest::mouseMove(&f.view, f.at(10, 40));
    QTest::mouseClick(&f.view, Qt::LeftButton, {}, f.at(10, 40));  // a corner (top left)
    QTest::mouseMove(&f.view, f.at(70, 20));
    QTest::mouseClick(&f.view, Qt::LeftButton, {}, f.at(70, 20));  // the opposite one
    auto* text = f.field<QPlainTextEdit>("text");
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
    // In its box: the box's bottom-left corner first, the text in its top left.
    REQUIRE(t.inBox());
    const Point& anchor = f.doc.sketch().point(t.anchor);
    const Point& far = f.doc.sketch().point(t.boxCorner);
    CHECK(anchor.x == Approx(10.0).margin(0.5));
    CHECK(anchor.y == Approx(20.0).margin(0.5));
    CHECK(far.x == Approx(70.0).margin(0.5));
    CHECK(far.y == Approx(40.0).margin(0.5));
    const Bounds b = boundsOf(t.shape);
    CHECK(anchor.y + b.y1 == Approx(40.0).margin(0.3));  // caps up against the top
    CHECK(anchor.x + b.x0 == Approx(10.0).margin(1.0));  // and against the left side
    REQUIRE_FALSE(f.recorded.isEmpty());
    CHECK(f.recorded.last().contains("s.text(\"Hi there\", box=("));
}

TEST_CASE("Box text: aligned in its box, which it follows; dragged and deleted whole", "[text][box]")
{
    Document doc;
    Id id = kNoId;
    doc.edit("text", [&](Editor& ed) {
        Sketch& s = const_cast<Sketch&>(ed.sketch());
        Text t = sample("Mid");
        t.align = TextAlign::Center;
        t.valign = TextVAlign::Middle;
        id = ed.textBox(t, s.addPoint(0, 0), s.addPoint(60, 30));
    });
    const Sketch& s = doc.sketch();
    auto centre = [&] {
        const Text& t = s.text(id);
        const Bounds b = boundsOf(t.shape);
        const Point c = textToWorld(s.point(t.anchor), t.angle, {(b.x0 + b.x1) / 2, (b.y0 + b.y1) / 2});
        return c;
    };
    CHECK(centre().x == Approx(30.0).margin(0.8));
    CHECK(centre().y == Approx(15.0).margin(0.8));
    REQUIRE(textPoints(s, s.text(id)).size() == 4);

    // Make the box bigger: the text stays in its middle.
    doc.beginDrag({s.text(id).boxCorner});
    doc.dragTo(40, 20);
    doc.endDrag();
    CHECK(centre().x == Approx(50.0).margin(0.8));
    CHECK(centre().y == Approx(25.0).margin(0.8));

    // Drag the whole of it (as dragging its letters does): box and text move together.
    const std::vector<Id> all = textPoints(s, s.text(id));
    doc.beginDrag(all);
    doc.dragTo(100, 0);
    doc.endDrag();
    CHECK(s.point(s.text(id).anchor).x == Approx(100.0).margin(1e-6));
    CHECK(s.point(s.text(id).boxCorner).x == Approx(200.0).margin(1e-6));
    CHECK(centre().x == Approx(150.0).margin(0.8));

    // Saved and loaded in its box; deleting it takes the box.
    const Sketch back = fromJson(toJson(s));
    CHECK(back.text(id).inBox());
    const size_t lines = std::ranges::count_if(s.entities(), [](const auto& kv) { return kv.second.kind() == EntityKind::Line; });
    doc.edit("delete", [&](Editor& ed) { ed.remove(id); });
    const size_t after = std::ranges::count_if(s.entities(), [](const auto& kv) { return kv.second.kind() == EntityKind::Line; });
    CHECK(lines - after == 4);
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
    auto* text = f.field<QPlainTextEdit>("text");
    CHECK(text->toPlainText() == "Move me");
    text->setPlainText("Moved");
    f.field<QPushButton>("ok")->click();
    f.settle();
    CHECK(f.doc.sketch().text(id).text == "Moved");
    CHECK(f.recorded.last().startsWith(QStringLiteral("s.edit_text(t%1").arg(id)));

    f.view.setSelection({id});
    f.view.deleteSelection();
    CHECK(f.doc.sketch().texts().empty());
    CHECK_FALSE(f.doc.sketch().hasEntity(anchor));
}

TEST_CASE("Lines, alignment, spacing and flips", "[text]")
{
    const Bounds one = boundsOf(textshapes::make(sample("Ab")));
    Text two = sample("Ab\nAb");
    const Bounds lines = boundsOf(textshapes::make(two));
    CHECK(lines.y0 < one.y0 - 5.0);  // a second line below the first
    CHECK(lines.x1 == Approx(one.x1).margin(0.01));

    // Centred / right: the anchor in the middle / at the right; top: caps hanging from it.
    Text t = sample("Ab");
    t.align = TextAlign::Center;
    const Bounds centred = boundsOf(textshapes::make(t));
    CHECK((centred.x0 + centred.x1) / 2 == Approx(0.0).margin(0.6));
    t.align = TextAlign::Right;
    CHECK(boundsOf(textshapes::make(t)).x1 == Approx(0.0).margin(0.6));
    t.align = TextAlign::Left;
    t.valign = TextVAlign::Top;
    CHECK(boundsOf(textshapes::make(t)).y1 == Approx(0.0).margin(0.2));
    t.valign = TextVAlign::Middle;
    const Bounds middle = boundsOf(textshapes::make(t));
    CHECK((middle.y0 + middle.y1) / 2 == Approx(0.0).margin(1.0));

    // Spacing spreads the letters; flips mirror them in place.
    Text spaced = sample("Ab");
    spaced.spacing = 50;
    CHECK(boundsOf(textshapes::make(spaced)).x1 > one.x1 + 2.0);
    Text flipped = sample("Ab");
    flipped.flipH = flipped.flipV = true;
    const Bounds f = boundsOf(textshapes::make(flipped));
    CHECK(f.x1 - f.x0 == Approx(one.x1 - one.x0).margin(0.01));
    CHECK(f.y1 - f.y0 == Approx(one.y1 - one.y0).margin(0.01));
}

TEST_CASE("Text along a path follows its curve", "[text][path]")
{
    Document doc;
    Id line = kNoId, circle = kNoId, onLine = kNoId, onCircle = kNoId;
    doc.edit("text", [&](Editor& ed) {
        Sketch& s = const_cast<Sketch&>(ed.sketch());
        line = s.addLine(0, 0, 100, 0);
        Text t = sample("ALONG");
        t.path = line;
        t.anchor = s.line(line).p1;
        t.align = TextAlign::Center;
        onLine = ed.text(t);
        circle = s.addCircle(200, 0, 20);
        Text c = sample("ROUND");
        c.path = circle;
        c.anchor = s.circle(circle).center;
        c.align = TextAlign::Center;
        onCircle = ed.text(c);
    });
    const Sketch& s = doc.sketch();
    auto worldBounds = [&](Id id) {
        const Text& t = s.text(id);
        Bounds b;
        for (const auto& c : t.shape) {
            for (const Point& p : c) {
                const Point w = textToWorld(s.point(t.anchor), t.angle, p);
                b.x0 = std::min(b.x0, w.x), b.y0 = std::min(b.y0, w.y), b.x1 = std::max(b.x1, w.x), b.y1 = std::max(b.y1, w.y);
            }
        }
        return b;
    };
    // On the line: standing on it, centred along it.
    Bounds b = worldBounds(onLine);
    CHECK(b.y0 == Approx(0.0).margin(0.2));
    CHECK((b.x0 + b.x1) / 2 == Approx(50.0).margin(1.0));
    // Round the circle: centred on its top, outside it.
    b = worldBounds(onCircle);
    CHECK((b.x0 + b.x1) / 2 == Approx(200.0).margin(1.0));
    CHECK(b.y1 > 20.0 + 8.0);
    CHECK(b.y0 > 0.0);

    // Drag the line's end up: the text turns with it.
    doc.beginDrag({s.line(line).p2});
    doc.dragTo(0, 100);
    doc.endDrag();
    b = worldBounds(onLine);
    CHECK(b.y1 > 40.0);  // now climbing to (100, 100)
    // Saved and loaded, still on its curve.
    const Sketch back = fromJson(toJson(s));
    CHECK(back.text(onLine).path == line);
}

TEST_CASE("Text tool: along a path", "[text][tool]")
{
    Fixture f;
    Id arc = kNoId;
    f.doc.modify("arc", [&](Sketch& s) { arc = s.addArc(0, 0, 30, 0, 180); });
    f.view.setTool(ToolKind::Text);
    f.field<QComboBox>("type")->setCurrentIndex(1);
    QTest::mouseMove(&f.view, f.at(0, 30));
    QTest::mouseClick(&f.view, Qt::LeftButton, {}, f.at(0, 30));  // the arc's top
    auto* text = f.field<QPlainTextEdit>("text");
    text->setPlainText("Over the top");
    f.field<QComboBox>("align")->setCurrentIndex(1);
    f.field<QPushButton>("ok")->click();
    f.settle();
    const Text& t = f.doc.sketch().text(f.onlyText());
    CHECK(t.path == arc);
    CHECK(t.align == TextAlign::Center);
    CHECK(f.recorded.last().contains(QStringLiteral("path=a%1").arg(arc)));
}