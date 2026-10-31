// Main window behaviour: the right-click recent-tools menu.

#include "MainWindow.h"
#include "SketchView.h"

#include <catch2/catch_test_macros.hpp>

#include <QApplication>
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
