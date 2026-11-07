// The Project tool: hovering over anything (another layer's curves and points, this layer's, the
// origin and axes, images, empty space) is safe, and clicking projects.

#include "Document.h"
#include "Images.h"
#include "SketchView.h"
#include "Snapshot.h"

#include <core/Builders.h>

#include <catch2/catch_test_macros.hpp>

#include <QTemporaryDir>
#include <QTest>

using namespace core;

TEST_CASE("Project tool: hovering over anything is safe; clicking projects", "[project]")
{
    Document doc;
    SketchView view(doc);
    view.resize(800, 600);
    view.show();
    view.resetView();
    Id circle = kNoId, rect = kNoId, groupId = kNoId;
    doc.modify("base", [&](Sketch& s) {
        circle = s.addCircle(s.addPoint(20, 10), 8);
        const build::Rectangle r = build::rectangle(s, s.addPoint(-40, -20), s.addPoint(-10, 0));
        rect = r.sides[0];
        s.addArc(s.addPoint(30, -30), s.addPoint(40, -30), s.addPoint(30, -20));
        s.addEllipse(s.addPoint(-30, 30), s.addPoint(-15, 30), s.addPoint(-30, 36));
        Spline sp;
        sp.points = {s.addPoint(40, 30), s.addPoint(50, 40), s.addPoint(60, 30)};
        s.addSpline(sp);
    });
    REQUIRE(view.tryEdit("fill", [&](Editor& ed) { ed.fill({-20, -10}, {255, 0, 0, 255}); }));
    groupId = doc.addGroup({rect});
    REQUIRE(groupId != kNoId);

    QTemporaryDir dir;
    QImage picture(10, 10, QImage::Format_RGB32);
    picture.fill(Qt::gray);
    const QString path = dir.filePath(QStringLiteral("p.png"));
    REQUIRE(picture.save(path));
    core::Image im = images::load(path);
    im.x = 60;
    im.y = -20;
    im.width = 20;
    REQUIRE(view.tryEdit("image", [&](Editor& ed) { ed.addImage(im); }));

    // A second layer to project into, with something of its own.
    REQUIRE(view.tryEdit("layer", [&](Editor& ed) { ed.addLayer(QStringLiteral("Top")); }));
    doc.modify("own", [](Sketch& s) { s.addLine(-50, 50, -40, 60); });
    view.setTool(ToolKind::Project);

    // Over everything: a sweep across the whole canvas, then the things themselves.
    for (int y = 0; y < view.height(); y += 9) {
        for (int x = 0; x < view.width(); x += 9) {
            INFO("at " << x << ", " << y);
            REQUIRE_NOTHROW(QTest::mouseMove(&view, QPoint(x, y)));
            REQUIRE_NOTHROW((void)view.hint());
        }
    }
    for (const Vec2 at : {Vec2{28, 10}, Vec2{20, 10}, Vec2{0, 0}, Vec2{30, 0}, Vec2{0, 30}, Vec2{-25, -20},
                          Vec2{-45, 55}, Vec2{60, -20}, Vec2{-20, -10}}) {
        QTest::mouseMove(&view, view.toScreen(at).toPoint());
        REQUIRE_NOTHROW((void)view.hint());
        REQUIRE_NOTHROW(view.grab());
    }

    // Clicking the circle projects it.
    const size_t before = doc.sketch().entities().size();
    QTest::mouseMove(&view, view.toScreen({28, 10}).toPoint());
    QTest::mouseClick(&view, Qt::LeftButton, {}, view.toScreen({28, 10}).toPoint());
    CHECK(doc.sketch().entities().size() > before);
    // Clicking the origin, or nothing, does nothing.
    const size_t now = doc.sketch().entities().size();
    QTest::mouseClick(&view, Qt::LeftButton, {}, view.toScreen({0, 0}).toPoint());
    QTest::mouseClick(&view, Qt::LeftButton, {}, view.toScreen({100, 100}).toPoint());
    CHECK(doc.sketch().entities().size() == now);
    saveSnapshot(view);
}

TEST_CASE("Every tool: hovering anywhere is safe, on another layer's geometry too", "[project][tools]")
{
    Document doc;
    SketchView view(doc);
    view.resize(500, 400);
    view.show();
    view.resetView();
    doc.modify("base", [&](Sketch& s) {
        s.addCircle(s.addPoint(20, 10), 8);
        build::rectangle(s, s.addPoint(-40, -20), s.addPoint(-10, 0));
        s.addArc(s.addPoint(30, -30), s.addPoint(40, -30), s.addPoint(30, -20));
        s.addEllipse(s.addPoint(-30, 30), s.addPoint(-15, 30), s.addPoint(-30, 36));
        Spline sp;
        sp.points = {s.addPoint(40, 30), s.addPoint(50, 40), s.addPoint(60, 30)};
        s.addSpline(sp);
    });
    REQUIRE(view.tryEdit("fill", [&](Editor& ed) { ed.fill({-20, -10}, {255, 0, 0, 255}); }));
    REQUIRE(view.tryEdit("layer", [&](Editor& ed) { ed.addLayer(QStringLiteral("Top")); }));
    doc.modify("own", [](Sketch& s) { s.addLine(-50, 40, -40, 45); });
    for (int k = 0; k <= static_cast<int>(ToolKind::Group); ++k) {
        const auto kind = static_cast<ToolKind>(k);
        if (kind == ToolKind::Paste) {
            continue;  // (needs something copied)
        }
        INFO("tool " << k);
        REQUIRE_NOTHROW(view.setTool(kind));
        for (int y = 0; y < view.height(); y += 23) {
            for (int x = 0; x < view.width(); x += 23) {
                INFO("at " << x << ", " << y);
                REQUIRE_NOTHROW(QTest::mouseMove(&view, QPoint(x, y)));
                REQUIRE_NOTHROW((void)view.hint());
            }
        }
        REQUIRE_NOTHROW(view.grab());
        QTest::keyClick(&view, Qt::Key_Escape);
        QTest::keyClick(&view, Qt::Key_Escape);
        QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    }
}