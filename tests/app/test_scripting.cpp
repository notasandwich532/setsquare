// Python integration: the macro recorder's output replays to the same sketch, console name
// resolution, script transactions and error reporting.

#include "Document.h"
#include "PythonHost.h"
#include "SketchView.h"

#include <core/Serialize.h>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <QCoreApplication>
#include <QLineEdit>
#include <QTest>

#include <cmath>

using namespace core;
using Catch::Approx;

namespace
{

// Points the Python host at a document for the duration of a test, capturing output.
struct ScriptScope
{
    QString out, err;
    explicit ScriptScope(Document& doc)
    {
        REQUIRE(script::isInitialised());
        script::setDocument(&doc);
        script::setUiHooks({});
        script::setOutput([this](const QString& text, bool isError) { (isError ? err : out) += text; });
        script::resetConsole();
    }
    ~ScriptScope()
    {
        script::setOutput(nullptr);
        script::setDocument(nullptr);
    }
};

struct Fixture
{
    Document doc;
    SketchView view{doc};
    QStringList recorded;

    Fixture()
    {
        view.resize(800, 600);
        view.show();
        view.resetView();
        QObject::connect(&doc, &Document::recorded, [this](const QString& py) { recorded << py; });
    }

    QPoint at(double x, double y) const { return view.toScreen({x, y}).toPoint(); }
    void click(double x, double y) { QTest::mouseClick(&view, Qt::LeftButton, {}, at(x, y)); }
    void move(double x, double y) { QTest::mouseMove(&view, at(x, y)); }
    void key(Qt::Key k) { QTest::keyClick(&view, k); }
    void drag(double x0, double y0, double x1, double y1)
    {
        QTest::mousePress(&view, Qt::LeftButton, {}, at(x0, y0));
        for (int i = 1; i <= 6; ++i) {
            move(x0 + (x1 - x0) * i / 6.0, y0 + (y1 - y0) * i / 6.0);
        }
        QTest::mouseRelease(&view, Qt::LeftButton, {}, at(x1, y1));
    }
    QLineEdit* editor() const
    {
        QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
        return view.findChild<QLineEdit*>("dimensionEditor");
    }
};

void requireSameSketch(const Sketch& a, const Sketch& b)
{
    REQUIRE(a.entities().size() == b.entities().size());
    REQUIRE(a.constraints().size() == b.constraints().size());
    CHECK(a.origin() == b.origin());
    for (const auto& [id, e] : a.entities()) {
        REQUIRE(b.hasEntity(id));
        const Entity& f = b.entity(id);
        CHECK(f.kind() == e.kind());
        CHECK(f.fixed == e.fixed);
        CHECK(f.construction == e.construction);
        CHECK(b.definingPoints(id) == a.definingPoints(id));
        if (e.kind() == EntityKind::Point) {
            CHECK(b.point(id).x == Approx(a.point(id).x).margin(1e-6));
            CHECK(b.point(id).y == Approx(a.point(id).y).margin(1e-6));
        }
        if (e.kind() == EntityKind::Circle) {
            CHECK(b.circle(id).radius == Approx(a.circle(id).radius).margin(1e-6));
        }
    }
    for (const auto& [id, c] : a.constraints()) {
        REQUIRE(b.hasConstraint(id));
        const Constraint& k = b.constraint(id);
        CHECK(k.type == c.type);
        CHECK(k.refs == c.refs);
        CHECK(k.value == Approx(c.value));
        CHECK(k.labelX == Approx(c.labelX));
        CHECK(k.labelY == Approx(c.labelY));
    }
}

}  // namespace

