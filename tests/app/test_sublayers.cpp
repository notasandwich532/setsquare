// Sublayers in the app: the browser, editing only the active one, the others as references.

#include "BrowserPanel.h"
#include "ConstraintPanel.h"
#include "Document.h"
#include "Editor.h"
#include "SketchView.h"
#include "Snapshot.h"

#include <core/Geometry.h>
#include <core/Serialize.h>

#include <catch2/catch_test_macros.hpp>

#include <QTest>
#include <QTreeWidget>

using namespace core;

namespace
{
struct Fixture
{
    Document doc;
    SketchView view{doc};
    BrowserPanel browser{doc, view};

    Fixture()
    {
        view.resize(800, 600);
        view.show();
        view.resetView();
    }
    ~Fixture() { saveSnapshot(view); }

    QPoint at(double x, double y) const { return view.toScreen({x, y}).toPoint(); }
    const Sketch& sketch() const { return doc.sketch(); }
    static Id lineAt(Editor& ed, double x, double y) { return ed.line(ed.point(x, y - 10), ed.point(x, y + 10)); }
};
}  // namespace

TEST_CASE("Browser lists sublayers; new, activate, hide", "[sublayers]")
{
    Fixture f;
    QTreeWidget* tree = f.browser.findChild<QTreeWidget*>("browser");
    REQUIRE(tree);
    REQUIRE(tree->topLevelItemCount() == 1);
    REQUIRE(tree->topLevelItem(0)->childCount() == 1);
    const Id first = f.sketch().activeSublayer();

    f.browser.addSublayer();
    REQUIRE(f.sketch().sublayers().size() == 2);
    const Id second = f.sketch().activeSublayer();
    CHECK(second != first);
    REQUIRE(f.browser.rowFor(second));
    CHECK(f.browser.rowFor(second)->font(0).bold());
    CHECK_FALSE(f.browser.rowFor(first)->font(0).bold());

    // Clicking the first's marker makes it the one being edited; its eye hides it.
    emit tree->itemClicked(f.browser.rowFor(first), 2);
    CHECK(f.sketch().activeSublayer() == first);
    emit tree->itemClicked(f.browser.rowFor(first), 1);
    CHECK_FALSE(f.sketch().sublayer(first).visible);
    emit tree->itemClicked(f.browser.rowFor(first), 1);
    CHECK(f.sketch().sublayer(first).visible);

    // Renaming in place.
    f.browser.rowFor(second)->setText(0, "Details");
    CHECK(f.sketch().sublayer(second).name == "Details");

    // Adding a sublayer is an undo step.
    f.doc.undo();
    f.doc.undo();
    CHECK(f.sketch().sublayers().size() == 1);
}

TEST_CASE("Only the active sublayer is picked for editing; others are references", "[sublayers]")
{
    Fixture f;
    const Id first = f.sketch().activeSublayer();
    Id a = kNoId, b = kNoId;
    f.doc.edit("lines", [&](Editor& ed) { a = Fixture::lineAt(ed, 10, 10); });
    f.browser.addSublayer();
    const Id second = f.sketch().activeSublayer();
    f.doc.edit("lines", [&](Editor& ed) { b = Fixture::lineAt(ed, 40, 10); });
    CHECK(f.sketch().sublayerOf(a) == first);
    CHECK(f.sketch().sublayerOf(b) == second);

    // Clicking the other sublayer's line selects nothing; this one's selects it.
    QTest::mouseClick(&f.view, Qt::LeftButton, {}, f.at(10, 10));
    CHECK(f.view.selection().empty());
    QTest::mouseClick(&f.view, Qt::LeftButton, {}, f.at(40, 10));
    CHECK(f.view.selection().contains(b));

    // But it can still be snapped to and constrained against.
    CHECK(f.view.pickAt(f.at(10, 10), false, false) == a);

    // Hidden: not picked at all.
    f.doc.setSublayerVisible(first, false);
    CHECK(f.view.pickAt(f.at(10, 10), false, false) == kNoId);
    f.doc.setSublayerVisible(first, true);

    // Moving the selection to the first sublayer.
    f.view.setSelection({b});
    f.browser.moveSelectionTo(first);
    CHECK(f.sketch().sublayerOf(b) == first);

    // Deleting a sublayer deletes what's in it, but not the last one.
    f.browser.deleteSublayer(first, false);
    CHECK_FALSE(f.sketch().hasEntity(a));
    CHECK_FALSE(f.sketch().hasEntity(b));
    REQUIRE(f.sketch().sublayers().size() == 1);
    f.browser.deleteSublayer(second, false);
    CHECK(f.sketch().sublayers().size() == 1);
}

