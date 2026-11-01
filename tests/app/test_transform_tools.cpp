// Move/Copy, Scale, Mirror, Pattern and Offset commands (panels and handles), and Paste, driven
// with simulated input.

#include "CommandPanel.h"
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

#include <QCheckBox>
#include <QComboBox>
#include <QCoreApplication>
#include <QDoubleSpinBox>
#include <QLineEdit>
#include <QPushButton>
#include <QSpinBox>
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
        view.resize(1000, 700);
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
    void click(double x, double y)
    {
        QTest::mouseMove(&view, at(x, y));
        QTest::mouseClick(&view, Qt::LeftButton, {}, at(x, y));
    }
    // Press at a screen point, move by (dx, dy) pixels in steps, release.
    void drag(QPoint from, QPoint by)
    {
        QTest::mouseMove(&view, from);
        QTest::mousePress(&view, Qt::LeftButton, {}, from);
        for (int i = 1; i <= 4; ++i) {
            QTest::mouseMove(&view, from + by * i / 4);
        }
        QTest::mouseRelease(&view, Qt::LeftButton, {}, from + by);
    }
    void settle()  // queued tool switches and deferred deletes
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
    void ok()
    {
        field<QPushButton>("ok")->click();
        settle();
    }
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
};

}  // namespace

TEST_CASE("Move: distances in the panel move the geometry live; OK keeps it", "[transform][move]")
{
    Fixture f;
    f.selectRect();
    f.view.setTool(ToolKind::Move);
    f.field<QDoubleSpinBox>("dx")->setValue(30);
    f.field<QDoubleSpinBox>("dy")->setValue(20);
    CHECK(f.pos(f.rect.corners[0]).x == Approx(40.0).margin(1e-6));  // already, as a preview
    f.ok();
    CHECK(f.view.toolKind() == ToolKind::Select);
    CHECK(f.pos(f.rect.corners[0]).y == Approx(30.0).margin(1e-6));
    REQUIRE(!f.recorded.isEmpty());
    CHECK(f.recorded.last().startsWith("s.move("));
    f.doc.undo();  // one step
    CHECK(f.pos(f.rect.corners[0]).x == Approx(10.0));
}

TEST_CASE("Move: Esc puts everything back", "[transform][move]")
{
    Fixture f;
    f.selectRect();
    f.view.setTool(ToolKind::Move);
    f.field<QDoubleSpinBox>("dx")->setValue(30);
    f.view.setFocus();
    QTest::keyClick(&f.view, Qt::Key_Escape);
    f.settle();
    CHECK(f.view.toolKind() == ToolKind::Select);
    CHECK(f.pos(f.rect.corners[0]).x == Approx(10.0));
    CHECK(!f.doc.isDragging());
}

TEST_CASE("Move: drag the X arrow, then copy instead", "[transform][move]")
{
    Fixture f;
    f.selectRect();
    f.view.setTool(ToolKind::Move);
    // The X arrow starts a little right of the rectangle's centre (20, 15).
    f.drag(f.at(20, 15) + QPoint(28, 0), QPoint(40, 0));
    const double moved = 40.0 / f.view.zoom();
    CHECK(f.field<QDoubleSpinBox>("dx")->value() == Approx(moved).margin(0.3));
    f.field<QCheckBox>("copy")->setChecked(true);
    CHECK(f.pos(f.rect.corners[0]).x == Approx(10.0));  // the original went back
    f.ok();
    CHECK(f.count(EntityKind::Line) == 8);
    CHECK(f.doc.solveResult().dof == 8);
}

TEST_CASE("Move: turn by an angle about the centre", "[transform][move]")
{
    Fixture f;
    Id l = kNoId;
    f.doc.modify("line", [&](Sketch& s) { l = s.addLine(50, 0, 60, 0); });
    f.view.setSelection({l});
    f.view.setTool(ToolKind::Move);
    f.field<QDoubleSpinBox>("angle")->setValue(90);
    f.ok();
    const Line line = f.sketch().line(l);
    CHECK(f.pos(line.p1).x == Approx(55.0).margin(1e-6));
    CHECK(f.pos(line.p1).y == Approx(-5.0).margin(1e-6));
    CHECK(f.pos(line.p2).y == Approx(5.0).margin(1e-6));
}

