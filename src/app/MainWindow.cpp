#include "MainWindow.h"

#include "Document.h"
#include "SketchView.h"
#include "Theme.h"

#include <QAction>
#include <QActionGroup>
#include <QKeySequence>
#include <QLabel>
#include <QMenuBar>
#include <QStatusBar>
#include <QToolBar>

MainWindow::MainWindow(QWidget* parent)
    : QMainWindow(parent)
    , m_doc(new Document(this))
    , m_view(new SketchView(*m_doc, this))
    , m_hintLabel(new QLabel(this))
    , m_solveLabel(new QLabel(this))
    , m_cursorLabel(new QLabel(this))
{
    setWindowTitle(tr("Drawing App"));
    setCentralWidget(m_view);
    resize(1280, 800);

    m_cursorLabel->setMinimumWidth(170);
    m_solveLabel->setMinimumWidth(170);
    statusBar()->addWidget(m_hintLabel, 1);
    statusBar()->addPermanentWidget(m_solveLabel);
    statusBar()->addPermanentWidget(m_cursorLabel);

    connect(m_view, &SketchView::cursorMoved, this, [this](QPointF p) {
        m_cursorLabel->setText(QStringLiteral("X %1   Y %2").arg(p.x(), 0, 'f', 2).arg(p.y(), 0, 'f', 2));
    });
    connect(m_view, &SketchView::hintChanged, m_hintLabel, &QLabel::setText);
    connect(m_view, &SketchView::message, this, [this](const QString& text) {
        statusBar()->showMessage(text, 5000);
    });
    connect(m_doc, &Document::changed, this, &MainWindow::updateSolveStatus);

    createActions();
    m_hintLabel->setText(m_view->hint());
    updateSolveStatus();
    m_view->setFocus();
}

void MainWindow::createActions()
{
    // --- File ---
    auto* fileMenu = menuBar()->addMenu(tr("&File"));
    auto* newSketch = fileMenu->addAction(tr("&New Sketch"));
    newSketch->setShortcut(QKeySequence::New);
    connect(newSketch, &QAction::triggered, this, [this] { m_doc->reset(); });
    fileMenu->addSeparator();
    auto* quit = fileMenu->addAction(tr("&Quit"));
    quit->setShortcut(QKeySequence::Quit);
    connect(quit, &QAction::triggered, this, &QWidget::close);

    // --- Edit ---
    auto* editMenu = menuBar()->addMenu(tr("&Edit"));
    auto* undo = editMenu->addAction(tr("&Undo"));
    undo->setShortcut(QKeySequence::Undo);
    connect(undo, &QAction::triggered, m_doc, &Document::undo);
    auto* redo = editMenu->addAction(tr("&Redo"));
    redo->setShortcuts({QKeySequence::Redo, QKeySequence(Qt::CTRL | Qt::Key_Y)});
    connect(redo, &QAction::triggered, m_doc, &Document::redo);
    auto syncUndo = [this, undo, redo] {
        undo->setEnabled(m_doc->canUndo());
        redo->setEnabled(m_doc->canRedo());
    };
    connect(m_doc, &Document::undoStateChanged, this, syncUndo);
    syncUndo();
    editMenu->addSeparator();
    auto* selectAll = editMenu->addAction(tr("Select &All"));
    selectAll->setShortcut(QKeySequence::SelectAll);
    connect(selectAll, &QAction::triggered, m_view, &SketchView::selectAll);
    auto* del = editMenu->addAction(tr("&Delete"));
    del->setShortcutContext(Qt::WidgetShortcut);  // the view handles Delete/Backspace itself
    del->setShortcut(QKeySequence::Delete);
    connect(del, &QAction::triggered, m_view, &SketchView::deleteSelection);

    // --- Sketch tools ---
    auto* sketchMenu = menuBar()->addMenu(tr("&Sketch"));
    auto* toolbar = addToolBar(tr("Sketch"));
    toolbar->setObjectName("SketchToolbar");
    toolbar->setMovable(false);
    toolbar->setToolButtonStyle(Qt::ToolButtonTextOnly);
    auto* group = new QActionGroup(this);

    struct ToolDef
    {
        ToolKind kind;
        QString text;
        QKeySequence key;
    };
    const ToolDef tools[] = {
        {ToolKind::Select, tr("Select"), QKeySequence(Qt::Key_S)},
        {ToolKind::Line, tr("Line"), QKeySequence(Qt::Key_L)},
        {ToolKind::Rectangle, tr("Rectangle"), QKeySequence(Qt::Key_R)},
        {ToolKind::Circle, tr("Circle"), QKeySequence(Qt::Key_C)},
        {ToolKind::ArcCenter, tr("Arc"), QKeySequence(Qt::Key_A)},
        {ToolKind::Arc3Point, tr("3-Point Arc"), QKeySequence(Qt::SHIFT | Qt::Key_A)},
    };
    for (const ToolDef& t : tools) {
        auto* action = new QAction(t.text, this);
        action->setCheckable(true);
        action->setShortcut(t.key);
        action->setToolTip(QStringLiteral("%1 (%2)").arg(t.text, t.key.toString(QKeySequence::NativeText)));
        group->addAction(action);
        sketchMenu->addAction(action);
        toolbar->addAction(action);
        const ToolKind kind = t.kind;
        connect(action, &QAction::triggered, this, [this, kind] { m_view->setTool(kind); });
        m_toolActions[kind] = action;
    }
    m_toolActions[ToolKind::Select]->setChecked(true);
    connect(m_view, &SketchView::toolChanged, this, [this](ToolKind kind) {
        if (auto it = m_toolActions.find(kind); it != m_toolActions.end()) {
            it->second->setChecked(true);
        }
    });

    // --- View ---
    auto* viewMenu = menuBar()->addMenu(tr("&View"));
    auto* home = viewMenu->addAction(tr("&Home View"));
    home->setShortcut(Qt::Key_Home);
    connect(home, &QAction::triggered, m_view, &SketchView::resetView);
}

void MainWindow::updateSolveStatus()
{
    const core::SolveResult& r = m_doc->solveResult();
    QString text;
    QColor color = theme::canvas().underConstrained;
    if (!r.conflicting.empty()) {
        text = tr("Conflicting constraints");
        color = theme::canvas().conflict;
    }
    else if (m_doc->sketch().entities().empty()) {
        text = tr("Empty sketch");
        color = palette().color(QPalette::WindowText);
    }
    else if (r.dof == 0) {
        text = tr("Fully constrained");
        color = theme::canvas().fullyConstrained;
    }
    else {
        text = tr("%n degree(s) of freedom", nullptr, r.dof);
    }
    if (!r.redundant.empty()) {
        text += tr("  (%n redundant)", nullptr, static_cast<int>(r.redundant.size()));
    }
    m_solveLabel->setText(text);
    m_solveLabel->setStyleSheet(QStringLiteral("color: %1").arg(color.name()));
}
