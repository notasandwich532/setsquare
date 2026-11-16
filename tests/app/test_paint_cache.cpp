// What the canvas keeps between paints (fill areas, found once and moved along with a group
// dragged round as a whole; the picture of the drawing, with what's hovered drawn over it): it
// must always show what painting everything afresh would.

#include "Document.h"
#include "SketchView.h"
#include "Snapshot.h"

#include <core/Builders.h>

#include <catch2/catch_test_macros.hpp>

#include <QImage>
#include <QTest>
#include <QWheelEvent>
#include "Theme.h"

using namespace core;

namespace
{
struct Fixture
{
    Document doc;
    SketchView view{doc};
    build::Rectangle rect;
    Id hole = kNoId;

    Fixture()
    {
        view.resize(800, 600);
        view.show();
        view.resetView();
        view.setPresentation(true);  // fills at full strength, nothing drawn over them
        doc.modify("rect", [&](Sketch& s) {
            rect = build::rectangle(s, s.addPoint(0, 0), s.addPoint(40, 20));
            hole = s.addCircle(30, 10, 4);
        });
        REQUIRE(view.tryEdit("fill", [&](Editor& ed) { ed.fill({10, 10}, {255, 0, 0, 255}); }));
    }
    ~Fixture() { saveSnapshot(view); }

    bool red(const QImage& shot, double x, double y) const
    {
        const QColor c = shot.pixelColor(view.toScreen({x, y}).toPoint());
        return c.red() > 200 && c.green() < 60 && c.blue() < 60;
    }
    // How many pixels differ from the same drawing painted by a canvas that's kept nothing.
    int differences()
    {
        SketchView fresh(doc);
        fresh.resize(view.size());
        fresh.show();
        fresh.resetView();
        fresh.setPresentation(true);
        const QImage a = view.grab().toImage(), b = fresh.grab().toImage();
        REQUIRE(a.size() == b.size());
        int n = 0;
        for (int y = 0; y < a.height(); ++y) {
            for (int x = 0; x < a.width(); ++x) {
                const QColor p = a.pixelColor(x, y), q = b.pixelColor(x, y);
                n += std::abs(p.red() - q.red()) + std::abs(p.green() - q.green()) + std::abs(p.blue() - q.blue()) > 60;
            }
        }
        return n;
    }
};
}  // namespace

TEST_CASE("Paint cache: a fill follows an edit of its outline", "[paint]")
{
    Fixture f;
    QImage shot = f.view.grab().toImage();
    CHECK(f.red(shot, 10, 10));
    CHECK(!f.red(shot, 30, 10));  // the hole
    CHECK(!f.red(shot, 50, 10));

    // One corner pulled out: the area grows (the same curves round it, but further).
    const Id corner = f.rect.corners[2];
    REQUIRE(f.doc.sketch().point(corner).x == 40.0);
    f.doc.beginDrag({corner});
    f.doc.dragTo(20, 0);
    shot = f.view.grab().toImage();
    f.doc.endDrag();
    CHECK(f.red(shot, 50, 10));
    CHECK(f.red(f.view.grab().toImage(), 50, 10));
    CHECK(f.differences() == 0);

    // The hole taken away: filled there too.
    f.doc.modify("delete", [&](Sketch& s) { s.remove(f.hole); });
    CHECK(f.red(f.view.grab().toImage(), 30, 10));
    CHECK(f.differences() == 0);
}

TEST_CASE("Paint cache: a group dragged round, its fill moved along", "[paint]")
{
    Fixture f;
    std::vector<Id> items(f.rect.sides.begin(), f.rect.sides.end());
    items.push_back(f.hole);
    REQUIRE(f.doc.addGroup(items) != kNoId);
    f.view.grab();

    // By one corner, a step at a time: it moves (and turns) as a whole.
    const Id corner = f.rect.corners[2];
    const Vec2 was{f.doc.sketch().point(corner).x, f.doc.sketch().point(corner).y};
    f.doc.beginDrag({corner});
    for (int i = 1; i <= 8; ++i) {
        f.doc.dragTo(3.0 * i, 4.0 * i);
        CHECK(f.differences() < 40);  // (rounded differently at the very edge, at most)
    }
    f.doc.endDrag();
    CHECK(f.doc.sketch().point(corner).x != was.x);
    CHECK(f.differences() < 40);
}

