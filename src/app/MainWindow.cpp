#include "MainWindow.h"

#include "SketchView.h"

#include <QAction>
#include <QKeySequence>
#include <QLabel>
#include <QMenuBar>
#include <QStatusBar>

MainWindow::MainWindow(QWidget* parent)
    : QMainWindow(parent)
    , m_view(new SketchView(this))
    , m_cursorLabel(new QLabel(this))
{
    setWindowTitle(tr("Drawing App"));
    setCentralWidget(m_view);
    resize(1280, 800);

    m_cursorLabel->setMinimumWidth(180);
    statusBar()->addPermanentWidget(m_cursorLabel);
    statusBar()->showMessage(tr("Middle-drag to pan, wheel to zoom"));

    connect(m_view, &SketchView::cursorMoved, this, [this](QPointF p) {
        m_cursorLabel->setText(QStringLiteral("X %1   Y %2")
                                   .arg(p.x(), 0, 'f', 2)
                                   .arg(p.y(), 0, 'f', 2));
    });

    createActions();
}

void MainWindow::createActions()
{
    auto* fileMenu = menuBar()->addMenu(tr("&File"));
    auto* quit = fileMenu->addAction(tr("&Quit"));
    quit->setShortcut(QKeySequence::Quit);
    connect(quit, &QAction::triggered, this, &QWidget::close);

    auto* viewMenu = menuBar()->addMenu(tr("&View"));
    auto* home = viewMenu->addAction(tr("&Home View"));
    home->setShortcut(Qt::Key_Home);
    connect(home, &QAction::triggered, m_view, &SketchView::resetView);
}
