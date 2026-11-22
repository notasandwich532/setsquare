// Parameters in the app: set, renamed and deleted as one undo step each, replayed from the
// history; typed into a dimension; and the Parameters panel.

#include "Document.h"
#include <algorithm>
#include <QFocusEvent>
#include <QAbstractSpinBox>
#include "NumberInput.h"
#include "CommandPanel.h"
#include "ParametersPanel.h"
#include "PythonHost.h"
#include "SketchView.h"

#include <core/Builders.h>
#include <core/Parameters.h>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <QCoreApplication>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QTableWidget>
#include <QTest>

using namespace core;
using Catch::Approx;

namespace
{
struct Fixture
{
    Document doc;
    build::Rectangle rect;
    Id width = kNoId;  // the bottom side's length

    Fixture()
    {
        doc.modify("rectangle", [&](Sketch& s) {
            rect = build::rectangle(s, s.addPoint(0, 0), s.addPoint(100, 50));
            width = s.addConstraint(ConstraintType::Distance, {rect.sides[0]}, 100);
        });
    }
    double length(Id line) const
    {
        const Line& l = doc.sketch().line(line);
        const Point &a = doc.sketch().point(l.p1), &b = doc.sketch().point(l.p2);
        return std::hypot(b.x - a.x, b.y - a.y);
    }
    double param(const char* name) const
    {
        const int at = parameters::find(doc.sketch(), name);
        REQUIRE(at >= 0);
        return doc.sketch().parameters()[static_cast<size_t>(at)].value;
    }
};
}  // namespace

TEST_CASE("Parameters: a dimension follows one; each change is an undo step; refused ones change nothing", "[parameters]")
{
    Fixture f;
    QString error;
    REQUIRE(f.doc.setParameter("w", "120", &error));
    REQUIRE(f.doc.setDimensionExpression(f.width, "w + 10", &error));
    CHECK(f.length(f.rect.sides[0]) == Approx(130.0));
    REQUIRE(f.doc.setParameter("w", "200", &error));
    CHECK(f.length(f.rect.sides[0]) == Approx(210.0));
    f.doc.undo();
    CHECK(f.length(f.rect.sides[0]) == Approx(130.0));
    CHECK(f.param("w") == Approx(120.0));

    // Refused: nothing changes, and it says why.
    CHECK(!f.doc.setParameter("w", "-50", &error));  // (the side would be -40)
    CHECK(error.contains("positive"));
    CHECK(!f.doc.deleteParameter("w", &error));
    CHECK(error.contains(QStringLiteral("d%1").arg(f.width)));
    CHECK(!f.doc.setDimensionExpression(f.width, "nope * 2", &error));
    CHECK(error.contains("don't know 'nope'"));
    CHECK(f.param("w") == Approx(120.0));
    CHECK(f.doc.sketch().constraint(f.width).expression == "w + 10");

    // Renamed: everywhere it's used. A plain value again: no expression.
    REQUIRE(f.doc.renameParameter("w", "Width", &error));
    CHECK(f.doc.sketch().constraint(f.width).expression == "Width + 10");
    REQUIRE(f.doc.setDimensionValue(f.width, 90, &error));
    CHECK(f.doc.sketch().constraint(f.width).expression.empty());
    REQUIRE(f.doc.deleteParameter("Width", &error));
    CHECK(f.doc.sketch().parameters().empty());
}

TEST_CASE("Parameters: the history rebuilds them, and what follows them", "[parameters]")
{
    Fixture f;
    QString error;
    REQUIRE(f.doc.setParameter("w", "120", &error));
    REQUIRE(f.doc.setParameter("half", "w / 2", &error));
    REQUIRE(f.doc.setDimensionExpression(f.width, "half", &error));
    REQUIRE(f.doc.renameParameter("half", "h", &error));
    REQUIRE(f.doc.setParameter("w", "300", &error));
    const QStringList lines = f.doc.history().mid(f.doc.history().size() - 5);
    CHECK(lines == QStringList{R"(s.parameter("w", "120"))", R"(s.parameter("half", "w / 2"))",
                               QStringLiteral(R"(d%1.value = "half")").arg(f.width), R"(s.rename_parameter("half", "h"))",
                               R"(s.parameter("w", "300"))"});

    // Replayed on the same starting drawing.
    Document replay;
    replay.modify("rectangle", [&](Sketch& s) {
        const build::Rectangle r = build::rectangle(s, s.addPoint(0, 0), s.addPoint(100, 50));
        s.addConstraint(ConstraintType::Distance, {r.sides[0]}, 100);
    });
    script::setDocument(&replay);
    const script::RunResult r = script::runConsole(lines.join('\n'));
    script::setDocument(nullptr);
    INFO(r.error.toStdString());
    REQUIRE(r.ok);
    REQUIRE(replay.sketch().parameters().size() == 2);
    CHECK(replay.sketch().parameters()[1].name == "h");
    CHECK(replay.sketch().constraint(f.width).expression == "h");
    CHECK(replay.sketch().constraint(f.width).value == Approx(150.0));
}

