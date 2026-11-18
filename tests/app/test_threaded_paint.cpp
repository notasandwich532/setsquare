// The canvas painted in bands by several threads at once, and only what comes into view painted
// when it's panned: either way, just what painting it all at once, afresh, makes.

#include "Document.h"
#include "SketchView.h"

#include <core/Builders.h>
#include <core/Hatch.h>

#include <catch2/catch_test_macros.hpp>

#include <QImage>
#include <QTest>

using namespace core;

namespace
{
// Pixels that differ noticeably.
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

// A busy drawing: fills (plain, graded, hatched), a text, dimensions and constraint symbols,
// construction lines, many points.
void busyDrawing(SketchView& view, Document& doc)
{
    build::Rectangle a, b, c;
    doc.modify("shapes", [&](Sketch& s) {
        a = build::rectangle(s, s.addPoint(-90, -60), s.addPoint(-10, 0));
        b = build::rectangle(s, s.addPoint(10, -60), s.addPoint(90, 0));
        c = build::rectangle(s, s.addPoint(-90, 10), s.addPoint(90, 60));
        s.addCircle(0, 35, 18);
        for (int i = 0; i < 40; ++i) {
            s.addLine(-100 + 5 * i, -70, -95 + 5 * i, 70);  // (crossing everything)
        }
        const Id guide = s.addLine(-100, -65, 100, 65);
        s.entity(guide).construction = true;
    });
    REQUIRE(view.tryEdit("fills", [&](Editor& ed) {
        ed.fill({-50, -30}, {255, 0, 0, 255});
        const Id hatched = ed.fill({50, -30}, {0, 200, 255, 255});
        ed.setFillHatch(hatched, *hatch::builtin("ANSI31"));
        Gradient g;
        g.kind = GradientKind::Linear;
        g.stops = {{0.0, {255, 200, 0, 255}}, {1.0, {120, 0, 200, 255}}};
        g.start = c.corners[0];
        g.end = c.corners[2];
        ed.fill({-70, 50}, {255, 200, 0, 255}, g);
        ed.constrain(ConstraintType::Distance, {a.corners[0], a.corners[1]}, 80.0);
        ed.constrain(ConstraintType::Distance, {b.corners[1], b.corners[2]}, 60.0);
        Text t;
        t.text = "Bands of rows";
        t.height = 14;
        t.anchor = c.corners[0];
        ed.text(t);
    }));
}

QImage paintedWith(Document& doc, QSize size, double zoom, QPointF pan, bool presentation, int threads)
{
    SketchView view(doc);
    view.resize(size);
    view.show();
    view.setView(zoom, pan);
    view.setPresentation(presentation);
    view.setPaintThreads(threads);
    return view.grab().toImage();
}
}  // namespace

TEST_CASE("Threaded painting: in bands by several threads, just as by one", "[paint]")
{
    Document doc;
    SketchView view(doc);
    view.resize(900, 700);
    view.show();
    view.resetView();
    busyDrawing(view, doc);
    for (bool presentation : {false, true}) {
        for (double zoom : {3.0, 4.37, 9.1}) {  // (whole, and bands cutting through the text)
            const QPointF pan(450.3, 350.7);
            INFO("presentation " << presentation << ", zoom " << zoom);
            const QImage one = paintedWith(doc, view.size(), zoom, pan, presentation, 1);
            for (int threads : {2, 3, 8}) {
                INFO(threads << " threads");
                CHECK(differing(paintedWith(doc, view.size(), zoom, pan, presentation, threads), one) == 0);
            }
        }
    }
}

TEST_CASE("Threaded painting: panned, only what comes into view is painted, as painting it all afresh would", "[paint]")
{
    Document doc;
    SketchView view(doc);
    view.resize(900, 700);
    view.show();
    view.resetView();
    busyDrawing(view, doc);
    view.setView(5.0, QPointF(450.3, 350.7));
    view.grab();
    QPoint mouse(450, 350);
    QTest::mousePress(&view, Qt::MiddleButton, {}, mouse);
    for (const QPoint by : {QPoint(37, 0), QPoint(0, -23), QPoint(-61, 44), QPoint(120, -90), QPoint(-3, 2)}) {
        mouse += by;
        QTest::mouseMove(&view, mouse);
        const QImage panned = view.grab().toImage();
        INFO("panned by " << by.x() << ", " << by.y());
        CHECK(differing(panned, paintedWith(doc, view.size(), view.zoom(), view.pan(), false, 1)) == 0);
    }
    QTest::mouseRelease(&view, Qt::MiddleButton, {}, mouse);
}

TEST_CASE("Threaded painting: an edit while panned and a busy drawing painted over and over stay as painting afresh", "[paint]")
{
    Document doc;
    SketchView view(doc);
    view.resize(900, 700);
    view.show();
    view.resetView();
    busyDrawing(view, doc);
    view.setView(4.0, QPointF(450.3, 350.7));
    view.setPaintThreads(8);
    // Dragged about (each frame painted afresh by eight threads at once).
    const Id corner = [&] {
        for (const auto& [id, e] : doc.sketch().entities()) {
            if (std::holds_alternative<Point>(e.geom) && !e.fixed) {
                return id;
            }
        }
        return kNoId;
    }();
    REQUIRE(corner != kNoId);
    doc.beginDrag({corner});
    for (int i = 1; i <= 30; ++i) {
        doc.dragTo(-90 + 0.3 * i, -60 + 0.2 * i);
        view.grab();
    }
    doc.endDrag();
    CHECK(differing(view.grab().toImage(), paintedWith(doc, view.size(), view.zoom(), view.pan(), false, 1)) == 0);
}