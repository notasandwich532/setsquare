#include "MainWindow.h"

#include "ConstraintPanel.h"
#include "Document.h"
#include "PythonHost.h"
#include "ScriptConsole.h"
#include "SketchView.h"
#include "Theme.h"

#include <QDesktopServices>
#include <QMenu>
#include <QUrl>

#include <QAction>
#include <QActionGroup>
#include <QCloseEvent>
#include <QDir>
#include <QDockWidget>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QKeySequence>
#include <QLabel>
#include <QMenuBar>
#include <QMessageBox>
#include <QScreen>
#include <QStandardPaths>
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
    // 1280x800, but never bigger than the screen we open on (high-DPI laptops, small monitors).
    const QRect available = screen() ? screen()->availableGeometry() : QRect(0, 0, 1280, 800);
    resize(std::min(1280, available.width() * 9 / 10), std::min(800, available.height() * 9 / 10));
    move(available.center() - rect().center());

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
    connect(m_doc, &Document::fileStateChanged, this, &MainWindow::updateTitle);

    // Python scripts act on this window's document and can drive its view.
    script::setDocument(m_doc);
    script::UiHooks hooks;
    hooks.fit = [this] { m_view->fitAll(); };
    hooks.selection = [this] { return m_view->selection(); };
    hooks.select = [this](const std::set<core::Id>& ids) { m_view->setSelection(ids); };
    hooks.tool = [this] { return toolName(m_view->toolKind()); };
    hooks.setTool = [this](const QString& name) {
        const auto kind = toolFromName(name);
        if (kind) {
            m_view->setTool(*kind);
        }
        return kind.has_value();
    };
    hooks.message = [this](const QString& text) { statusBar()->showMessage(text, 5000); };
    script::setUiHooks(std::move(hooks));

    createActions();
    m_hintLabel->setText(m_view->hint());
    updateSolveStatus();
    updateTitle();
    m_view->setFocus();
}

MainWindow::~MainWindow()
{
    script::setUiHooks({});
    script::setDocument(nullptr);
}

namespace
{
const QString kFileFilter = QObject::tr("Sketches (*.dsk);;All files (*)");

const std::pair<ToolKind, const char*> kToolNames[] = {
    {ToolKind::Select, "select"},
    {ToolKind::Line, "line"},
    {ToolKind::Rectangle, "rectangle"},
    {ToolKind::Circle, "circle"},
    {ToolKind::ArcCenter, "arc"},
    {ToolKind::Arc3Point, "arc3"},
    {ToolKind::Dimension, "dimension"},
    {ToolKind::Coincident, "coincident"},
    {ToolKind::HorizontalVertical, "horizontal_vertical"},
    {ToolKind::Parallel, "parallel"},
    {ToolKind::Perpendicular, "perpendicular"},
    {ToolKind::Tangent, "tangent"},
    {ToolKind::Equal, "equal"},
    {ToolKind::Midpoint, "midpoint"},
    {ToolKind::Concentric, "concentric"},
    {ToolKind::Symmetric, "symmetric"},
    {ToolKind::Fix, "fix"},
};

QString macrosFolder()
{
    const QString dir = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation) + QStringLiteral("/macros");
    QDir().mkpath(dir);
    return dir;
}
}  // namespace

QString MainWindow::toolName(ToolKind kind)
{
    for (const auto& [k, name] : kToolNames) {
        if (k == kind) {
            return QString::fromLatin1(name);
        }
    }
    return {};
}

std::optional<ToolKind> MainWindow::toolFromName(const QString& name)
{
    for (const auto& [k, n] : kToolNames) {
        if (name == QLatin1String(n)) {
            return k;
        }
    }
    return std::nullopt;
}

void MainWindow::runScript(const QString& path)
{
    m_consoleDock->show();
    m_console->runFile(path);
    m_view->setFocus();
}