namespace
{
// Pixels that differ noticeably between two pictures of the same size.
int differing(const QImage& a, const QImage& b)
{
    REQUIRE(a.size() == b.size());
    int n = 0;
    for (int y = 0; y < a.height(); ++y) {
        for (int x = 0; x < a.width(); ++x) {
            const QColor p = a.pixelColor(x, y), q = b.pixelColor(x, y);
            n += std::abs(p.red() - q.red()) + std::abs(p.green() - q.green()) + std::abs(p.blue() - q.blue()) > 30;
        }
    }
    return n;
}

// The same drawing, painted by a canvas that's kept nothing, as `view` is set up.
QImage afresh(Document& doc, const SketchView& view, Id hovered)
{
    SketchView fresh(doc);
    fresh.resize(view.size());
    fresh.show();
    fresh.resetView();
    fresh.setPresentation(view.presentation());
    fresh.setSelection(view.selection());
    fresh.setHovered(hovered);
    return fresh.grab().toImage();
}

// The same, at the view's zoom and pan.
QImage afreshAsViewed(Document& doc, const SketchView& view)
{
    SketchView fresh(doc);
    fresh.resize(view.size());
    fresh.show();
    fresh.setView(view.zoom(), view.pan());
    fresh.setPresentation(view.presentation());
    fresh.setSelection(view.selection());
    return fresh.grab().toImage();
}
}  // namespace

TEST_CASE("Paint cache: hovering, selecting and editing show as painting afresh would", "[paint]")
{
    Document doc;
    SketchView view(doc);
    view.resize(800, 600);
    view.show();
    view.resetView();
    build::Rectangle rect;
    Id dimension = kNoId;
    doc.modify("rect", [&](Sketch& s) { rect = build::rectangle(s, s.addPoint(0, 0), s.addPoint(40, 20)); });
    REQUIRE(view.tryEdit("fill", [&](Editor& ed) { ed.fill({10, 10}, {255, 0, 0, 255}); }));
    REQUIRE(view.tryEdit("dimension", [&](Editor& ed) { dimension = ed.constrain(ConstraintType::Distance, {rect.corners[0], rect.corners[1]}, 40.0); }));
    const Id bottom = rect.sides[0], corner = rect.corners[2];
    auto pixel = [&](double x, double y) { return view.grab().toImage().pixelColor(view.toScreen({x, y}).toPoint()); };
    auto near = [](const QColor& a, const QColor& b) {
        return std::abs(a.red() - b.red()) + std::abs(a.green() - b.green()) + std::abs(a.blue() - b.blue()) < 40;
    };
    view.grab();

    // Hovered, one thing after another (a line, a point, a dimension), then nothing.
    for (Id hovered : {bottom, corner, dimension, kNoId}) {
        view.setHovered(hovered);
        CHECK(differing(view.grab().toImage(), afresh(doc, view, hovered)) == 0);
    }
    view.setHovered(bottom);
    CHECK(near(pixel(20, 0), theme::canvas().hover));
    view.setHovered(kNoId);
    CHECK(!near(pixel(20, 0), theme::canvas().hover));

    // Selected.
    view.setSelection({bottom});
    CHECK(near(pixel(20, 0), theme::canvas().selection));
    CHECK(differing(view.grab().toImage(), afresh(doc, view, kNoId)) == 0);
    view.setSelection({});

    // An edit while something's hovered: the picture under it is painted again.
    view.setHovered(rect.sides[2]);
    view.grab();
    doc.beginDrag({corner});
    doc.dragTo(10, 0);
    doc.endDrag();
    CHECK(differing(view.grab().toImage(), afresh(doc, view, rect.sides[2])) == 0);
    view.setHovered(kNoId);

    // Presentation view, and back.
    view.setPresentation(true);
    CHECK(differing(view.grab().toImage(), afresh(doc, view, kNoId)) == 0);
    view.setPresentation(false);
    CHECK(differing(view.grab().toImage(), afresh(doc, view, kNoId)) == 0);

    // Zoomed: not the picture from before.
    const QImage before = view.grab().toImage();
    const QPointF c(400, 300);
    QWheelEvent wheel(c, view.mapToGlobal(c), QPoint(), QPoint(0, 120), Qt::NoButton, Qt::NoModifier, Qt::NoScrollPhase, false);
    QCoreApplication::sendEvent(&view, &wheel);
    CHECK(differing(view.grab().toImage(), before) > 100);
}

