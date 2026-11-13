// Main window behaviour: the right-click recent-tools menu.

#include "MainWindow.h"
#include "SketchView.h"
#include "ToolSearch.h"

#include <catch2/catch_test_macros.hpp>

#include <QApplication>
#include <QDir>
#include <QPainter>
#include <QLineEdit>
#include <QMenu>
#include <QTest>
#include <QToolBar>
#include <QToolButton>

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

TEST_CASE("The modify tools share the drawing tools' row, a tool button's width along", "[mainwindow][tools]")
{
    MainWindow window;
    window.resize(1280, 800);  // (the opening size on a normal screen)
    window.show();
    QApplication::processEvents();
    auto* sketchBar = window.findChild<QToolBar*>(QStringLiteral("SketchToolbar"));
    auto* modifyBar = window.findChild<QToolBar*>(QStringLiteral("ModifyToolbar"));
    REQUIRE(sketchBar != nullptr);
    REQUIRE(modifyBar != nullptr);
    CHECK(modifyBar->geometry().top() == sketchBar->geometry().top());
    CHECK(modifyBar->geometry().left() > sketchBar->geometry().right());
    auto* gapAction = modifyBar->findChild<QAction*>(QStringLiteral("ModifyGap"));
    REQUIRE(gapAction != nullptr);
    CHECK(modifyBar->actions().front() == gapAction);
    const QWidget* gap = modifyBar->widgetForAction(gapAction);
    const QWidget* line = sketchBar->widgetForAction(sketchBar->actions().at(1));  // (a plain tool button)
    CHECK(gap->width() == line->width());
    CHECK_FALSE(gap->isEnabled());
    INFO("sketch " << sketchBar->geometry().width() << ", modify " << modifyBar->geometry().width()
                   << ", window " << window.width());
    CHECK(modifyBar->geometry().right() < window.width());  // all of it shows at the opening size
}

TEST_CASE("The sketch toolbar shows one icon per kind of tool, the rest in its menu", "[mainwindow][tools]")
{
    MainWindow window;
    window.show();
    auto* view = window.findChild<SketchView*>();
    auto* bar = window.findChild<QToolBar*>(QStringLiteral("SketchToolbar"));
    REQUIRE(bar != nullptr);
    CHECK(bar->toolButtonStyle() == Qt::ToolButtonIconOnly);
    for (QToolButton* b : bar->findChildren<QToolButton*>()) {
        if (b->defaultAction() || b->menu()) {
            INFO(b->text().toStdString());
            CHECK_FALSE(b->icon().isNull());
        }
    }

    auto* circles = bar->findChild<QToolButton*>(QStringLiteral("circleButton"));
    REQUIRE(circles != nullptr);
    REQUIRE(circles->menu() != nullptr);
    CHECK(circles->menu()->actions().size() == 5);
    for (QAction* a : circles->menu()->actions()) {
        CHECK_FALSE(a->icon().isNull());  // the variants have their icons in the menu
    }

    // Clicking it starts the centre circle; picking another circle keeps it lit, and the icon.
    const QImage before = circles->icon().pixmap(24).toImage();
    QTest::mouseClick(circles, Qt::LeftButton, {}, QPoint(6, circles->height() / 2));
    CHECK(view->toolKind() == ToolKind::Circle);
    CHECK(circles->isChecked());
    circles->menu()->actions().at(2)->trigger();  // 3-point
    CHECK(view->toolKind() == ToolKind::Circle3Point);
    CHECK(circles->isChecked());
    CHECK(circles->icon().pixmap(24).toImage() == before);
    view->setTool(ToolKind::Line);
    CHECK_FALSE(circles->isChecked());
    // Clicking it while its tool is on leaves it on.
    QTest::mouseClick(circles, Qt::LeftButton, {}, QPoint(6, circles->height() / 2));
    QTest::mouseClick(circles, Qt::LeftButton, {}, QPoint(6, circles->height() / 2));
    CHECK(view->toolKind() == ToolKind::Circle);
    CHECK(circles->isChecked());

    if (const QString dir = qEnvironmentVariable("DRAWAPP_SNAPSHOT_DIR"); !dir.isEmpty()) {
        circles->menu()->popup(circles->mapToGlobal(QPoint(0, circles->height())));
        QImage shot = window.grab().toImage();
        QPainter painter(&shot);
        painter.drawPixmap(circles->mapTo(&window, QPoint(0, circles->height())), circles->menu()->grab());
        painter.end();
        circles->menu()->close();
        CHECK(shot.save(QDir(dir).filePath(QStringLiteral("toolbar.png"))));
    }
}