void MainWindow::updateTitle()
{
    const QString path = m_doc->filePath();
    const QString name = path.isEmpty() ? tr("Untitled") : QFileInfo(path).fileName();
    setWindowTitle(QStringLiteral("%1%2 - %3").arg(name, m_doc->isModified() ? QStringLiteral("*") : QString(),
                                                   tr("Drawing App")));
}

bool MainWindow::maybeSave()
{
    if (!m_doc->isModified()) {
        return true;
    }
    const auto answer = QMessageBox::warning(
        this, tr("Unsaved changes"), tr("Save changes to %1?").arg(m_doc->filePath().isEmpty()
                                                                      ? tr("Untitled")
                                                                      : QFileInfo(m_doc->filePath()).fileName()),
        QMessageBox::Save | QMessageBox::Discard | QMessageBox::Cancel, QMessageBox::Save);
    if (answer == QMessageBox::Save) {
        return save();
    }
    return answer == QMessageBox::Discard;
}

bool MainWindow::save()
{
    if (m_doc->filePath().isEmpty()) {
        return saveAs();
    }
    QString error;
    if (!m_doc->save(m_doc->filePath(), &error)) {
        QMessageBox::critical(this, tr("Save failed"), error);
        return false;
    }
    statusBar()->showMessage(tr("Saved %1").arg(QDir::toNativeSeparators(m_doc->filePath())), 3000);
    return true;
}

bool MainWindow::saveAs()
{
    const QString start = m_doc->filePath().isEmpty()
        ? QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation) + QStringLiteral("/untitled.dsk")
        : m_doc->filePath();
    QString path = QFileDialog::getSaveFileName(this, tr("Save Sketch"), start, kFileFilter);
    if (path.isEmpty()) {
        return false;
    }
    if (QFileInfo(path).suffix().isEmpty()) {
        path += QStringLiteral(".dsk");
    }
    QString error;
    if (!m_doc->save(path, &error)) {
        QMessageBox::critical(this, tr("Save failed"), error);
        return false;
    }
    statusBar()->showMessage(tr("Saved %1").arg(QDir::toNativeSeparators(path)), 3000);
    return true;
}

void MainWindow::open()
{
    if (!maybeSave()) {
        return;
    }
    const QString start = m_doc->filePath().isEmpty()
        ? QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation)
        : QFileInfo(m_doc->filePath()).absolutePath();
    const QString path = QFileDialog::getOpenFileName(this, tr("Open Sketch"), start, kFileFilter);
    if (!path.isEmpty()) {
        openFile(path);
    }
}

bool MainWindow::openFile(const QString& path)
{
    QString error;
    if (!m_doc->open(path, &error)) {
        QMessageBox::critical(this, tr("Open failed"), error);
        return false;
    }
    m_view->setTool(ToolKind::Select);
    m_view->fitAll();
    return true;
}

void MainWindow::newSketch()
{
    if (maybeSave()) {
        m_doc->newSketch();
        m_view->setTool(ToolKind::Select);
        m_view->resetView();
    }
}

void MainWindow::closeEvent(QCloseEvent* event)
{
    if (maybeSave()) {
        event->accept();
    }
    else {
        event->ignore();
    }
}