TEST_CASE("Sublayers round-trip through a file", "[sublayers]")
{
    Sketch s;
    const Id first = s.activeSublayer();
    const Id p = s.addPoint(0, 0);
    const Id two = s.addSublayer("Two");
    s.setActiveSublayer(two);
    const Id q = s.addPoint(5, 5);
    const Id l = s.addLine(p, q);
    s.setSublayerVisible(first, false);

    const Sketch back = fromJson(toJson(s));
    REQUIRE(back.sublayers().size() == 2);
    CHECK(back.sublayers()[1].name == "Two");
    CHECK_FALSE(back.sublayer(first).visible);
    CHECK(back.activeSublayer() == two);
    CHECK(back.sublayerOf(p) == first);
    CHECK(back.sublayerOf(l) == two);

    // removeSublayer takes the items in it, and whatever depends on them.
    s.removeSublayer(first);
    CHECK_FALSE(s.hasEntity(p));
    CHECK_FALSE(s.hasEntity(l));
    CHECK_FALSE(s.hasEntity(q));  // (the line's end, gone with it)
    CHECK(s.sublayers().size() == 1);
}

TEST_CASE("Browser lists layers; new layer goes on top and is edited", "[layers]")
{
    Fixture f;
    QTreeWidget* tree = f.browser.findChild<QTreeWidget*>("browser");
    REQUIRE(tree);
    const Id first = f.sketch().activeLayer();
    f.browser.addLayer();
    REQUIRE(f.sketch().layers().size() == 2);
    const Id second = f.sketch().activeLayer();
    CHECK(second != first);
    CHECK(f.sketch().layers().front().id == second);
    REQUIRE(tree->topLevelItemCount() == 2);
    CHECK(tree->topLevelItem(0) == f.browser.rowForLayer(second));
    CHECK(f.browser.rowForLayer(second)->font(0).bold());
    CHECK(f.browser.rowForLayer(second)->childCount() == 1);

    // Clicking the other layer edits it (its own sublayer); its eye hides it.
    emit tree->itemClicked(f.browser.rowForLayer(first), 2);
    CHECK(f.sketch().activeLayer() == first);
    emit tree->itemClicked(f.browser.rowForLayer(second), 1);
    CHECK_FALSE(f.sketch().layer(second).visible);

    f.browser.deleteLayer(second, false);
    CHECK(f.sketch().layers().size() == 1);
    f.browser.deleteLayer(first, false);  // (not the last)
    CHECK(f.sketch().layers().size() == 1);
}

TEST_CASE("Other layers can't be picked or snapped to; Project brings them in", "[layers]")
{
    Fixture f;
    Id a = kNoId;
    f.doc.edit("line", [&](Editor& ed) { a = Fixture::lineAt(ed, 10, 10); });
    f.browser.addLayer();
    const Id top = f.sketch().activeLayer();

    // Not selectable, not snappable, not a reference for a new line.
    QTest::mouseClick(&f.view, Qt::LeftButton, {}, f.at(10, 10));
    CHECK(f.view.selection().empty());
    CHECK(f.view.pickAt(f.at(10, 10), false, false) == kNoId);
    CHECK(f.view.snap(f.at(10, 20)).isFree());  // (its end; not on an axis)

    // Project it by clicking it.
    f.view.setTool(ToolKind::Project);
    QTest::mouseMove(&f.view, f.at(10, 5));
    QTest::mouseClick(&f.view, Qt::LeftButton, {}, f.at(10, 5));
    Id copy = kNoId;
    for (const auto& [id, e] : f.sketch().entities()) {
        if (e.projectedFrom == a) {
            copy = id;
        }
    }
    REQUIRE(copy != kNoId);
    CHECK(f.sketch().layerOf(copy) == top);
    CHECK(f.sketch().entity(copy).construction);
    CHECK(f.sketch().isLinked(copy));

    // Now the copy is there to snap to.
    f.view.setTool(ToolKind::Select);
    CHECK(f.view.pickAt(f.at(10, 5), false, false) == copy);

    // And it follows the original.
    f.doc.setActiveLayer(f.sketch().layers().back().id);
    f.doc.beginDrag(f.sketch().definingPoints(a));
    f.doc.dragTo(5, 0);
    f.doc.endDrag();
    const core::Line& l = f.sketch().line(copy);
    CHECK(std::abs(f.sketch().point(l.p1).x - 15) < 1e-6);
}

TEST_CASE("Another layer's trouble doesn't block edits here", "[layers]")
{
    Fixture f;
    // A layer whose constraints conflict (put in unchecked, as an old file might have them).
    Id line = kNoId;
    f.doc.edit("line", [&](Editor& ed) { line = Fixture::lineAt(ed, 10, 10); });
    f.doc.modify("conflict", [&](core::Sketch& s) {
        s.addConstraint(core::ConstraintType::Distance, {line}, 20.0);
        s.addConstraint(core::ConstraintType::Distance, {line}, 30.0);
    });
    REQUIRE_FALSE(f.doc.solveResult().ok);

    f.browser.addLayer();
    Id circle = kNoId;
    f.doc.edit("circle", [&](Editor& ed) { circle = ed.circle(ed.point(50, 50), 5); });
    QString error;
    const Id radius = f.doc.addConstraint(core::ConstraintType::Radius, {circle}, 5.0, &error);
    REQUIRE(radius != kNoId);
    CHECK(f.doc.setDimensionValue(radius, 8.0, &error));
    CHECK(error.isEmpty());
    CHECK(std::abs(f.sketch().circle(circle).radius - 8.0) < 1e-9);
}