TEST_CASE("Replaying recorded UI actions reproduces the sketch", "[scripting][recorder]")
{
    Fixture f;

    // A session touching most tools.
    f.view.setTool(ToolKind::Rectangle);
    f.click(0, 0);  // on the origin
    f.click(30, 20);
    f.view.setTool(ToolKind::Line);
    f.click(50, 0);
    f.move(70, 0.3);
    f.click(70, 0.3);  // inferred horizontal
    f.click(60, 15);
    f.key(Qt::Key_Escape);
    f.view.setTool(ToolKind::Circle);
    f.click(15, 10);
    f.click(20, 10);
    f.view.setTool(ToolKind::ArcCenter);
    f.click(-30, 0);
    f.click(-20, 0);
    f.move(-23, 7);
    f.move(-30, 10);
    f.click(-30, 10);
    f.view.setTool(ToolKind::Dimension);
    f.click(15, 0);  // bottom of the rectangle
    f.click(15, -8);
    REQUIRE(f.editor());
    f.editor()->selectAll();
    QTest::keyClicks(f.editor(), "40");
    QTest::keyClick(f.editor(), Qt::Key_Return);
    f.view.setTool(ToolKind::Equal);
    f.click(60, 0);    // the inferred-horizontal line
    f.click(65, 7.5);  // the diagonal one
    f.view.setTool(ToolKind::Fix);
    f.click(-30, 0);  // arc centre
    f.view.setTool(ToolKind::Select);
    f.drag(40, 20, 40, 30);  // the rectangle's free corner: height changes
    f.click(15, 10 - 5);     // circle rim... select it
    f.key(Qt::Key_Delete);
    f.doc.undo();  // changed our mind

    REQUIRE(f.recorded.size() > 8);
    INFO(f.recorded.join('\n').toStdString());

    Document replay;
    ScriptScope scope(replay);
    const script::RunResult r = script::runConsole(f.recorded.join('\n'));
    INFO(scope.err.toStdString());
    REQUIRE(r.ok);
    requireSameSketch(f.doc.sketch(), replay.sketch());
}

TEST_CASE("Console names like p12 resolve to sketch items", "[scripting][console]")
{
    Document doc;
    ScriptScope scope(doc);
    REQUIRE(script::runConsole("ln = s.line((0, 0), (30, 40))").ok);
    const Id id = doc.sketch().entities().rbegin()->first;  // the line, created last
    REQUIRE(script::runConsole(QStringLiteral("l%1.length").arg(id)).ok);  // expression: printed
    CHECK(scope.out.trimmed() == "50.0");
    REQUIRE(script::runConsole(QStringLiteral("print(ln == s[%1], s.origin.fixed)").arg(id)).ok);
    CHECK(scope.out.contains("True True"));
}

TEST_CASE("A script is one undo step; console commands are separate steps", "[scripting][undo]")
{
    Document doc;
    ScriptScope scope(doc);
    REQUIRE(script::runSource("s.rectangle((0, 0), (10, 10))\ns.circle((5, 5), 2)\ns.point(20, 20)").ok);
    CHECK(doc.sketch().entities().size() == 5 + 8 + 2 + 1);  // 5: origin and axes
    doc.undo();
    CHECK(doc.sketch().entities().size() == 5);
    CHECK_FALSE(doc.canUndo());

    REQUIRE(script::runConsole("s.point(1, 1)\ns.point(2, 2)").ok);
    doc.undo();
    CHECK(doc.sketch().entities().size() == 6);  // origin, axes + first point
}

TEST_CASE("Errors come back as Python exceptions with a traceback", "[scripting][errors]")
{
    Document doc;
    ScriptScope scope(doc);
    const script::RunResult r = script::runSource("s.horizontal(s.origin)", "bad.py");
    CHECK_FALSE(r.ok);
    CHECK(r.exitCode == 1);
    CHECK(scope.err.contains("ValueError"));
    CHECK(scope.err.contains("bad.py"));

    scope.err.clear();
    CHECK(script::runSource("raise SystemExit(3)").exitCode == 3);
    CHECK(script::runSource("import sys; sys.exit()").ok);
}

TEST_CASE("Scripts run on the bundled Python, isolated from the system", "[scripting]")
{
    Document doc;
    ScriptScope scope(doc);
    REQUIRE(script::runSource("import sys, json, math, re; print(sys.version_info[:2], sys.flags.isolated)").ok);
    CHECK(scope.out.contains(", 1"));  // isolated mode
}