TEST_CASE("Scale: pick the point, set the factor", "[transform][scale]")
{
    Fixture f;
    f.doc.modify("dimension", [&](Sketch& s) { s.addConstraint(ConstraintType::Distance, {f.rect.sides[0]}, 20.0); });
    f.selectRect();
    f.view.setTool(ToolKind::Scale);
    f.click(10, 10);  // the "Point" input is current: the corner
    f.field<QDoubleSpinBox>("factor")->setValue(0.5);
    f.ok();
    CHECK(f.pos(f.rect.corners[2]).x == Approx(20.0).margin(1e-6));
    CHECK(f.sketch().constraint(f.feature(ConstraintType::Distance)).value == Approx(10.0));
}

TEST_CASE("Scale: Enter applies it", "[transform][scale]")
{
    Fixture f;
    const bool fromOrigin = GENERATE(false, true);
    const bool inPanel = GENERATE(false, true);
    if (fromOrigin) {  // a rectangle drawn from the origin uses the origin as a corner
        f.doc.modify("origin rectangle", [&](Sketch& s) {
            f.rect = build::rectangle(s, s.origin(), s.addPoint(20, 10));
        });
    }
    INFO("from origin " << fromOrigin << ", Enter in panel " << inPanel);
    f.selectRect();
    f.view.setTool(ToolKind::Scale);
    f.click(50, 50);  // empty space: scale about here
    QDoubleSpinBox* factor = f.field<QDoubleSpinBox>("factor");
    factor->setValue(2);
    const Id corner = f.rect.corners[2];
    const Vec2 before = f.pos(corner);
    if (inPanel) {
        factor->setFocus();
        QTest::keyClick(factor, Qt::Key_Return);
    }
    else {
        f.view.setFocus();
        QTest::keyClick(&f.view, Qt::Key_Return);
    }
    f.settle();
    CHECK(f.view.toolKind() == ToolKind::Select);
    REQUIRE(f.sketch().hasEntity(corner));
    CHECK(f.doc.solveResult().ok);
    if (fromOrigin) {
        // The origin can't move, so the shape stays on it; the rest still scales.
        CHECK(f.pos(f.rect.corners[0]).x == 0.0);
        CHECK(f.pos(corner).x != Approx(before.x));
    }
    else {
        CHECK(f.pos(corner).x == Approx(50 + 2 * (before.x - 50)).margin(1e-6));
        CHECK(f.pos(corner).y == Approx(50 + 2 * (before.y - 50)).margin(1e-6));
    }
}

TEST_CASE("Scale about the origin when the shape is on it", "[transform][scale]")
{
    Fixture f;
    f.doc.modify("origin rectangle", [&](Sketch& s) { f.rect = build::rectangle(s, s.origin(), s.addPoint(20, 10)); });
    f.selectRect();
    f.view.setTool(ToolKind::Scale);
    f.click(0, 0);  // the origin itself
    f.field<QDoubleSpinBox>("factor")->setValue(2);
    QTest::keyClick(&f.view, Qt::Key_Return);
    f.settle();
    CHECK(f.pos(f.rect.corners[2]).x == Approx(40.0).margin(1e-6));
    CHECK(f.pos(f.rect.corners[2]).y == Approx(20.0).margin(1e-6));
}

TEST_CASE("Mirror: pick objects, then the line", "[transform][mirror]")
{
    Fixture f;
    f.view.setTool(ToolKind::Mirror);
    f.click(20, 10);  // bottom side
    f.click(30, 15);  // right side
    CHECK(f.field<SelectionButton>("objects")->text().contains("2"));
    f.field<SelectionButton>("line")->click();
    f.click(0, 40);  // the Y axis
    f.ok();
    CHECK(f.count(EntityKind::Line) == 6);
}

TEST_CASE("Mirror with a selection goes straight to the line", "[transform][mirror]")
{
    Fixture f;
    f.selectRect();
    f.view.setTool(ToolKind::Mirror);
    f.click(0, 40);
    QTest::keyClick(&f.view, Qt::Key_Return);
    f.settle();
    CHECK(f.count(EntityKind::Line) == 8);
    CHECK(f.doc.solveResult().dof == 4);
}

