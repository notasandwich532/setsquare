// Main window behaviour: the right-click recent-tools menu.

#include "MainWindow.h"
#include "SketchView.h"
#include "ToolSearch.h"

#include <catch2/catch_test_macros.hpp>

#include <QApplication>
#include <QLineEdit>
#include <QMenu>
#include <QTest>

TEST_CASE("Right-click shows the recently used tools", "[mainwindow][tools]")
{
    MainWindow window;
    window.show();
    auto* view = window.findChild<SketchView*>();
    REQUIRE(view != nullptr);

    for (ToolKind kind : {ToolKind::Line, ToolKind::Circle, ToolKind::Rectangle, ToolKind::Line,
                          ToolKind::Trim, ToolKind::Fillet, ToolKind::Dimension, ToolKind::Select}) {
        view->setTool(kind);
    }
    QTest::mouseClick(view, Qt::RightButton, {}, view->rect().center());
    auto* menu = qobject_cast<QMenu*>(QApplication::activePopupWidget());
    REQUIRE(menu != nullptr);

    QStringList items;
    for (QAction* a : menu->actions()) {
        if (!a->isSeparator()) {
            items << a->text();
        }
    }
    // The five most recent, newest first, no repeats; then Select.
    REQUIRE(items.size() == 6);
    CHECK(items[0] == "Dimension");
    CHECK(items[1] == "Fillet");
    CHECK(items[2] == "Trim");
    CHECK(items[3] == "Line");
    CHECK(items[4].contains("Rectangle"));
    CHECK(items[5] == "Select");

    menu->actions().at(1)->trigger();  // pick Fillet
    menu->close();
    CHECK(view->toolKind() == ToolKind::Fillet);
}

TEST_CASE("S opens a search box that finds and runs tools", "[mainwindow][tools]")
{
    MainWindow window;
    window.show();
    auto* view = window.findChild<SketchView*>();
    REQUIRE(view != nullptr);
    window.activateWindow();
    REQUIRE(QTest::qWaitForWindowActive(&window));  // shortcuts only reach the active window
    view->setFocus();
    QTest::keyClick(view, Qt::Key_S);
    auto* search = window.findChild<ToolSearch*>("toolSearch");
    REQUIRE(search != nullptr);
    CHECK(search->isVisible());

    // Ranking: names starting with the query first.
    const auto trims = search->matches(QStringLiteral("trim"));
    REQUIRE(trims.size() >= 2);
    CHECK(trims[0]->name == "Trim");
    const auto pattern = search->matches(QStringLiteral("circ pat"));
    REQUIRE(!pattern.empty());
    CHECK(pattern[0]->name == "Circular Pattern");

    auto* input = search->findChild<QLineEdit*>("toolSearchInput");
    REQUIRE(input != nullptr);
    QTest::keyClicks(input, "rect pat");
    QTest::keyClick(input, Qt::Key_Return);
    CHECK(!search->isVisible());
    CHECK(view->toolKind() == ToolKind::RectangularPattern);
}