TEST_CASE("Parameters: typed into a dimension, it follows; its label says so; it opens showing the expression", "[parameters]")
{
    Fixture f;
    QString error;
    REQUIRE(f.doc.setParameter("w", "80", &error));
    SketchView view(f.doc);
    view.resize(900, 600);
    view.show();
    view.fitAll();
    CHECK(!view.formatDimension(f.doc.sketch().constraint(f.width)).startsWith(QStringLiteral("ƒ")));

    view.editValue(f.width, QPointF(450, 300));
    auto* editor = view.findChild<QLineEdit*>("dimensionEditor");
    REQUIRE(editor != nullptr);
    editor->setText("w * 1.5");
    QTest::keyClick(editor, Qt::Key_Return);
    QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    CHECK(f.doc.sketch().constraint(f.width).expression == "w * 1.5");
    CHECK(f.length(f.rect.sides[0]) == Approx(120.0));
    CHECK(view.formatDimension(f.doc.sketch().constraint(f.width)) == QStringLiteral("ƒ 120"));

    view.editValue(f.width, QPointF(450, 300));
    editor = view.findChild<QLineEdit*>("dimensionEditor");
    REQUIRE(editor != nullptr);
    CHECK(editor->text() == "w * 1.5");
    editor->setText("75");  // (a number: a plain value again)
    QTest::keyClick(editor, Qt::Key_Return);
    CHECK(f.doc.sketch().constraint(f.width).expression.empty());
    CHECK(f.length(f.rect.sides[0]) == Approx(75.0));
}

TEST_CASE("Parameters panel: edit an expression or a name; what can't be done is said, and goes back; add, remove", "[parameters]")
{
    Fixture f;
    QString error;
    REQUIRE(f.doc.setParameter("w", "120", &error));
    REQUIRE(f.doc.setDimensionExpression(f.width, "w", &error));
    ParametersPanel panel(f.doc);
    panel.show();
    auto* table = panel.findChild<QTableWidget*>("parametersTable");
    auto* problem = panel.findChild<QLabel*>("parameterProblem");
    REQUIRE(table != nullptr);
    REQUIRE(table->rowCount() == 1);
    CHECK(table->item(0, 0)->text() == "w");
    CHECK(table->item(0, 2)->text() == "120");
    CHECK(table->item(0, 0)->toolTip().contains(QStringLiteral("d%1").arg(f.width)));

    table->item(0, 1)->setText("150");  // (as typing into the cell)
    CHECK(f.length(f.rect.sides[0]) == Approx(150.0));
    CHECK(table->item(0, 2)->text() == "150");

    table->item(0, 0)->setText("width");
    CHECK(f.doc.sketch().constraint(f.width).expression == "width");

    table->item(0, 1)->setText("width + 1");  // a loop: refused
    CHECK(problem->isVisibleTo(&panel));
    CHECK(problem->text().contains("loop"));
    QCoreApplication::processEvents();
    CHECK(table->item(0, 1)->text() == "150");  // (back)

    panel.findChild<QPushButton*>("addParameter")->click();
    REQUIRE(table->rowCount() == 2);
    CHECK(table->item(1, 0)->text() == "param1");
    table->setCurrentCell(1, 0);
    panel.findChild<QPushButton*>("removeParameter")->click();
    CHECK(table->rowCount() == 1);
    table->setCurrentCell(0, 0);
    panel.findChild<QPushButton*>("removeParameter")->click();  // (used: refused)
    CHECK(table->rowCount() == 1);
    CHECK(problem->text().contains("used by"));
}
TEST_CASE("Number boxes take parameters: kept as an expression (marked), shown again to edit, dropped by the arrows", "[parameters][ui]")
{
    setNumberInputNames([](const std::string& name) -> std::optional<double> {
        return name == "w" ? std::optional<double>(120.0) : std::nullopt;
    });
    ExpressionSpinBox box;
    box.setRange(-1e6, 1e6);
    box.show();
    box.setFocus();
    box.findChild<QLineEdit*>()->selectAll();
    QTest::keyClicks(&box, "w / 4");
    QTest::keyClick(&box, Qt::Key_Return);
    CHECK(box.value() == Approx(30.0));
    CHECK(box.expression() == "w / 4");
    CHECK(box.text().startsWith(QStringLiteral("ƒ")));
    box.clearFocus();
    QFocusEvent in(QEvent::FocusIn);
    QCoreApplication::sendEvent(&box, &in);
    CHECK(box.findChild<QLineEdit*>()->text() == "w / 4");  // (to edit: the expression)
    static_cast<QAbstractSpinBox&>(box).stepBy(1);  // (the arrows)
    CHECK(box.expression().isEmpty());
    CHECK(box.value() == Approx(31.0));

    ExpressionCountBox count;
    count.setRange(1, 1000);
    CHECK(!count.setExpression("w / 7"));  // (not whole)
    CHECK(count.setExpression("w / 10"));
    CHECK(count.value() == 12);
    CHECK(count.expression() == "w / 10");
    setNumberInputNames({});
}