TEST_CASE("Rectangular pattern: quantities, distances and direction", "[transform][pattern]")
{
    Fixture f;
    f.selectRect();
    f.view.setTool(ToolKind::RectangularPattern);
    f.field<QSpinBox>("quantity")->setValue(3);
    f.field<QDoubleSpinBox>("distance")->setValue(60);  // extent: spacing 30
    f.field<QSpinBox>("quantity2")->setValue(2);
    f.field<QDoubleSpinBox>("distance2")->setValue(-20);  // downwards
    f.field<QComboBox>("symmetric")->setCurrentIndex(1);  // both ways along X
    f.ok();
    const Id k = f.feature(ConstraintType::LinearPattern);
    REQUIRE(k != kNoId);
    const auto p = feature::linearParams(f.sketch().constraint(k));
    CHECK(p.spacing == Approx(30.0));
    CHECK(p.symmetric);
    CHECK(feature::pattern(f.sketch().constraint(k)).copies.size() == 5 * 2 - 1);
    CHECK(f.doc.solveResult().dof == 4);
    double minX = 1e9, minY = 1e9;
    for (const auto& [id, e] : f.sketch().entities()) {
        if (e.kind() == EntityKind::Point && !f.sketch().isReference(id)) {
            minX = std::min(minX, f.pos(id).x);
            minY = std::min(minY, f.pos(id).y);
        }
    }
    CHECK(minX == Approx(-50.0));
    CHECK(minY == Approx(-10.0));

    // Editing it again from the constraints panel opens the command on it.
    f.view.editFeature(k);
    CHECK(f.field<QSpinBox>("quantity")->value() == 3);
    f.field<QSpinBox>("quantity")->setValue(2);
    f.ok();
    const Id k2 = f.feature(ConstraintType::LinearPattern);
    CHECK(feature::pattern(f.sketch().constraint(k2)).copies.size() == 3 * 2 - 1);
}

TEST_CASE("Rectangular pattern: drag the arrow to set the extent", "[transform][pattern]")
{
    Fixture f;
    f.selectRect();
    f.view.setTool(ToolKind::RectangularPattern);
    f.field<QSpinBox>("quantity")->setValue(2);
    f.field<QDoubleSpinBox>("distance")->setValue(30);
    // Arrow 1 sits just beyond centre (20, 15) + 30 along X.
    f.drag(f.at(50, 15) + QPoint(28, 0), QPoint(40, 0));
    CHECK(f.field<QDoubleSpinBox>("distance")->value() == Approx(30.0 + 40.0 / f.view.zoom()).margin(0.3));
}

TEST_CASE("Circular pattern: pick the centre", "[transform][pattern]")
{
    Fixture f;
    Id spoke = kNoId;
    f.doc.modify("spoke", [&](Sketch& s) { spoke = s.addLine(-5, 0, -15, 0); });
    f.view.setSelection({spoke});
    f.view.setTool(ToolKind::CircularPattern);
    f.click(0, 0);  // the origin
    f.field<QSpinBox>("quantity")->setValue(3);
    f.ok();
    const Id c = f.feature(ConstraintType::CircularPattern);
    REQUIRE(c != kNoId);
    CHECK(feature::pattern(f.sketch().constraint(c)).copies.size() == 2);
}

TEST_CASE("Offset: click a curve for its chain, set the distance", "[transform][offset]")
{
    Fixture f;
    f.view.setTool(ToolKind::Offset);
    f.click(20, 10.2);  // the bottom side, from just inside
    CHECK(f.field<SelectionButton>("curves")->text().contains("4"));
    f.field<QDoubleSpinBox>("distance")->setValue(3);
    f.ok();
    const Id k = f.feature(ConstraintType::Offset);
    REQUIRE(k != kNoId);
    const Line inner = f.sketch().line(feature::offsetRefs(f.sketch().constraint(k)).offsets[0]);
    CHECK(f.pos(inner.p1).y == Approx(13.0).margin(1e-6));
    CHECK(f.doc.solveResult().dof == 4);
}

TEST_CASE("Offset: a negative distance goes to the other side", "[transform][offset]")
{
    Fixture f;
    f.view.setTool(ToolKind::Offset);
    f.click(20, 10.2);
    f.field<QDoubleSpinBox>("distance")->setValue(-2);
    f.ok();
    const Id k = f.feature(ConstraintType::Offset);
    REQUIRE(k != kNoId);
    const Line outer = f.sketch().line(feature::offsetRefs(f.sketch().constraint(k)).offsets[0]);
    CHECK(f.pos(outer.p1).y == Approx(8.0).margin(1e-6));
}

TEST_CASE("Fillet settings are in a panel on the canvas", "[transform][options]")
{
    Fixture f;
    f.view.setTool(ToolKind::Fillet);
    f.field<QDoubleSpinBox>("radius")->setValue(3);
    CHECK(f.view.modifyOptions().filletRadius == Approx(3.0));
    f.view.setTool(ToolKind::Line);
    f.settle();
    CHECK(f.view.findChild<CommandPanel*>("commandPanel") == nullptr);
}