TEST_CASE("Density: a spline's control polygon shows only where it can be made out (or it's selected)", "[paint]")
{
    Document doc;
    SketchView view(doc);
    view.resize(800, 600);
    view.show();
    view.resetView();
    Id spline = kNoId;
    doc.modify("spline", [&](Sketch& s) {
        Spline sp;
        sp.points = {s.addPoint(0, 0), s.addPoint(0, 40), s.addPoint(40, 40), s.addPoint(40, 0)};
        sp.kind = SplineKind::Control;
        spline = s.addSpline(sp);
    });
    // Round the middle of its polygon's top (the curve itself peaks well below it).
    auto changed = [&]() {
        const QPoint at = view.toScreen({20, 40}).toPoint();
        const QRect box(at - QPoint(2, 2), QSize(5, 5));
        view.setSelection({});
        const QImage plain = view.grab().toImage().copy(box);
        view.setSelection({spline});
        const QImage selected = view.grab().toImage().copy(box);
        view.setSelection({});
        return differing(plain, selected);
    };
    CHECK(changed() == 0);  // big enough: shown either way
    for (int n = 0; n < 40 && view.zoom() * 40 >= 20; ++n) {
        const QPointF c(view.toScreen({20, 20}));
        QWheelEvent wheel(c, view.mapToGlobal(c), QPoint(), QPoint(0, -120), Qt::NoButton, Qt::NoModifier, Qt::NoScrollPhase, false);
        QCoreApplication::sendEvent(&view, &wheel);
    }
    REQUIRE(view.zoom() * 40 < 20);
    CHECK(changed() > 0);  // too small to show, but selected it does
}

TEST_CASE("Paint cache: a text, drawn from a picture kept of it, shows as painting it afresh would", "[paint]")
{
    Document doc;
    SketchView view(doc);
    view.resize(800, 600);
    view.show();
    view.resetView();
    Id anchor = kNoId, other = kNoId, text = kNoId;
    doc.modify("points", [&](Sketch& s) {
        anchor = s.addPoint(-40, 0);
        other = s.addLine(-40, -30, 40, -30);
    });
    REQUIRE(view.tryEdit("text", [&](Editor& ed) {
        Text t;
        t.text = "Setsquare";
        t.height = 12;
        t.anchor = anchor;
        text = ed.text(t);
    }));
    REQUIRE(differing(view.grab().toImage(), afresh(doc, view, kNoId)) == 0);

    // Hovered, selected, and back: each its own picture.
    view.setHovered(text);
    CHECK(differing(view.grab().toImage(), afresh(doc, view, text)) == 0);
    view.setHovered(kNoId);
    view.setSelection({text});
    CHECK(differing(view.grab().toImage(), afresh(doc, view, kNoId)) == 0);
    view.setSelection({});
    CHECK(differing(view.grab().toImage(), afresh(doc, view, kNoId)) == 0);

    // Something else dragged (the text's picture as it was), then the text itself (a new one).
    const Id end = doc.sketch().line(other).p2;
    doc.beginDrag({end});
    doc.dragTo(5, 7);
    doc.endDrag();
    CHECK(differing(view.grab().toImage(), afresh(doc, view, kNoId)) == 0);
    doc.beginDrag({anchor});
    doc.dragTo(3.3, 1.7);
    doc.endDrag();
    CHECK(differing(view.grab().toImage(), afresh(doc, view, kNoId)) == 0);

    // Scrolled (by the middle button): the same, moved by as much.
    const QPoint from(400, 300), by(37, 23);
    QTest::mousePress(&view, Qt::MiddleButton, {}, from);
    QTest::mouseMove(&view, from + by);
    QTest::mouseRelease(&view, Qt::MiddleButton, {}, from + by);
    const QImage scrolled = view.grab().toImage(), fresh = afresh(doc, view, kNoId);
    const double ratio = scrolled.devicePixelRatio();
    const QImage shifted = scrolled.copy(QRect(QPoint(by.x() * ratio, by.y() * ratio), fresh.size() - QSize(by.x() * ratio, by.y() * ratio)));
    CHECK(differing(shifted, fresh.copy(QRect(QPoint(0, 0), shifted.size()))) == 0);
}