void MainWindow::createActions()
{
    // --- File ---
    auto* fileMenu = menuBar()->addMenu(tr("&File"));
    auto* newSketch = fileMenu->addAction(tr("&New Sketch"));
    newSketch->setShortcut(QKeySequence::New);
    connect(newSketch, &QAction::triggered, this, &MainWindow::newSketch);
    auto* open = fileMenu->addAction(tr("&Open..."));
    open->setShortcut(QKeySequence::Open);
    connect(open, &QAction::triggered, this, &MainWindow::open);
    auto* save = fileMenu->addAction(tr("&Save"));
    save->setShortcut(QKeySequence::Save);
    connect(save, &QAction::triggered, this, &MainWindow::save);
    auto* saveAs = fileMenu->addAction(tr("Save &As..."));
    saveAs->setShortcut(QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_S));
    connect(saveAs, &QAction::triggered, this, &MainWindow::saveAs);
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
    auto addTools = [&](QToolBar* bar, std::initializer_list<ToolDef> defs) {
        for (const ToolDef& t : defs) {
            auto* action = new QAction(t.text, this);
            action->setCheckable(true);
            if (!t.key.isEmpty()) {
                action->setShortcut(t.key);
                action->setToolTip(QStringLiteral("%1 (%2)").arg(t.text, t.key.toString(QKeySequence::NativeText)));
            }
            group->addAction(action);
            sketchMenu->addAction(action);
            bar->addAction(action);
            const ToolKind kind = t.kind;
            connect(action, &QAction::triggered, this, [this, kind] { m_view->setTool(kind); });
            m_toolActions[kind] = action;
        }
    };

    addTools(toolbar, {
                          {ToolKind::Select, tr("Select"), QKeySequence(Qt::Key_S)},
                          {ToolKind::Line, tr("Line"), QKeySequence(Qt::Key_L)},
                          {ToolKind::Rectangle, tr("Rectangle"), QKeySequence(Qt::Key_R)},
                          {ToolKind::Circle, tr("Circle"), QKeySequence(Qt::Key_C)},
                          {ToolKind::ArcCenter, tr("Arc"), QKeySequence(Qt::Key_A)},
                          {ToolKind::Arc3Point, tr("3-Point Arc"), QKeySequence(Qt::SHIFT | Qt::Key_A)},
                      });

    sketchMenu->addSeparator();
    addToolBarBreak();
    auto* constraintBar = addToolBar(tr("Constraints"));
    constraintBar->setObjectName("ConstraintToolbar");
    constraintBar->setMovable(false);
    constraintBar->setToolButtonStyle(Qt::ToolButtonTextOnly);
    addTools(constraintBar, {
                                {ToolKind::Dimension, tr("Dimension"), QKeySequence(Qt::Key_D)},
                                {ToolKind::Coincident, tr("Coincident"), QKeySequence(Qt::Key_O)},
                                {ToolKind::HorizontalVertical, tr("Horiz/Vert"), QKeySequence(Qt::Key_H)},
                                {ToolKind::Parallel, tr("Parallel"), {}},
                                {ToolKind::Perpendicular, tr("Perpendicular"), {}},
                                {ToolKind::Tangent, tr("Tangent"), QKeySequence(Qt::Key_T)},
                                {ToolKind::Equal, tr("Equal"), QKeySequence(Qt::Key_E)},
                                {ToolKind::Midpoint, tr("Midpoint"), {}},
                                {ToolKind::Concentric, tr("Concentric"), {}},
                                {ToolKind::Symmetric, tr("Symmetric"), {}},
                                {ToolKind::Fix, tr("Fix/Unfix"), QKeySequence(Qt::Key_F)},
                            });

    sketchMenu->addSeparator();
    auto* construction = sketchMenu->addAction(tr("Toggle &Construction"));
    construction->setShortcut(Qt::Key_X);
    connect(construction, &QAction::triggered, this, [this] {
        const auto selection = m_view->selection();
        m_view->tryEdit(tr("Construction"), [&](Editor& ed) {
            for (core::Id id : selection) {
                const core::Sketch& s = ed.sketch();
                if (s.hasEntity(id) && s.entity(id).kind() != core::EntityKind::Point) {
                    ed.setConstruction(id, !s.entity(id).construction);
                }
            }
        });
    });
    m_toolActions[ToolKind::Select]->setChecked(true);
    connect(m_view, &SketchView::toolChanged, this, [this](ToolKind kind) {
        if (auto it = m_toolActions.find(kind); it != m_toolActions.end()) {
            it->second->setChecked(true);
        }
    });

    // --- Docks ---
    auto* constraintsDock = new QDockWidget(tr("Constraints"), this);
    constraintsDock->setObjectName("ConstraintsDock");
    constraintsDock->setWidget(new ConstraintPanel(*m_doc, *m_view, constraintsDock));
    addDockWidget(Qt::RightDockWidgetArea, constraintsDock);
    resizeDocks({constraintsDock}, {300}, Qt::Horizontal);

    m_console = new ScriptConsole(*m_doc, this);
    m_consoleDock = new QDockWidget(tr("Python"), this);
    m_consoleDock->setObjectName("PythonDock");
    m_consoleDock->setWidget(m_console);
    addDockWidget(Qt::BottomDockWidgetArea, m_consoleDock);
    resizeDocks({m_consoleDock}, {180}, Qt::Vertical);

    // --- Macros ---
    auto* macroMenu = menuBar()->addMenu(tr("&Macros"));
    auto* runScriptAction = macroMenu->addAction(tr("&Run Script..."));
    runScriptAction->setShortcut(QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_R));
    connect(runScriptAction, &QAction::triggered, this, [this] {
        const QString path = QFileDialog::getOpenFileName(this, tr("Run Script"), macrosFolder(), tr("Python (*.py)"));
        if (!path.isEmpty()) {
            runScript(path);
        }
    });
    auto* openFolder = macroMenu->addAction(tr("Open Macros &Folder"));
    connect(openFolder, &QAction::triggered, this,
            [] { QDesktopServices::openUrl(QUrl::fromLocalFile(macrosFolder())); });
    auto* saveRecording = macroMenu->addAction(tr("&Save Recording as Macro..."));
    connect(saveRecording, &QAction::triggered, this, [this] {
        const QString path = QFileDialog::getSaveFileName(this, tr("Save Recording"), macrosFolder() + "/recorded.py",
                                                          tr("Python (*.py)"));
        if (path.isEmpty()) {
            return;
        }
        QFile file(path);
        if (!file.open(QIODevice::WriteOnly | QIODevice::Text)) {
            QMessageBox::critical(this, tr("Save failed"), file.errorString());
            return;
        }
        const QString body = QStringLiteral("# Recorded with Drawing App. `doc` and `s` (the sketch) are predefined.\n")
            + m_console->recording().join(QLatin1Char('\n')) + QLatin1Char('\n');
        file.write(body.toUtf8());
    });
    macroMenu->addSeparator();
    // The macros folder's scripts, listed fresh each time the menu opens.
    auto* macroList = macroMenu->addMenu(tr("Macros Folder"));
    connect(macroList, &QMenu::aboutToShow, this, [this, macroList] {
        macroList->clear();
        const QFileInfoList files = QDir(macrosFolder()).entryInfoList({QStringLiteral("*.py")}, QDir::Files, QDir::Name);
        if (files.isEmpty()) {
            macroList->addAction(tr("(none yet)"))->setEnabled(false);
        }
        for (const QFileInfo& fi : files) {
            connect(macroList->addAction(fi.completeBaseName()), &QAction::triggered, this,
                    [this, path = fi.absoluteFilePath()] { runScript(path); });
        }
    });

    // --- View ---
    auto* viewMenu = menuBar()->addMenu(tr("&View"));
    viewMenu->addAction(constraintsDock->toggleViewAction());
    viewMenu->addAction(m_consoleDock->toggleViewAction());
    auto* home = viewMenu->addAction(tr("&Fit All"));
    home->setShortcut(Qt::Key_Home);
    connect(home, &QAction::triggered, m_view, &SketchView::fitAll);
    auto* stats = viewMenu->addAction(tr("Performance &Stats"));
    stats->setCheckable(true);
    stats->setShortcut(Qt::Key_F12);
    connect(stats, &QAction::toggled, m_view, &SketchView::setShowStats);
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
    // Runs on every drag step: only touch the label when something changed, since
    // setStyleSheet() forces a style recalculation.
    if (text != m_solveLabel->text()) {
        m_solveLabel->setText(text);
    }
    const QString style = QStringLiteral("color: %1").arg(color.name());
    if (style != m_solveLabel->styleSheet()) {
        m_solveLabel->setStyleSheet(style);
    }
}
