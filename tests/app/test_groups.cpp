// Groups in the UI: the Group tool (pre-selection, gathering, a box), dragging a group, its symbol
// (select and delete it to ungroup), and dimensions held inside one.

#include "Document.h"
#include "SketchView.h"
#include "Snapshot.h"
#include "tools/ConstraintTools.h"

#include <core/Builders.h>
#include <core/Groups.h>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <QTest>

#include <cmath>

using namespace core;
using Catch::Approx;

namespace
{
struct Fixture
{
    Document doc;
    SketchView view{doc};
    QStringList recorded;
    std::vector<Id> tri;  // a triangle's sides: (0, 0), (30, 0), (10, 20)
    Id a = kNoId, b = kNoId, c = kNoId;
    Id loose = kNoId;  // a line apart from it

    Fixture()
    {
        view.resize(800, 600);
        view.show();
        view.resetView();
        QObject::connect(&doc, &Document::recorded, [this](const QString& py) { recorded << py; });
        doc.modify("shape", [&](Sketch& s) {
            a = s.addPoint(0, 0);
            b = s.addPoint(30, 0);
            c = s.addPoint(10, 20);
            tri = {s.addLine(a, b), s.addLine(b, c), s.addLine(c, a)};
            loose = s.addLine(-40, -30, -20, -30);
        });
    }
    ~Fixture() { saveSnapshot(view); }

    QPoint at(double x, double y) const { return view.toScreen({x, y}).toPoint(); }
    void click(double x, double y)
    {
        QTest::mouseMove(&view, at(x, y));
        QTest::mouseClick(&view, Qt::LeftButton, {}, at(x, y));
    }
    void drag(double x0, double y0, double x1, double y1)
    {
        QTest::mouseMove(&view, at(x0, y0));
        QTest::mousePress(&view, Qt::LeftButton, {}, at(x0, y0));
        for (int i = 1; i <= 8; ++i) {
            QTest::mouseMove(&view, at(x0 + (x1 - x0) * i / 8, y0 + (y1 - y0) * i / 8));
        }
        QTest::mouseRelease(&view, Qt::LeftButton, {}, at(x1, y1));
    }
    Point p(Id id) const { return doc.sketch().point(id); }
    double d(Id u, Id v) const { return std::hypot(p(u).x - p(v).x, p(u).y - p(v).y); }
    std::vector<Id> groups() const
    {
        std::vector<Id> out;
        for (const auto& [id, k] : doc.sketch().constraints()) {
            if (k.type == ConstraintType::Group) {
                out.push_back(id);
            }
        }
        return out;
    }
};
}  // namespace

TEST_CASE("Group tool: what's selected is grouped at once", "[group]")
{
    Fixture f;
    f.view.setSelection({f.tri.begin(), f.tri.end()});
    f.view.setTool(ToolKind::Group);
    QCoreApplication::processEvents();
    CHECK(f.groups().size() == 1);
    CHECK(f.view.toolKind() == ToolKind::Select);
    CHECK(f.recorded.join('\n').contains(QStringLiteral("s.group([")));
}

TEST_CASE("Group tool: click to gather (again to leave out), Enter to group", "[group]")
{
    Fixture f;
    f.view.setTool(ToolKind::Group);
    f.click(15, 0);   // bottom side
    f.click(20, 10);  // right side
    f.click(-30, -30);
    f.click(-30, -30);  // the loose line: in, then out
    QTest::keyClick(&f.view, Qt::Key_Return);
    REQUIRE(f.groups().size() == 1);
    const group::Members m = group::members(f.doc.sketch(), f.doc.sketch().constraint(f.groups()[0]));
    CHECK(m.points.size() == 3);  // (the two sides: all three corners)
    CHECK(f.view.toolKind() == ToolKind::Group);  // (ready for another)
}

TEST_CASE("A group drags as one; its symbol selects it, and deleting it ungroups", "[group]")
{
    Fixture f;
    REQUIRE(f.doc.addGroup(f.tri) != kNoId);
    const double ab = f.d(f.a, f.b), bc = f.d(f.b, f.c), ca = f.d(f.c, f.a);
    f.view.setTool(ToolKind::Select);
    f.drag(10, 20, 25, 35);  // by its top corner
    CHECK(f.p(f.c).x == Approx(25).margin(0.3));
    CHECK(f.d(f.a, f.b) == Approx(ab).margin(1e-6));
    CHECK(f.d(f.b, f.c) == Approx(bc).margin(1e-6));
    CHECK(f.d(f.c, f.a) == Approx(ca).margin(1e-6));
    CHECK(f.p(f.a).x != Approx(0).margin(0.1));  // the rest came along
    const Line loose = f.doc.sketch().line(f.loose);
    CHECK(f.p(loose.p1).x == Approx(-40));

    // Its symbol sits in the middle of its corners: click it, Delete.
    const Point mid{(f.p(f.a).x + f.p(f.b).x + f.p(f.c).x) / 3, (f.p(f.a).y + f.p(f.b).y + f.p(f.c).y) / 3};
    const Id picked = f.view.pickAt(f.view.toScreen({mid.x, mid.y}));
    REQUIRE(picked == f.groups().at(0));
    f.view.setSelection({picked});
    f.view.deleteSelection();
    CHECK(f.groups().empty());
}

TEST_CASE("A dimension inside a group is held by it", "[group]")
{
    Fixture f;
    const Id dim = f.doc.addConstraint(ConstraintType::Distance, {f.tri[0]}, 30.0);
    REQUIRE(dim != kNoId);
    REQUIRE(f.doc.addGroup(f.tri) != kNoId);
    CHECK(f.doc.solveResult().redundant.empty());
    QString error;
    CHECK(!f.doc.setDimensionValue(dim, 40.0, &error));
    CHECK(error.contains(QStringLiteral("group")));
    CHECK(f.d(f.a, f.b) == Approx(30));
}
