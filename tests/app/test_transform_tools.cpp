// Move / Rotate / Scale / Mirror / Pattern / Offset / Paste tools, driven with simulated input.

#include "Document.h"
#include "PythonHost.h"
#include "SketchView.h"
#include "Snapshot.h"

#include <core/Features.h>
#include <core/Geometry.h>
#include <core/Transform.h>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <QCoreApplication>
#include <QLineEdit>
#include <QTest>

#include <algorithm>

using namespace core;
using Catch::Approx;

namespace
{

struct Fixture
{
    Document doc;
    SketchView view{doc};
    QStringList recorded;
    build::Rectangle rect;

    Fixture()
    {
        view.resize(800, 600);
        view.show();
        view.resetView();
        QObject::connect(&doc, &Document::recorded, [this](const QString& py) { recorded << py; });
        doc.modify("rectangle", [&](Sketch& s) {
            const Id a = s.addPoint(10, 10);  // in this order, as s.rectangle((10, 10), (30, 20)) does
            rect = build::rectangle(s, a, s.addPoint(30, 20));
        });
    }
    ~Fixture() { saveSnapshot(view); }

    QPoint at(double x, double y) const { return view.toScreen({x, y}).toPoint(); }
    void click(double x, double y, Qt::KeyboardModifiers mods = {})
    {
        QTest::mouseMove(&view, at(x, y));
        QTest::mouseClick(&view, Qt::LeftButton, mods, at(x, y));
    }
    void move(double x, double y) { QTest::mouseMove(&view, at(x, y)); }
    void key(int k) { QTest::keyClick(&view, static_cast<Qt::Key>(k)); }
    const Sketch& sketch() const { return doc.sketch(); }
    Vec2 pos(Id p) const { return {sketch().point(p).x, sketch().point(p).y}; }
    void selectRect() { view.setSelection({rect.sides.begin(), rect.sides.end()}); }
    size_t count(EntityKind kind) const
    {
        return static_cast<size_t>(std::ranges::count_if(sketch().entities(), [&](const auto& kv) {
            return kv.second.kind() == kind && !sketch().isReference(kv.first);
        }));
    }
    Id feature(ConstraintType type) const
    {
        for (const auto& [id, c] : sketch().constraints()) {
            if (c.type == type) {
                return id;
            }
        }
        return kNoId;
    }
    QLineEdit* editor() const
    {
        QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
        return view.findChild<QLineEdit*>("dimensionEditor");
    }
};

}  // namespace

TEST_CASE("Move the selection from a base point to a target", "[transform][move]")
{
    Fixture f;
    f.selectRect();
    f.view.setTool(ToolKind::Move);
    f.click(10, 10);  // base: the corner
    f.move(25, 15);
    f.click(40, 30);
    CHECK(f.pos(f.rect.corners[0]).x == Approx(40.0).margin(1e-6));
    CHECK(f.pos(f.rect.corners[0]).y == Approx(30.0).margin(1e-6));
    CHECK(f.pos(f.rect.corners[2]).x == Approx(60.0).margin(1e-6));
    REQUIRE(!f.recorded.isEmpty());
    CHECK(f.recorded.last().startsWith("s.move("));
    f.doc.undo();  // one step
    CHECK(f.pos(f.rect.corners[0]).x == Approx(10.0));
}

TEST_CASE("Esc puts a move back", "[transform][move]")
{
    Fixture f;
    f.selectRect();
    f.view.setTool(ToolKind::Move);
    f.click(10, 10);
    f.move(50, 50);
    f.view.repaint();  // the live move happens before a frame
    f.key(Qt::Key_Escape);
    CHECK(f.pos(f.rect.corners[0]).x == Approx(10.0));
    CHECK(!f.doc.isDragging());
}

TEST_CASE("Copy with the option or Ctrl", "[transform][move]")
{
    Fixture f;
    f.selectRect();
    f.view.setTool(ToolKind::Move);
    const bool option = GENERATE(true, false);
    f.view.modifyOptions().copy = option;
    f.click(10, 10);
    f.click(10, 50, option ? Qt::KeyboardModifiers{} : Qt::ControlModifier);
    CHECK(f.count(EntityKind::Line) == 8);
    CHECK(f.pos(f.rect.corners[0]).y == Approx(10.0));  // the original stays
    CHECK(f.doc.solveResult().dof == 8);                // two rectangles, each H/V constrained
}

TEST_CASE("Rotate a free shape about a centre", "[transform][rotate]")
{
    Fixture f;
    Id l = kNoId;
    f.doc.modify("line", [&](Sketch& s) { l = s.addLine(50, 0, 60, 0); });
    f.view.setSelection({l});
    f.view.setTool(ToolKind::Rotate);
    f.click(50, 0);   // centre
    f.click(60, 0);   // reference direction
    f.move(55, 4);
    f.view.repaint();
    f.click(50, 10);  // straight up: 90 degrees
    const Line line = f.sketch().line(l);
    CHECK(f.pos(line.p2).x == Approx(50.0).margin(1e-6));
    CHECK(f.pos(line.p2).y == Approx(10.0).margin(1e-6));
}

TEST_CASE("Scale about a clicked point", "[transform][scale]")
{
    Fixture f;
    f.doc.modify("dimension", [&](Sketch& s) { s.addConstraint(ConstraintType::Distance, {f.rect.sides[0]}, 20.0); });
    f.selectRect();
    f.view.modifyOptions().scaleFactor = 0.5;
    f.view.setTool(ToolKind::Scale);
    f.click(10, 10);
    CHECK(f.pos(f.rect.corners[2]).x == Approx(20.0).margin(1e-6));
    CHECK(f.sketch().constraint(f.feature(ConstraintType::Distance)).value == Approx(10.0));
}

