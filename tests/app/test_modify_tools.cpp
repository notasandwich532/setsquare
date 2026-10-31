// Trim / Extend / Break / Fillet / Chamfer tools, driven with simulated input.

#include "Document.h"
#include "PythonHost.h"
#include "SketchView.h"
#include "Snapshot.h"

#include <core/Geometry.h>
#include <core/Serialize.h>

#include <QFile>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <QCoreApplication>
#include <QLineEdit>
#include <QTest>

#include <algorithm>
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

    Fixture()
    {
        view.resize(800, 600);
        view.show();
        view.resetView();
        QObject::connect(&doc, &Document::recorded, [this](const QString& py) { recorded << py; });
    }
    ~Fixture() { saveSnapshot(view); }

    QPoint at(double x, double y) const { return view.toScreen({x, y}).toPoint(); }
    void click(double x, double y) { QTest::mouseClick(&view, Qt::LeftButton, {}, at(x, y)); }
    void move(double x, double y) { QTest::mouseMove(&view, at(x, y)); }
    const Sketch& sketch() const { return doc.sketch(); }
    size_t count(EntityKind kind, bool construction = false) const
    {
        return static_cast<size_t>(std::ranges::count_if(sketch().entities(), [&](const auto& kv) {
            return kv.second.kind() == kind && kv.second.construction == construction
                && !sketch().isReference(kv.first);
        }));
    }
    QLineEdit* editor() const
    {
        QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
        return view.findChild<QLineEdit*>("dimensionEditor");
    }
    void type(const QString& text)
    {
        QLineEdit* e = editor();
        REQUIRE(e != nullptr);
        e->selectAll();
        QTest::keyClicks(e, text);
        QTest::keyClick(e, Qt::Key_Return);
    }
    void rectangle()  // 10,10 .. 50,40, away from the axes
    {
        view.setTool(ToolKind::Rectangle);
        click(10, 10);
        click(50, 40);
    }
    Id onlyArc() const
    {
        for (const auto& [id, e] : sketch().entities()) {
            if (e.kind() == EntityKind::Arc && !e.construction) {
                return id;
            }
        }
        return kNoId;
    }
};

}  // namespace

TEST_CASE("Fillet a corner by clicking its point, then type the radius", "[modify][fillet]")
{
    Fixture f;
    f.rectangle();
    f.view.setTool(ToolKind::Fillet);
    f.click(50, 10);  // bottom-right corner
    REQUIRE(f.onlyArc() != kNoId);
    CHECK(f.sketch().arcRadius(f.onlyArc()) == Approx(5.0));  // the default
    f.type("3");
    CHECK(f.sketch().arcRadius(f.onlyArc()) == Approx(3.0).margin(1e-6));
    CHECK(f.count(EntityKind::Line, true) == 2);  // the sides that met at the corner
    const SolveResult& r = f.doc.solveResult();
    CHECK(r.ok);
    CHECK(r.redundant.empty());
    CHECK(r.dof == 4);  // as the rectangle was
}

TEST_CASE("Fillet by clicking two curves", "[modify][fillet]")
{
    Fixture f;
    f.rectangle();
    f.view.modifyOptions().filletRadius = 7;
    f.view.setTool(ToolKind::Fillet);
    f.click(30, 40);  // top
    f.click(10, 25);  // left
    QTest::keyClick(f.editor(), Qt::Key_Escape);
    REQUIRE(f.onlyArc() != kNoId);
    const ArcShape a = arcShape(f.sketch(), f.onlyArc());
    CHECK(a.center.x == Approx(17.0).margin(1e-6));
    CHECK(a.center.y == Approx(33.0).margin(1e-6));
}

TEST_CASE("Chamfer variants set their dimensions", "[modify][chamfer]")
{
    Fixture f;
    f.rectangle();
    f.view.modifyOptions().chamferDistance = 6;
    f.view.modifyOptions().chamferAngle = 30;
    f.view.setTool(ToolKind::ChamferDistanceAngle);
    f.click(50, 40);  // top-right corner
    QTest::keyClick(f.editor(), Qt::Key_Escape);
    const auto angles = std::ranges::count_if(f.sketch().constraints(), [](const auto& kv) {
        return kv.second.type == ConstraintType::Angle && kv.second.value == 30.0;
    });
    CHECK(angles == 1);
    const SolveResult& r = f.doc.solveResult();
    CHECK(r.ok);
    CHECK(r.redundant.empty());
    CHECK(r.dof == 4);
}

TEST_CASE("Trim, extend and break with the mouse", "[modify]")
{
    Fixture f;
    Id h = kNoId;
    f.doc.modify("lines", [&](Sketch& s) {
        h = s.addLine(10, 10, 60, 10);
        s.addLine(30, 0, 30, 20);
        s.addLine(80, 0, 80, 20);
    });
    f.view.setTool(ToolKind::Trim);
    f.move(50, 10);
    f.click(50, 10);  // the right part of h
    CHECK(f.sketch().point(f.sketch().line(h).p2).x == Approx(30.0));

    f.view.setTool(ToolKind::Extend);
    f.click(28, 10);  // near h's end: extends past the vertical it touches, to the next one
    CHECK(f.sketch().point(f.sketch().line(h).p2).x == Approx(80.0));

    f.view.setTool(ToolKind::Break);
    f.click(20, 10);
    CHECK(f.count(EntityKind::Line) == 4);  // h split at x = 30
}

TEST_CASE("Replaying recorded modify tools reproduces the sketch", "[modify][recorder]")
{
    Fixture f;
    f.rectangle();
    f.view.setTool(ToolKind::Fillet);
    f.click(50, 10);
    f.type("4");
    f.view.setTool(ToolKind::ChamferEqual);
    f.click(10, 40);
    f.type("3");
    f.view.setTool(ToolKind::Line);
    f.click(30, 0);
    f.click(30, 50);
    QTest::keyClick(&f.view, Qt::Key_Escape);
    f.view.setTool(ToolKind::Trim);
    f.click(30, 45);  // the bit poking out of the top
    REQUIRE(f.recorded.size() >= 5);

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
    if (const QString dir = qEnvironmentVariable("DRAWAPP_DEBUG_DIR"); !dir.isEmpty()) {
        QFile script(dir + "/recorded.py");
        if (script.open(QIODevice::WriteOnly)) {
            script.write(f.recorded.join('\n').toUtf8());
        }
        QFile a(dir + "/original.json"), b(dir + "/replay.json");
        if (a.open(QIODevice::WriteOnly) && b.open(QIODevice::WriteOnly)) {
            a.write(QByteArray::fromStdString(core::toJson(f.sketch())));
            b.write(QByteArray::fromStdString(core::toJson(replay.sketch())));
        }
    }
    REQUIRE(replay.sketch().entities().size() == f.sketch().entities().size());
    REQUIRE(replay.sketch().constraints().size() == f.sketch().constraints().size());
    for (const auto& [id, e] : f.sketch().entities()) {
        REQUIRE(replay.sketch().hasEntity(id));
        CHECK(replay.sketch().entity(id).construction == e.construction);
        if (e.kind() == EntityKind::Point) {
            CHECK(replay.sketch().point(id).x == Approx(f.sketch().point(id).x).margin(1e-6));
            CHECK(replay.sketch().point(id).y == Approx(f.sketch().point(id).y).margin(1e-6));
        }
    }
}