TEST_CASE("Paste follows the cursor", "[transform][paste]")
{
    Fixture f;
    f.view.setPasteFragment(xform::extract(f.sketch(), {f.rect.sides.begin(), f.rect.sides.end()}));
    f.view.setTool(ToolKind::Paste);
    f.click(60, 60);
    CHECK(f.count(EntityKind::Line) == 8);
    double maxX = 0;
    for (Id id : f.view.selection()) {
        if (f.sketch().entity(id).kind() == EntityKind::Point) {
            maxX = std::max(maxX, f.pos(id).x);
        }
    }
    CHECK(maxX == Approx(70.0));  // 20 wide, centred on 60
}

TEST_CASE("Replaying recorded commands reproduces the sketch", "[transform][recorder]")
{
    Fixture f;
    f.recorded.clear();
    f.recorded << "s.rectangle((10, 10), (30, 20))";  // what the fixture made directly
    f.selectRect();
    f.view.setTool(ToolKind::Move);
    f.field<QDoubleSpinBox>("dx")->setValue(5);
    f.field<QDoubleSpinBox>("dy")->setValue(2);
    f.ok();
    f.selectRect();
    f.view.setTool(ToolKind::Move);
    f.field<QCheckBox>("copy")->setChecked(true);
    f.field<QDoubleSpinBox>("dy")->setValue(28);
    f.ok();
    f.selectRect();
    f.view.setTool(ToolKind::Mirror);
    f.click(0, 40);
    f.ok();
    f.selectRect();
    f.view.setTool(ToolKind::CircularPattern);
    f.click(0, 0);
    f.ok();
    f.view.setTool(ToolKind::Offset);
    f.click(25, 12.2);
    f.field<QDoubleSpinBox>("distance")->setValue(2);
    f.ok();
    f.view.modifyOptions().copy = false;

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

TEST_CASE("Offset: both sides, with the ends capped", "[transform][offset]")
{
    Fixture f;
    Id line = kNoId;
    f.doc.modify("line", [&](Sketch& s) { line = s.addLine(50, 50, 80, 50); });
    f.view.setTool(ToolKind::Offset);
    f.click(65, 50);
    f.field<QComboBox>("sides")->setCurrentIndex(1);
    REQUIRE(f.field<QCheckBox>("caps")->isVisible());
    f.field<QCheckBox>("caps")->setChecked(true);
    f.field<QDoubleSpinBox>("distance")->setValue(4);
    f.ok();
    const Id k = f.feature(ConstraintType::Offset);
    REQUIRE(k != kNoId);
    const auto refs = feature::offsetRefs(f.sketch().constraint(k));
    CHECK(refs.offsets2.size() == 1);
    CHECK(refs.caps.size() == 2);
    CHECK(f.doc.solveResult().ok);
    CHECK(f.doc.solveResult().redundant.empty());
    std::vector<double> ys;
    for (Id l : {refs.offsets[0], refs.offsets2[0]}) {
        ys.push_back(f.pos(f.sketch().line(l).p1).y);
    }
    std::ranges::sort(ys);
    CHECK(ys[0] == Approx(46.0).margin(1e-6));
    CHECK(ys[1] == Approx(54.0).margin(1e-6));
}

TEST_CASE("Divide: points evenly along a line, kept there as it moves", "[transform][divide]")
{
    Fixture f;
    f.view.setTool(ToolKind::Divide);
    f.click(20, 10.2);  // the bottom side, 20 long
    f.field<QComboBox>("spacing")->setCurrentIndex(0);
    f.field<QSpinBox>("points")->setValue(3);
    f.ok();
    const Id k = f.feature(ConstraintType::Divide);
    REQUIRE(k != kNoId);
    const Constraint c = f.sketch().constraint(k);
    REQUIRE(c.refs.size() == 4);
    CHECK(c.refs[0] == f.rect.sides[0]);
    for (size_t i = 1; i < 4; ++i) {
        CHECK(f.pos(c.refs[i]).x == Approx(10.0 + 5.0 * double(i)).margin(1e-6));
        CHECK(f.pos(c.refs[i]).y == Approx(10.0).margin(1e-6));
    }
    CHECK(f.doc.solveResult().dof == 4);  // the rectangle's; the points add none
    REQUIRE(!f.recorded.isEmpty());
    CHECK(f.recorded.last().contains("s.divide(") );
    CHECK(f.recorded.last().contains("count=3"));
}

TEST_CASE("Divide: by a ratio, from the arrow's end; clicking the arrow turns it round", "[transform][divide]")
{
    Fixture f;
    f.view.setTool(ToolKind::Divide);
    f.click(20, 10.2);
    f.field<QComboBox>("spacing")->setCurrentIndex(1);
    f.field<QLineEdit>("ratio")->setText("3:2:1");
    auto* reverse = f.field<QCheckBox>("reverse");
    CHECK_FALSE(reverse->isChecked());
    f.view.fitAll();  // (closer, for the snapshot)
    saveSnapshot(f.view, QStringLiteral("-panel"));
    // The arrow sits beside the start of the line (10, 10), pointing along it.
    const QPoint arrow = f.at(10, 10) + QPoint(10, -18);
    QTest::mouseMove(&f.view, arrow);
    QTest::mouseClick(&f.view, Qt::LeftButton, {}, arrow);
    CHECK(reverse->isChecked());
    f.ok();
    const Id k = f.feature(ConstraintType::Divide);
    REQUIRE(k != kNoId);
    const Constraint c = f.sketch().constraint(k);
    REQUIRE(c.refs.size() == 3);
    // From (30, 10) back: gaps 10, 6.67, 3.33.
    CHECK(f.pos(c.refs[1]).x == Approx(20.0).margin(1e-6));
    CHECK(f.pos(c.refs[2]).x == Approx(30.0 - 50.0 / 3.0).margin(1e-6));
    CHECK(f.recorded.last().contains("ratio=\"3:2:1\", reversed=True"));
}

TEST_CASE("Divide: a ratio that doesn't read is refused, with why", "[transform][divide]")
{
    Fixture f;
    QStringList messages;
    QObject::connect(&f.view, &SketchView::message, [&](const QString& m) { messages << m; });
    f.view.setTool(ToolKind::Divide);
    f.click(20, 10.2);
    f.field<QComboBox>("spacing")->setCurrentIndex(1);
    f.field<QLineEdit>("ratio")->setText("3:x");
    f.ok();
    CHECK(f.feature(ConstraintType::Divide) == kNoId);
    REQUIRE(!messages.isEmpty());
    CHECK(messages.last().contains("isn't a number"));
}

TEST_CASE("Divide: double-clicking it in the list edits it", "[transform][divide]")
{
    Fixture f;
    Id k = kNoId;
    f.doc.modify("divide", [&](Sketch& s) { k = feature::divide(s, f.rect.sides[0], feature::evenly(1)); });
    f.view.editFeature(k);
    f.settle();
    CHECK(f.view.toolKind() == ToolKind::Divide);
    CHECK(f.field<QSpinBox>("points")->value() == 1);
    f.field<QSpinBox>("points")->setValue(4);
    f.ok();
    const Id edited = f.feature(ConstraintType::Divide);
    REQUIRE(edited != kNoId);
    CHECK(f.sketch().constraint(edited).refs.size() == 5);
    CHECK(f.count(EntityKind::Point) == 4 + 4);  // the rectangle's corners and the new points only
}
TEST_CASE("Features and fixed items show a symbol on the canvas that selects them", "[transform][symbols]")
{
    Fixture f;
    Id k = kNoId, divide = kNoId, fixedLine = kNoId;
    f.doc.modify("features", [&](Sketch& s) {
        k = feature::offset(s, {f.rect.sides.begin(), f.rect.sides.end()}, 2, {20, 15});
        fixedLine = s.addLine(s.addPoint(40, 10), s.addPoint(40, 30));
        s.entity(fixedLine).fixed = true;
        divide = feature::divide(s, fixedLine, feature::evenly(2));
    });
    // Look for each one's symbol near what it's on.
    auto found = [&](Id want, Vec2 near) {
        const QPoint c = f.at(near.x, near.y);
        for (int dy = -45; dy <= 45; dy += 2) {
            for (int dx = -45; dx <= 45; dx += 2) {
                if (f.view.pickAt(QPointF(c + QPoint(dx, dy))) == want) {
                    return c + QPoint(dx, dy);
                }
            }
        }
        return QPoint(-1, -1);
    };
    const QPoint offsetSymbol = found(k, {20, 12});  // the offset's first curve: the bottom, raised
    CHECK(offsetSymbol != QPoint(-1, -1));
    CHECK(found(divide, {40, 20}) != QPoint(-1, -1));
    const QPoint lock = found(fixedLine, {40, 20});
    CHECK(lock != QPoint(-1, -1));

    // Clicking the offset's symbol selects the offset (so Delete unlinks it).
    QTest::mouseMove(&f.view, offsetSymbol);
    QTest::mouseClick(&f.view, Qt::LeftButton, {}, offsetSymbol);
    CHECK(f.view.selection().contains(k));
}