TEST_CASE("Mirror across an axis", "[transform][mirror]")
{
    Fixture f;
    f.selectRect();
    f.view.setTool(ToolKind::Mirror);
    f.click(0, 40);  // on the Y axis
    CHECK(f.count(EntityKind::Line) == 8);
    CHECK(f.doc.solveResult().dof == 4);
    CHECK(f.view.selection().empty());
}

TEST_CASE("With nothing selected, pick the items first", "[transform]")
{
    Fixture f;
    f.view.setTool(ToolKind::Mirror);
    f.click(20, 10);  // bottom side
    f.click(30, 15);  // right side
    CHECK(f.view.selection().size() == 2);
    f.key(Qt::Key_Return);
    f.click(0, 40);
    CHECK(f.count(EntityKind::Line) == 6);
}

TEST_CASE("Rectangular and circular patterns from the toolbar settings", "[transform][pattern]")
{
    Fixture f;
    f.selectRect();
    f.view.modifyOptions().patternCount = 2;
    f.view.modifyOptions().patternCount2 = 2;
    f.view.modifyOptions().patternSpacing = 30;
    f.view.modifyOptions().patternSpacing2 = 20;
    f.view.setTool(ToolKind::RectangularPattern);
    f.key(Qt::Key_Return);
    const Id k = f.feature(ConstraintType::LinearPattern);
    REQUIRE(k != kNoId);
    CHECK(feature::pattern(f.sketch().constraint(k)).copies.size() == 3);
    CHECK(f.doc.solveResult().dof == 4);

    Id spoke = kNoId;
    f.doc.modify("spoke", [&](Sketch& s) { spoke = s.addLine(-5, 0, -15, 0); });
    f.view.setSelection({spoke});
    f.view.modifyOptions().circularCount = 3;
    f.view.setTool(ToolKind::CircularPattern);
    f.click(0, 0);  // the origin
    const Id c = f.feature(ConstraintType::CircularPattern);
    REQUIRE(c != kNoId);
    CHECK(feature::pattern(f.sketch().constraint(c)).copies.size() == 2);
}

TEST_CASE("Offset a chain, then type the distance", "[transform][offset]")
{
    Fixture f;
    f.view.modifyOptions().offsetDistance = 2;
    f.view.setTool(ToolKind::Offset);
    f.move(20, 8);
    f.click(20, 10.2);  // the bottom side, cursor just inside
    const Id k = f.feature(ConstraintType::Offset);
    REQUIRE(k != kNoId);
    QLineEdit* e = f.editor();
    REQUIRE(e != nullptr);
    e->selectAll();
    QTest::keyClicks(e, "3");
    QTest::keyClick(e, Qt::Key_Return);
    CHECK(f.sketch().constraint(k).value == Approx(3.0));
    const Line inner = f.sketch().line(feature::offsetRefs(f.sketch().constraint(k)).offsets[0]);
    CHECK(f.pos(inner.p1).y == Approx(13.0).margin(1e-6));
    CHECK(f.doc.solveResult().dof == 4);
}

TEST_CASE("Paste follows the cursor", "[transform][paste]")
{
    Fixture f;
    f.view.setPasteFragment(xform::extract(f.sketch(), {f.rect.sides.begin(), f.rect.sides.end()}));
    f.view.setTool(ToolKind::Paste);
    f.move(60, 60);
    f.click(60, 60);
    CHECK(f.count(EntityKind::Line) == 8);
    CHECK(f.view.selection().size() == 8);  // the pasted lines and points
    double maxX = 0;
    for (Id id : f.view.selection()) {
        if (f.sketch().entity(id).kind() == EntityKind::Point) {
            maxX = std::max(maxX, f.pos(id).x);
        }
    }
    CHECK(maxX == Approx(70.0));  // 20 wide, centred on 60
}

TEST_CASE("Replaying recorded transform tools reproduces the sketch", "[transform][recorder]")
{
    Fixture f;
    f.recorded.clear();
    // Start the recording from the rectangle the fixture made directly.
    f.recorded << "s.rectangle((10, 10), (30, 20))";
    f.selectRect();
    f.view.setTool(ToolKind::Move);
    f.click(10, 10);
    f.click(15, 12);
    f.view.modifyOptions().copy = true;
    f.click(15, 12);
    f.click(15, 40);
    f.view.setTool(ToolKind::Mirror);
    f.click(0, 40);
    f.selectRect();
    f.view.setTool(ToolKind::CircularPattern);
    f.click(0, 0);
    f.view.setTool(ToolKind::Offset);
    f.click(25, 12.2);
    QTest::keyClick(f.editor(), Qt::Key_Escape);

    Document replay;
    script::setDocument(&replay);
    QString err;
    script::setOutput([&](const QString& t, bool e) {
        if (e) {
            err += t;
        }
    });
    script::resetConsole();
    const script::RunResult r = script::runConsole(f.recorded.join('\n'));
    script::setOutput(nullptr);
    script::setDocument(nullptr);
    INFO(f.recorded.join('\n').toStdString());
    INFO(err.toStdString());
    REQUIRE(r.ok);
    REQUIRE(replay.sketch().entities().size() == f.sketch().entities().size());
    REQUIRE(replay.sketch().constraints().size() == f.sketch().constraints().size());
    for (const auto& [id, e] : f.sketch().entities()) {
        REQUIRE(replay.sketch().hasEntity(id));
        if (e.kind() == EntityKind::Point) {
            CHECK(replay.sketch().point(id).x == Approx(f.sketch().point(id).x).margin(1e-6));
            CHECK(replay.sketch().point(id).y == Approx(f.sketch().point(id).y).margin(1e-6));
        }
    }
}
