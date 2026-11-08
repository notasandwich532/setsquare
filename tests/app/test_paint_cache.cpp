// What the canvas keeps between paints (fill areas, found once and moved along with a group
// dragged round as a whole): it must always show what finding them afresh would.

#include "Document.h"
#include "SketchView.h"
#include "Snapshot.h"

#include <core/Builders.h>

#include <catch2/catch_test_macros.hpp>

#include <QImage>

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