TEST_CASE("Paint cache: zooming a slow drawing by the wheel shows the picture stretched, then sharp once the wheel stops", "[paint]")
{
    Document doc;
    SketchView view(doc);
    view.resize(800, 600);
    view.show();
    view.resetView();
    doc.modify("rect", [&](Sketch& s) { build::rectangle(s, s.addPoint(-40, -20), s.addPoint(40, 20)); });
    REQUIRE(view.tryEdit("fill", [&](Editor& ed) { ed.fill({0, 0}, {255, 0, 0, 255}); }));
    const QImage before = view.grab().toImage();
    auto wheel = [&](int notches) {
        const QPointF c(430, 280);  // (off centre: zoomed about the mouse)
        QWheelEvent e(c, view.mapToGlobal(c), QPoint(), QPoint(0, 120 * notches), Qt::NoButton, Qt::NoModifier,
                      Qt::NoScrollPhase, false);
        QCoreApplication::sendEvent(&view, &e);
    };

    SECTION("A quick drawing: painted sharp every time")
    {
        wheel(1);
        CHECK(differing(view.grab().toImage(), afreshAsViewed(doc, view)) == 0);
        CHECK(!view.showingQuickZoom());
    }
    SECTION("A slow one: stretched, where painting afresh puts it")
    {
        // Where the red fill shows (its box, on screen).
        auto redBox = [](const QImage& image) {
            QRect box;
            for (int y = 0; y < image.height(); ++y) {
                for (int x = 0; x < image.width(); ++x) {
                    const QColor p = image.pixelColor(x, y);
                    if (p.red() - p.green() > 50 && p.red() - p.blue() > 50) {
                        box |= QRect(x, y, 1, 1);
                    }
                }
            }
            return box;
        };
        auto near = [](const QRect& a, const QRect& b) {
            return std::abs(a.left() - b.left()) <= 2 && std::abs(a.top() - b.top()) <= 2 && std::abs(a.right() - b.right()) <= 2
                && std::abs(a.bottom() - b.bottom()) <= 2;
        };
        view.setQuickZoomFrom(0.0);  // (as if it were slow)
        double pictured = view.zoom();  // the zoom of the picture being stretched
        for (int notches : {1, 2, -1, 4, 3, -5}) {
            wheel(notches);
            const QImage shown = view.grab().toImage();
            const double k = view.zoom() / pictured;
            const bool stretched = k <= 2.0 && k >= 0.5;  // (past twice or half: a new picture)
            if (!stretched) {
                pictured = view.zoom();
            }
            INFO("after " << notches << " notches, zoom " << view.zoom());
            CHECK(view.showingQuickZoom() == stretched);
            const QImage fresh = afreshAsViewed(doc, view);
            CHECK(!redBox(fresh).isEmpty());
            CHECK(near(redBox(shown), redBox(fresh)));
            if (!stretched) {
                CHECK(differing(shown, fresh) == 0);
            }
        }        CHECK(differing(view.grab().toImage(), before) > 1000);  // (it has zoomed)
        // The wheel stopped: painted sharp.
        QTest::qWait(400);
        const QImage sharp = view.grab().toImage();
        CHECK(!view.showingQuickZoom());
        CHECK(differing(sharp, afreshAsViewed(doc, view)) == 0);
    }
}