TEST_CASE("Pattern panels: a count or spacing typed as a parameter follows it, and opens showing it", "[parameters][ui]")
{
    Document doc;
    SketchView view(doc);
    view.resize(1000, 700);
    view.show();
    view.resetView();
    Id c = kNoId;
    doc.modify("circle", [&](Sketch& s) { c = s.addCircle(0, 0, 2); });
    QString error;
    REQUIRE(doc.setParameter("holes", "3", &error));
    auto circles = [&] {
        return std::ranges::count_if(doc.sketch().entities(), [](const auto& kv) { return kv.second.kind() == EntityKind::Circle; });
    };
    auto field = [&](const char* name) {
        QCoreApplication::processEvents();
        auto* panel = view.findChild<CommandPanel*>("commandPanel");
        REQUIRE(panel != nullptr);
        auto* w = panel->findChild<QAbstractSpinBox*>(name);
        REQUIRE(w != nullptr);
        return w;
    };
    // (Tab to the next box; Enter in one is the panel's OK, as it is for anyone typing.)
    auto type = [&](const char* name, const char* text, Qt::Key then) {
        QAbstractSpinBox* box = field(name);
        box->setFocus();
        box->selectAll();
        QTest::keyClicks(box, text);
        QTest::keyClick(box, then);
    };
    auto ok = [&] {
        view.findChild<CommandPanel*>("commandPanel")->findChild<QPushButton*>("ok")->click();
        QCoreApplication::processEvents();
        QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    };

    view.setSelection({c});
    view.setTool(ToolKind::RectangularPattern);
    type("quantity", "holes", Qt::Key_Tab);
    type("distance", "holes * 10", Qt::Key_Return);  // (Extent: shared between the gaps; Enter: OK)
    QCoreApplication::processEvents();
    QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    CHECK(circles() == 3);
    Id k = kNoId;
    for (const auto& [id, kc] : doc.sketch().constraints()) {
        if (kc.type == ConstraintType::LinearPattern) {
            k = id;
        }
    }
    REQUIRE(k != kNoId);
    CHECK(doc.sketch().constraint(k).settings.at("count") == "holes");
    CHECK(doc.sketch().constraint(k).value == Approx(15.0));  // 30 over 2 gaps

    REQUIRE(doc.setParameter("holes", "5", &error));
    CHECK(circles() == 5);
    for (const auto& [id, kc] : doc.sketch().constraints()) {
        if (kc.type == ConstraintType::LinearPattern) {
            k = id;
        }
    }
    CHECK(doc.sketch().constraint(k).value == Approx(12.5));  // 50 over 4

    view.editFeature(k);  // opened again: what it's worked out from
    CHECK(static_cast<ExpressionCountBox*>(field("quantity"))->expression() == "holes");
    CHECK(!static_cast<ExpressionSpinBox*>(field("distance"))->expression().isEmpty());
    view.setTool(ToolKind::Select);
}

TEST_CASE("Offset panel: a distance typed as a parameter follows it", "[parameters][ui]")
{
    Document doc;
    SketchView view(doc);
    view.resize(1000, 700);
    view.show();
    view.resetView();
    Id l = kNoId;
    doc.modify("line", [&](Sketch& s) { l = s.addLine(-20, 0, 20, 0); });
    QString error;
    REQUIRE(doc.setParameter("gap", "4", &error));
    view.setSelection({l});
    view.setTool(ToolKind::Offset);
    QCoreApplication::processEvents();
    auto* panel = view.findChild<CommandPanel*>("commandPanel");
    REQUIRE(panel != nullptr);
    auto* distance = panel->findChild<ExpressionSpinBox*>("distance");
    REQUIRE(distance != nullptr);
    distance->setFocus();
    distance->selectAll();
    QTest::keyClicks(distance, "gap");
    QTest::keyClick(distance, Qt::Key_Return);
    panel->findChild<QPushButton*>("ok")->click();
    QCoreApplication::processEvents();
    Id k = kNoId;
    for (const auto& [id, kc] : doc.sketch().constraints()) {
        if (kc.type == ConstraintType::Offset) {
            k = id;
        }
    }
    REQUIRE(k != kNoId);
    CHECK(doc.sketch().constraint(k).settings.at("distance") == "gap");
    REQUIRE(doc.setParameter("gap", "7", &error));
    CHECK(doc.sketch().constraint(k).value == Approx(7.0));
}
