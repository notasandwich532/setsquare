#include "MainWindow.h"

#include "ImportDialog.h"
#include "Recovery.h"
#include "Images.h"
#include "PageSetup.h"

#include <QImageReader>
#include "NumberInput.h"

#include "BrowserPanel.h"
#include "ColorBar.h"
#include "Icons.h"
#include "ConstraintPanel.h"
#include "FileAssociation.h"
#include "ParametersPanel.h"
#include "Document.h"
#include "PythonHost.h"
#include "ScriptConsole.h"
#include "SketchView.h"
#include "Theme.h"
#include "ToolSearch.h"

#include <core/Serialize.h>
#include <core/Transform.h>

#include <QClipboard>
#include <QCursor>
#include <QApplication>
#include <QSpinBox>
#include <QCheckBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDoubleSpinBox>
#include <QFormLayout>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QVBoxLayout>
#include <QDesktopServices>
#include <QGuiApplication>
#include <QMenu>
#include <QMimeData>
#include <functional>
#include <QToolButton>
#include <QUrl>

#include <QAction>
#include <QActionGroup>
#include <QCloseEvent>
#include <QSettings>
#include <QSignalBlocker>
#include <QLocale>
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
#include <QPainter>
#include <QToolBar>

namespace
{
// The toolbars' separator line: a quarter of the way from the background to the text, so it shows
// in the dark theme and the light (the style's own is too dark to see on the dark one).
QColor separatorColor(const QPalette& palette)
{
    const QColor a = palette.color(QPalette::Window);
    const QColor b = palette.color(QPalette::WindowText);
    return QColor::fromRgbF(a.redF() * 0.7f + b.redF() * 0.3f, a.greenF() * 0.7f + b.greenF() * 0.3f,
                            a.blueF() * 0.7f + b.blueF() * 0.3f);
}

// Between groups of tools on a toolbar: the separator line, with space either side.
class GroupDivider : public QWidget
{
public:
    using QWidget::QWidget;

protected:
    void paintEvent(QPaintEvent*) override
    {
        QPainter painter(this);
        painter.fillRect(QRect(width() / 2, 4, 1, height() - 8), separatorColor(palette()));
    }
};
}  // namespace

MainWindow::MainWindow(QWidget* parent)
    : QMainWindow(parent)
    , m_doc(new Document(this))
    , m_view(new SketchView(*m_doc, this))
    , m_hintLabel(new QLabel(this))
    , m_solveLabel(new QLabel(this))
    , m_cursorLabel(new QLabel(this))
{
    setWindowTitle(tr("Setsquare"));
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
    m_recovery = new Recovery(*m_doc, {}, this);  // autosave, for after a crash
    // Opened or saved (from the menus or from Python): to the top of File > Open Recent.
    connect(m_doc, &Document::fileStateChanged, this, [this] {
        if (!m_doc->filePath().isEmpty() && !m_doc->isModified()) {
            addRecentFile(m_doc->filePath());
        }
    });

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
    hooks.presentation = [this] { return m_view->presentation(); };
    hooks.setPresentation = [this](bool on) { m_view->setPresentation(on); };
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
const QString kFileFilter = QObject::tr("Sketches (*.setsq);;All files (*)");

const std::pair<ToolKind, const char*> kToolNames[] = {
    {ToolKind::Select, "select"},
    {ToolKind::Line, "line"},
    {ToolKind::Point, "point"},
    {ToolKind::Text, "text"},
    {ToolKind::Rectangle, "rectangle"},
    {ToolKind::Rectangle3Point, "rectangle_3point"},
    {ToolKind::RectangleCenter, "rectangle_center"},
    {ToolKind::Circle, "circle"},
    {ToolKind::Circle2Point, "circle_2point"},
    {ToolKind::Circle3Point, "circle_3point"},
    {ToolKind::CircleTangent2, "circle_tangent2"},
    {ToolKind::CircleTangent3, "circle_tangent3"},
    {ToolKind::ArcCenter, "arc"},
    {ToolKind::Arc3Point, "arc3"},
    {ToolKind::ArcTangent, "arc_tangent"},
    {ToolKind::PolygonInscribed, "polygon_inscribed"},
    {ToolKind::PolygonCircumscribed, "polygon_circumscribed"},
    {ToolKind::PolygonEdge, "polygon_edge"},
    {ToolKind::SlotCenterToCenter, "slot"},
    {ToolKind::SlotOverall, "slot_overall"},
    {ToolKind::SlotCenterPoint, "slot_center_point"},
    {ToolKind::SlotArc3Point, "arc_slot_3point"},
    {ToolKind::SlotArcCenter, "arc_slot"},
    {ToolKind::Collinear, "collinear"},
    {ToolKind::Trim, "trim"},
    {ToolKind::TrimToConstruction, "trim_construction"},
    {ToolKind::Extend, "extend"},
    {ToolKind::Break, "break"},
    {ToolKind::Fillet, "fillet"},
    {ToolKind::ChamferEqual, "chamfer"},
    {ToolKind::ChamferTwoDistances, "chamfer_2distances"},
    {ToolKind::ChamferDistanceAngle, "chamfer_angle"},
    {ToolKind::Move, "move"},
    {ToolKind::Scale, "scale"},
    {ToolKind::Mirror, "mirror"},
    {ToolKind::RectangularPattern, "rectangular_pattern"},
    {ToolKind::CircularPattern, "circular_pattern"},
    {ToolKind::Offset, "offset"},
    {ToolKind::Divide, "divide"},
    {ToolKind::Paste, "paste"},
    {ToolKind::StrokeBrush, "stroke_brush"},
    {ToolKind::Fill, "fill"},
    {ToolKind::Dimension, "dimension"},
    {ToolKind::Coincident, "coincident"},
    {ToolKind::HorizontalVertical, "horizontal_vertical"},
    {ToolKind::Parallel, "parallel"},
    {ToolKind::Perpendicular, "perpendicular"},
    {ToolKind::Tangent, "tangent"},
    {ToolKind::Equal, "equal"},
    {ToolKind::Ratio, "ratio"},
    {ToolKind::Midpoint, "midpoint"},
    {ToolKind::Concentric, "concentric"},
    {ToolKind::Symmetric, "symmetric"},
    {ToolKind::Fix, "fix"},
    {ToolKind::Project, "project"},
    {ToolKind::Ellipse, "ellipse"},
    {ToolKind::SplineFit, "spline"},
    {ToolKind::SplineControl, "spline_control"},
    {ToolKind::Gradient, "gradient"},
    {ToolKind::ResetFill, "reset_fill"},
    {ToolKind::Hatch, "hatch"},
    {ToolKind::Image, "image"},
    {ToolKind::Eyedropper, "eyedropper"},
    {ToolKind::Group, "group"},
    {ToolKind::Measure, "measure"},
    {ToolKind::Annotate, "annotate"},
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
                                                   tr("Setsquare")));
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
        ? QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation) + QStringLiteral("/untitled.setsq")
        : m_doc->filePath();
    QString path = QFileDialog::getSaveFileName(this, tr("Save Sketch"), start, kFileFilter);
    if (path.isEmpty()) {
        return false;
    }
    if (QFileInfo(path).suffix().isEmpty()) {
        path += QStringLiteral(".setsq");
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
    if (m_doc->historyFromOlderVersion()) {
        statusBar()->showMessage(tr("This drawing's command history was recorded by an older version: replaying it "
                                    "may not rebuild it exactly (File > Command History)"),
                                 8000);
    }
    return true;
}

QStringList MainWindow::recentFiles()
{
    return QSettings().value(QStringLiteral("recentFiles")).toStringList();
}

void MainWindow::addRecentFile(const QString& path)
{
    const QString full = QDir::cleanPath(QFileInfo(path).absoluteFilePath());
    QStringList list = recentFiles();
    list.removeIf([&](const QString& p) { return QString::compare(p, full, Qt::CaseInsensitive) == 0; });
    list.prepend(full);
    while (list.size() > kRecentFiles) {
        list.removeLast();
    }
    QSettings().setValue(QStringLiteral("recentFiles"), list);
}

void MainWindow::fillRecentMenu()
{
    m_recentFilesMenu->clear();
    const QStringList list = recentFiles();
    for (int i = 0; i < list.size(); ++i) {
        const QString path = list[i];
        // "&1 coaster.setsq": a number to pick it by, then its name (the folder in the tooltip).
        QAction* a = m_recentFilesMenu->addAction(
            QStringLiteral("&%1  %2").arg(i < 9 ? QString::number(i + 1) : QStringLiteral("1&0"), QFileInfo(path).fileName()));
        a->setToolTip(QDir::toNativeSeparators(path));
        a->setStatusTip(QDir::toNativeSeparators(path));
        connect(a, &QAction::triggered, this, [this, path] { openRecent(path); });
    }
    if (list.isEmpty()) {
        m_recentFilesMenu->addAction(tr("(none yet)"))->setEnabled(false);
        return;
    }
    m_recentFilesMenu->addSeparator();
    QAction* clear = m_recentFilesMenu->addAction(tr("&Clear Recent"));
    connect(clear, &QAction::triggered, this, [] { QSettings().remove(QStringLiteral("recentFiles")); });
}

void MainWindow::openRecent(const QString& path)
{
    if (!QFileInfo::exists(path)) {
        QStringList list = recentFiles();
        list.removeAll(path);
        QSettings().setValue(QStringLiteral("recentFiles"), list);
        QMessageBox::warning(this, tr("Open Recent"),
                             tr("%1 isn't there any more (moved, renamed or deleted), so it's been taken off the list.")
                                 .arg(QDir::toNativeSeparators(path)));
        return;
    }
    if (maybeSave()) {
        openFile(path);
    }
}

bool MainWindow::offerRecovery(const QString& folder)
{
    const std::vector<Recovery::Found> found = Recovery::left(folder);
    if (found.empty()) {
        return false;
    }
    const Recovery::Found& f = found.front();  // (the newest; any others are offered next time)
    const QString name = f.original.isEmpty() ? tr("an untitled drawing") : QFileInfo(f.original).fileName();
    QMessageBox box(QMessageBox::Warning, tr("Recover unsaved work?"),
                    tr("Setsquare didn't close properly last time. Your unsaved work on %1 was autosaved at %2.")
                        .arg(name, QLocale().toString(f.when, QLocale::ShortFormat)),
                    QMessageBox::NoButton, this);
    box.setObjectName(QStringLiteral("recoveryBox"));
    box.setInformativeText(found.size() > 1 ? tr("(There's more from other windows: they'll be offered next time.)")
                                            : QString());
    QPushButton* recover = box.addButton(tr("Recover"), QMessageBox::AcceptRole);
    QPushButton* discard = box.addButton(tr("Discard"), QMessageBox::DestructiveRole);
    box.addButton(tr("Not now"), QMessageBox::RejectRole);
    box.setDefaultButton(recover);
    box.exec();
    if (box.clickedButton() == discard) {
        Recovery::remove(f);
        return false;
    }
    if (box.clickedButton() != recover) {
        return false;  // (kept for next time)
    }
    QString error;
    if (!m_doc->openRecovered(f.copy, f.original, &error)) {
        QMessageBox::critical(this, tr("Recovery failed"), error);
        return false;
    }
    Recovery::remove(f);
    m_view->setTool(ToolKind::Select);
    m_view->fitAll();
    statusBar()->showMessage(tr("Recovered your unsaved work on %1: save it to keep it").arg(name), 10000);
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

namespace
{
const QString kSketchMime = QStringLiteral("application/x-setsquare-sketch");
}

void MainWindow::importDrawing(const QString& given)
{
    QString path = given;
    if (path.isEmpty()) {
        path = QFileDialog::getOpenFileName(this, tr("Import"), QString(), tr("Drawings (*.svg *.dxf)"));
        if (path.isEmpty()) {
            return;
        }
    }
    const core::Sketch& s = m_doc->sketch();
    ImportDialog dialog(path, QString::fromStdString(s.sublayer(s.activeSublayer()).name), s.page().has_value(), this);
    if (!dialog.readable()) {
        QMessageBox::warning(this, tr("Import"), tr("Can't import %1: %2").arg(QFileInfo(path).fileName(), dialog.error()));
        return;
    }
    if (dialog.exec() != QDialog::Accepted) {
        return;
    }
    importer::Options options = dialog.options();
    if (options.placement == importer::Placement::Center) {
        options.at = m_view->toWorld(QPointF(m_view->width() / 2.0, m_view->height() / 2.0));
    }
    importer::Report report;
    if (!m_view->tryEdit(tr("Import"), [&](Editor& ed) {
            report = ed.importFile(path, options, dialog.pxPerInch(), dialog.mmPerUnit());
        })) {
        return;
    }
    if (options.placement != importer::Placement::Center) {
        m_view->fitAll();
    }
    statusBar()->showMessage(tr("Imported %1: %2").arg(QFileInfo(path).fileName(), report.summary()), 10000);
}

void MainWindow::insertImage(const QString& given)
{
    QString path = given;
    if (path.isEmpty()) {
        QStringList patterns;
        for (const QByteArray& format : QImageReader::supportedImageFormats()) {
            patterns << QStringLiteral("*.") + QString::fromLatin1(format);
        }
        path = QFileDialog::getOpenFileName(this, tr("Insert Image"), QString(),
                                            tr("Images (%1)").arg(patterns.join(QLatin1Char(' '))));
        if (path.isEmpty()) {
            return;
        }
    }
    core::Image image;
    try {
        image = images::load(path);
    }
    catch (const core::SketchError& e) {
        QMessageBox::critical(this, tr("Insert Image"), QString::fromStdString(e.what()));
        return;
    }
    // In the middle of the view, filling most of it (Calibrate sets its true size).
    const core::Vec2 a = m_view->toWorld(QPointF(0, 0));
    const core::Vec2 b = m_view->toWorld(QPointF(m_view->width(), m_view->height()));
    const double viewW = std::abs(b.x - a.x), viewH = std::abs(b.y - a.y);
    image.x = (a.x + b.x) / 2;
    image.y = (a.y + b.y) / 2;
    image.width = 0.7 * std::min(viewW, viewH * image.pixelWidth / image.pixelHeight);
    core::Id id = core::kNoId;
    if (m_view->tryEdit(tr("Insert image"), [&](Editor& ed) { id = ed.addImage(image); })) {
        m_view->editImage(id);
    }
}

void MainWindow::pageSetup()
{
    PageSetupDialog dialog(m_doc->sketch().page(), this);
    if (dialog.exec() != QDialog::Accepted) {
        return;
    }
    const std::optional<core::Page> page = dialog.page();
    if (page == m_doc->sketch().page()) {
        return;
    }
    m_view->tryEdit(tr("Page setup"), [&](Editor& ed) { ed.setPage(page); });
    m_view->fitAll();
}

void MainWindow::exportSvg()
{
    QDialog dialog(this);
    dialog.setObjectName(QStringLiteral("exportSvgDialog"));
    dialog.setWindowTitle(tr("Export SVG"));
    auto* form = new QFormLayout(&dialog);
    auto* note = new QLabel(tr("The drawing as presentation view shows it, in millimetres."), &dialog);
    note->setWordWrap(true);
    form->addRow(note);
    auto* fills = new QCheckBox(tr("Fills"), &dialog);
    fills->setChecked(m_svgOptions.fills);
    auto* plain = new QCheckBox(tr("Curves without a stroke (thin black)"), &dialog);
    plain->setChecked(m_svgOptions.undecorated);
    auto* construction = new QCheckBox(tr("Construction geometry (grey, dashed)"), &dialog);
    construction->setChecked(m_svgOptions.construction);
    auto* margin = new ExpressionSpinBox(&dialog);
    margin->setRange(0.0, 1000.0);
    margin->setDecimals(2);
    margin->setSuffix(QStringLiteral(" mm"));
    margin->setValue(m_svgOptions.margin);
    auto* onPage = new QCheckBox(&dialog);
    onPage->setObjectName(QStringLiteral("svgPage"));
    if (const auto& page = m_doc->sketch().page()) {
        onPage->setText(tr("The page (%1 × %2 mm): what's off it is cut off").arg(page->width).arg(page->height));
        onPage->setChecked(m_svgOptions.page);
    }
    else {
        onPage->setText(tr("The page (no page set: File > Page Setup)"));
        onPage->setEnabled(false);
    }
    auto* outlines = new QCheckBox(tr("Text as outlines (exact shapes, but no longer editable text)"), &dialog);
    outlines->setObjectName(QStringLiteral("svgTextOutlines"));
    outlines->setChecked(m_svgOptions.textAsOutlines);
    auto* hatchLines = new QCheckBox(tr("Hatches as lines (for plotters and cutters)"), &dialog);
    hatchLines->setObjectName(QStringLiteral("svgHatchLines"));
    hatchLines->setChecked(m_svgOptions.hatchLines);
    auto* dimensions = new QCheckBox(tr("Dimensions and notes (from Annotate)"), &dialog);
    dimensions->setObjectName(QStringLiteral("svgDimensions"));
    dimensions->setChecked(m_svgOptions.dimensions);
    form->addRow(fills);
    form->addRow(plain);
    form->addRow(construction);
    form->addRow(outlines);
    form->addRow(hatchLines);
    form->addRow(dimensions);
    form->addRow(onPage);
    form->addRow(tr("Margin"), margin);
    auto marginUsed = [onPage, margin] { margin->setEnabled(!(onPage->isEnabled() && onPage->isChecked())); };
    connect(onPage, &QCheckBox::toggled, &dialog, marginUsed);
    marginUsed();
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dialog);
    buttons->button(QDialogButtonBox::Ok)->setText(tr("Export..."));
    form->addRow(buttons);
    connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    if (dialog.exec() != QDialog::Accepted) {
        return;
    }
    m_svgOptions.fills = fills->isChecked();
    m_svgOptions.undecorated = plain->isChecked();
    m_svgOptions.construction = construction->isChecked();
    m_svgOptions.margin = margin->value();
    m_svgOptions.textAsOutlines = outlines->isChecked();
    m_svgOptions.hatchLines = hatchLines->isChecked();
    m_svgOptions.dimensions = dimensions->isChecked();
    if (onPage->isEnabled()) {
        m_svgOptions.page = onPage->isChecked();
    }

    const QString path = exportPath(QStringLiteral("svg"), tr("Export SVG"), tr("SVG drawings (*.svg)"));
    if (path.isEmpty()) {
        return;
    }
    QString error;
    if (!m_doc->exportSvg(path, m_svgOptions, &error)) {
        QMessageBox::critical(this, tr("Export failed"), error);
        return;
    }
    statusBar()->showMessage(tr("Exported %1").arg(QDir::toNativeSeparators(path)), 4000);
}

void MainWindow::exportDxf()
{
    QDialog dialog(this);
    dialog.setObjectName(QStringLiteral("exportDxfDialog"));
    dialog.setWindowTitle(tr("Export DXF"));
    auto* form = new QFormLayout(&dialog);
    auto* note = new QLabel(tr("The exact geometry in millimetres (DXF 2004), for CAD and CAM: lines, arcs, "
                               "ellipses and splines as themselves, text in its font, fills as hatches."),
                            &dialog);
    note->setWordWrap(true);
    form->addRow(note);
    auto* styles = new QCheckBox(tr("Stroke colours, line styles and weights"), &dialog);
    styles->setChecked(m_dxfOptions.styles);
    auto* fills = new QCheckBox(tr("Fills (solid and hatched)"), &dialog);
    fills->setObjectName(QStringLiteral("dxfFills"));
    fills->setChecked(m_dxfOptions.fills);
    auto* construction = new QCheckBox(tr("Construction geometry (on its own layer, switched off)"), &dialog);
    construction->setChecked(m_dxfOptions.construction);
    auto* outlines = new QCheckBox(tr("Text as outlines (exact shapes, but no longer editable text)"), &dialog);
    outlines->setObjectName(QStringLiteral("dxfTextOutlines"));
    outlines->setChecked(m_dxfOptions.textAsOutlines);
    auto* dimensions = new QCheckBox(tr("Dimensions and notes (drawn out as lines and text, on their own layer)"), &dialog);
    dimensions->setObjectName(QStringLiteral("dxfDimensions"));
    dimensions->setChecked(m_dxfOptions.dimensions);
    form->addRow(styles);
    form->addRow(fills);
    form->addRow(construction);
    form->addRow(outlines);
    form->addRow(dimensions);
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dialog);
    buttons->button(QDialogButtonBox::Ok)->setText(tr("Export..."));
    form->addRow(buttons);
    connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    if (dialog.exec() != QDialog::Accepted) {
        return;
    }
    m_dxfOptions.styles = styles->isChecked();
    m_dxfOptions.fills = fills->isChecked();
    m_dxfOptions.construction = construction->isChecked();
    m_dxfOptions.textAsOutlines = outlines->isChecked();
    m_dxfOptions.dimensions = dimensions->isChecked();

    const QString path = exportPath(QStringLiteral("dxf"), tr("Export DXF"), tr("DXF drawings (*.dxf)"));
    if (path.isEmpty()) {
        return;
    }
    QString error;
    if (!m_doc->exportDxf(path, m_dxfOptions, &error)) {
        QMessageBox::critical(this, tr("Export failed"), error);
        return;
    }
    statusBar()->showMessage(tr("Exported %1").arg(QDir::toNativeSeparators(path)), 4000);
}

void MainWindow::exportPdf()
{
    QDialog dialog(this);
    dialog.setObjectName(QStringLiteral("exportPdfDialog"));
    dialog.setWindowTitle(tr("Export PDF"));
    auto* form = new QFormLayout(&dialog);
    QString what;
    if (const auto& page = m_doc->sketch().page()) {
        what = tr("The page (%1 × %2 mm) as presentation view shows it, at true size: what's off it is cut off.")
                   .arg(page->width)
                   .arg(page->height);
    }
    else {
        what = tr("No page is set (File > Page Setup), so the PDF's page is the drawing's size, with the margin "
                  "round it. At true size.");
    }
    auto* note = new QLabel(what, &dialog);
    note->setWordWrap(true);
    form->addRow(note);
    auto* fills = new QCheckBox(tr("Fills"), &dialog);
    fills->setChecked(m_pdfOptions.fills);
    auto* plain = new QCheckBox(tr("Curves without a stroke (thin black)"), &dialog);
    plain->setChecked(m_pdfOptions.undecorated);
    auto* construction = new QCheckBox(tr("Construction geometry (grey, dashed)"), &dialog);
    construction->setChecked(m_pdfOptions.construction);
    auto* outlines = new QCheckBox(tr("Text as outlines (exact shapes, but no longer selectable text)"), &dialog);
    outlines->setObjectName(QStringLiteral("pdfTextOutlines"));
    outlines->setChecked(m_pdfOptions.textAsOutlines);
    auto* dimensions = new QCheckBox(tr("Dimensions and notes (from Annotate)"), &dialog);
    dimensions->setObjectName(QStringLiteral("pdfDimensions"));
    dimensions->setChecked(m_pdfOptions.dimensions);
    auto* margin = new ExpressionSpinBox(&dialog);
    margin->setObjectName(QStringLiteral("pdfMargin"));
    margin->setRange(0.0, 1000.0);
    margin->setDecimals(2);
    margin->setSuffix(QStringLiteral(" mm"));
    margin->setValue(m_pdfOptions.margin);
    form->addRow(fills);
    form->addRow(plain);
    form->addRow(construction);
    form->addRow(outlines);
    form->addRow(dimensions);
    if (!m_doc->sketch().page()) {
        form->addRow(tr("Margin"), margin);
    }
    else {
        margin->hide();  // (the page is the page)
    }
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dialog);
    buttons->button(QDialogButtonBox::Ok)->setText(tr("Export..."));
    form->addRow(buttons);
    connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    if (dialog.exec() != QDialog::Accepted) {
        return;
    }
    m_pdfOptions.fills = fills->isChecked();
    m_pdfOptions.undecorated = plain->isChecked();
    m_pdfOptions.construction = construction->isChecked();
    m_pdfOptions.textAsOutlines = outlines->isChecked();
    m_pdfOptions.dimensions = dimensions->isChecked();
    m_pdfOptions.margin = margin->value();
    m_pdfOptions.page = true;  // (the page whenever there is one)

    const QString path = exportPath(QStringLiteral("pdf"), tr("Export PDF"), tr("PDF documents (*.pdf)"));
    if (path.isEmpty()) {
        return;
    }
    QString error;
    if (!m_doc->exportPdf(path, m_pdfOptions, &error)) {
        QMessageBox::critical(this, tr("Export failed"), error);
        return;
    }
    statusBar()->showMessage(tr("Exported %1").arg(QDir::toNativeSeparators(path)), 4000);
}

void MainWindow::exportImage(bool jpeg)
{
    const QString kind = jpeg ? tr("JPG") : tr("PNG");
    QDialog dialog(this);
    dialog.setObjectName(QStringLiteral("exportImageDialog"));
    dialog.setWindowTitle(tr("Export %1").arg(kind));
    auto* form = new QFormLayout(&dialog);
    QString what;
    if (const auto& page = m_doc->sketch().page()) {
        what = tr("The page (%1 × %2 mm) as presentation view shows it: what's off it is cut off.")
                   .arg(page->width)
                   .arg(page->height);
    }
    else {
        what = tr("No page is set (File > Page Setup), so the picture is the drawing, with the margin round it.");
    }
    auto* note = new QLabel(what, &dialog);
    note->setWordWrap(true);
    form->addRow(note);
    auto* dpi = new ExpressionSpinBox(&dialog);
    dpi->setObjectName(QStringLiteral("imageDpi"));
    dpi->setRange(10.0, 10000.0);
    dpi->setDecimals(0);
    dpi->setSuffix(tr(" dpi"));
    dpi->setValue(m_imageOptions.dpi);
    auto* pixels = new QLabel(&dialog);
    pixels->setObjectName(QStringLiteral("imagePixels"));
    auto* transparent = new QCheckBox(tr("Transparent background (where there's no page colour)"), &dialog);
    transparent->setObjectName(QStringLiteral("imageTransparent"));
    transparent->setChecked(m_imageOptions.transparent);
    auto* quality = new QSpinBox(&dialog);
    quality->setObjectName(QStringLiteral("imageQuality"));
    quality->setRange(1, 100);
    quality->setValue(m_imageOptions.quality);
    auto* fills = new QCheckBox(tr("Fills"), &dialog);
    fills->setChecked(m_imageSvgOptions.fills);
    auto* plain = new QCheckBox(tr("Curves without a stroke (thin black)"), &dialog);
    plain->setChecked(m_imageSvgOptions.undecorated);
    auto* construction = new QCheckBox(tr("Construction geometry (grey, dashed)"), &dialog);
    construction->setChecked(m_imageSvgOptions.construction);
    auto* dimensions = new QCheckBox(tr("Dimensions and notes (from Annotate)"), &dialog);
    dimensions->setChecked(m_imageSvgOptions.dimensions);
    auto* margin = new ExpressionSpinBox(&dialog);
    margin->setRange(0.0, 1000.0);
    margin->setDecimals(2);
    margin->setSuffix(QStringLiteral(" mm"));
    margin->setValue(m_imageSvgOptions.margin);
    form->addRow(tr("Resolution"), dpi);
    form->addRow(tr("Size"), pixels);
    if (jpeg) {
        form->addRow(tr("Quality"), quality);
        transparent->hide();
    }
    else {
        form->addRow(transparent);
        quality->hide();
    }
    form->addRow(fills);
    form->addRow(plain);
    form->addRow(construction);
    form->addRow(dimensions);
    if (!m_doc->sketch().page()) {
        form->addRow(tr("Margin"), margin);
    }
    else {
        margin->hide();  // (the page is the page)
    }
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dialog);
    buttons->button(QDialogButtonBox::Ok)->setText(tr("Export..."));
    form->addRow(buttons);
    connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    auto options = [&] {
        core::exporter::SvgOptions o = m_imageSvgOptions;
        o.fills = fills->isChecked();
        o.undecorated = plain->isChecked();
        o.construction = construction->isChecked();
        o.dimensions = dimensions->isChecked();
        o.margin = margin->value();
        o.page = true;  // (the page whenever there is one)
        return o;
    };
    // The size it'll be, as the options change (too big: can't be exported).
    auto showSize = [&] {
        const QSize size = m_doc->exportImageSize(options(), dpi->value());
        const bool tooBig = size.width() > Document::kMaxImageSide || size.height() > Document::kMaxImageSide
                         || qint64(size.width()) * size.height() > Document::kMaxImagePixels;
        pixels->setText(size.isEmpty() ? tr("(nothing to export)")
                        : tooBig       ? tr("%1 × %2 pixels: too big, try a lower resolution").arg(size.width()).arg(size.height())
                                       : tr("%1 × %2 pixels").arg(size.width()).arg(size.height()));
        buttons->button(QDialogButtonBox::Ok)->setEnabled(!size.isEmpty() && !tooBig);
    };
    connect(dpi, &QDoubleSpinBox::valueChanged, &dialog, showSize);
    connect(margin, &QDoubleSpinBox::valueChanged, &dialog, showSize);
    for (QCheckBox* box : {fills, plain, construction, dimensions}) {
        connect(box, &QCheckBox::toggled, &dialog, showSize);
    }
    showSize();
    if (dialog.exec() != QDialog::Accepted) {
        return;
    }
    m_imageSvgOptions = options();
    m_imageOptions.dpi = dpi->value();
    m_imageOptions.transparent = transparent->isChecked();
    m_imageOptions.quality = quality->value();

    const QString path = jpeg ? exportPath(QStringLiteral("jpg"), tr("Export JPG"), tr("JPEG images (*.jpg *.jpeg)"))
                              : exportPath(QStringLiteral("png"), tr("Export PNG"), tr("PNG images (*.png)"));
    if (path.isEmpty()) {
        return;
    }
    QString error;
    QApplication::setOverrideCursor(Qt::WaitCursor);  // (a big one takes a moment)
    const bool done = m_doc->exportImage(path, m_imageSvgOptions, m_imageOptions, &error);
    QApplication::restoreOverrideCursor();
    if (!done) {
        QMessageBox::critical(this, tr("Export failed"), error);
        return;
    }
    statusBar()->showMessage(tr("Exported %1").arg(QDir::toNativeSeparators(path)), 4000);
}

QString MainWindow::exportPath(const QString& suffix, const QString& title, const QString& filter)
{
    const QFileInfo current(m_doc->filePath());
    const QString start = m_doc->filePath().isEmpty()
        ? QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation) + QStringLiteral("/drawing.") + suffix
        : current.absolutePath() + QLatin1Char('/') + current.completeBaseName() + QLatin1Char('.') + suffix;
    QString path = QFileDialog::getSaveFileName(this, title, start, filter);
    if (!path.isEmpty() && QFileInfo(path).suffix().isEmpty()) {
        path += QLatin1Char('.') + suffix;
    }
    return path;
}

void MainWindow::showHistory()
{
    QDialog dialog(this);
    dialog.setObjectName(QStringLiteral("historyDialog"));
    dialog.setWindowTitle(tr("Command History"));
    dialog.resize(720, 480);
    auto* layout = new QVBoxLayout(&dialog);
    auto* note = new QLabel(tr("The commands that built this drawing, oldest first. It's saved with the file, and "
                               "running it in a new sketch rebuilds the drawing."),
                            &dialog);
    note->setWordWrap(true);
    layout->addWidget(note);
    if (m_doc->historyFromOlderVersion()) {
        auto* older = new QLabel(tr("⚠ This history was recorded by an older version of Setsquare. Commands refer to "
                                    "what earlier ones made by number, and some commands now make different things, "
                                    "so replaying it may not rebuild the drawing exactly."),
                                 &dialog);
        older->setObjectName(QStringLiteral("historyOlder"));
        older->setWordWrap(true);
        older->setStyleSheet(QStringLiteral("color: #f2b33d;"));
        layout->addWidget(older);
    }
    auto* text = new QPlainTextEdit(&dialog);
    text->setObjectName(QStringLiteral("historyText"));
    text->setReadOnly(true);
    text->setLineWrapMode(QPlainTextEdit::NoWrap);
    QFont mono(QStringLiteral("Consolas"));
    mono.setStyleHint(QFont::Monospace);
    text->setFont(mono);
    const QString history = m_doc->history().join(QLatin1Char('\n'));
    text->setPlainText(history.isEmpty() ? tr("# (nothing yet)") : history);
    layout->addWidget(text);

    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Close, &dialog);
    auto* copy = buttons->addButton(tr("Copy"), QDialogButtonBox::ActionRole);
    auto* saveMacro = buttons->addButton(tr("Save as Macro..."), QDialogButtonBox::ActionRole);
    connect(copy, &QPushButton::clicked, &dialog, [history] { QGuiApplication::clipboard()->setText(history); });
    connect(saveMacro, &QPushButton::clicked, &dialog, [this, &dialog, history] {
        const QString path = QFileDialog::getSaveFileName(&dialog, tr("Save History as Macro"),
                                                          macrosFolder() + QStringLiteral("/history.py"),
                                                          tr("Python (*.py)"));
        if (path.isEmpty()) {
            return;
        }
        QFile file(path);
        if (!file.open(QIODevice::WriteOnly | QIODevice::Text)) {
            QMessageBox::critical(&dialog, tr("Save failed"), file.errorString());
            return;
        }
        file.write((QStringLiteral("# Command history from Setsquare. Run in a new sketch to rebuild it.\n") + history
                    + QLatin1Char('\n'))
                       .toUtf8());
    });
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    layout->addWidget(buttons);
    dialog.exec();
}

void MainWindow::copySelection()
{
    std::vector<core::Id> items;
    for (core::Id id : m_view->selection()) {
        if (m_doc->sketch().hasEntity(id) && !m_doc->sketch().isReference(id)) {
            items.push_back(id);
        }
    }
    if (items.empty()) {
        statusBar()->showMessage(tr("Nothing selected to copy"), 3000);
        return;
    }
    const QByteArray json =
        QByteArray::fromStdString(core::toJson(core::xform::extract(m_doc->sketch(), items), true));
    auto* mime = new QMimeData;
    mime->setData(kSketchMime, json);
    mime->setText(QString::fromUtf8(json));  // pasteable into scripts: s.paste(text, at=...)
    QGuiApplication::clipboard()->setMimeData(mime);
    statusBar()->showMessage(tr("Copied %n item(s)", nullptr, static_cast<int>(items.size())), 3000);
}

void MainWindow::pasteClipboard()
{
    const QMimeData* mime = QGuiApplication::clipboard()->mimeData();
    QByteArray json;
    if (mime && mime->hasFormat(kSketchMime)) {
        json = mime->data(kSketchMime);
    }
    else if (mime && mime->hasText()) {
        json = mime->text().toUtf8();  // e.g. copied from s.extract()
    }
    core::Sketch fragment;
    try {
        fragment = core::fromJson(json.toStdString());
    }
    catch (const core::SketchError&) {
        statusBar()->showMessage(tr("The clipboard has no sketch geometry"), 3000);
        return;
    }
    if (fragment.entities().empty()) {
        return;
    }
    m_view->setPasteFragment(std::move(fragment));
    m_view->setTool(ToolKind::Paste);
    m_view->setFocus();
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
    m_recentFilesMenu = fileMenu->addMenu(tr("Open &Recent"));
    m_recentFilesMenu->setObjectName(QStringLiteral("recentFilesMenu"));
    m_recentFilesMenu->setToolTipsVisible(true);
    connect(m_recentFilesMenu, &QMenu::aboutToShow, this, &MainWindow::fillRecentMenu);
    fillRecentMenu();
    auto* save = fileMenu->addAction(tr("&Save"));
    save->setShortcut(QKeySequence::Save);
    connect(save, &QAction::triggered, this, &MainWindow::save);
    auto* saveAs = fileMenu->addAction(tr("Save &As..."));
    saveAs->setShortcut(QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_S));
    connect(saveAs, &QAction::triggered, this, &MainWindow::saveAs);
    fileMenu->addSeparator();
    auto* importAction = fileMenu->addAction(tr("I&mport..."));
    importAction->setObjectName(QStringLiteral("importAction"));
    importAction->setShortcut(QKeySequence(Qt::CTRL | Qt::Key_I));
    importAction->setToolTip(tr("Bring in an SVG or DXF drawing: its curves, strokes and fills (each of its layers held "
                                "together as a group)"));
    connect(importAction, &QAction::triggered, this, [this] { importDrawing(); });
    m_insertImage = fileMenu->addAction(tr("&Insert Image..."));
    m_insertImage->setObjectName(QStringLiteral("insertImageAction"));
    m_insertImage->setIcon(icons::forName(QStringLiteral("image")));
    m_insertImage->setToolTip(tr("Insert Image: a picture to draw over, as a reference (scale it with Calibrate; "
                                 "it isn't part of the drawing). Double-click it later to place it"));
    connect(m_insertImage, &QAction::triggered, this, [this] { insertImage(); });
    auto* pageSetupAction = fileMenu->addAction(tr("Pa&ge Setup..."));
    pageSetupAction->setObjectName(QStringLiteral("pageSetupAction"));
    pageSetupAction->setToolTip(tr("The page the drawing is on: its size, orientation and colour (or none)"));
    connect(pageSetupAction, &QAction::triggered, this, &MainWindow::pageSetup);
    QMenu* exportMenu = fileMenu->addMenu(tr("&Export"));
    auto* exportSvgAction = exportMenu->addAction(tr("&SVG..."));
    exportSvgAction->setToolTip(tr("The drawing as it looks in presentation view, at true size"));
    connect(exportSvgAction, &QAction::triggered, this, &MainWindow::exportSvg);
    auto* exportDxfAction = exportMenu->addAction(tr("&DXF..."));
    exportDxfAction->setToolTip(tr("Exact geometry for CAD and CAM, in millimetres"));
    connect(exportDxfAction, &QAction::triggered, this, &MainWindow::exportDxf);
    auto* exportPdfAction = exportMenu->addAction(tr("&PDF..."));
    exportPdfAction->setObjectName(QStringLiteral("exportPdfAction"));
    exportPdfAction->setToolTip(tr("The page as presentation view shows it, at true size, to print or share"));
    connect(exportPdfAction, &QAction::triggered, this, &MainWindow::exportPdf);
    auto* exportPngAction = exportMenu->addAction(tr("P&NG..."));
    exportPngAction->setObjectName(QStringLiteral("exportPngAction"));
    exportPngAction->setToolTip(tr("A picture of the page as presentation view shows it, at the resolution you choose"));
    connect(exportPngAction, &QAction::triggered, this, [this] { exportImage(false); });
    auto* exportJpgAction = exportMenu->addAction(tr("&JPG..."));
    exportJpgAction->setObjectName(QStringLiteral("exportJpgAction"));
    exportJpgAction->setToolTip(tr("A picture of the page as presentation view shows it, at the resolution you choose"));
    connect(exportJpgAction, &QAction::triggered, this, [this] { exportImage(true); });
    if (fileassoc::supported()) {
        // Double-clicking a .setsq file opens it here (see FileAssociation.h).
        auto* associate = fileMenu->addAction(tr("Open .setsq Files with Setsquare"));
        associate->setObjectName(QStringLiteral("fileAssociationAction"));
        associate->setCheckable(true);
        associate->setToolTip(tr("Double-clicking a .setsq drawing opens it in this copy of Setsquare"));
        connect(fileMenu, &QMenu::aboutToShow, associate, [associate] {
            const QSignalBlocker quiet(associate);
            associate->setChecked(fileassoc::isRegistered());
        });
        connect(associate, &QAction::toggled, this, [this](bool on) {
            QSettings().setValue(QStringLiteral("fileAssociation"), on ? QStringLiteral("yes") : QStringLiteral("no"));
            if (!on) {
                fileassoc::unregisterFiles();
                statusBar()->showMessage(tr(".setsq files no longer open with Setsquare"), 4000);
            }
            else if (fileassoc::registerFiles()) {
                statusBar()->showMessage(tr(".setsq files now open with Setsquare"), 4000);
            }
            else {
                QMessageBox::warning(this, tr("Open .setsq Files"), tr("Windows didn't let Setsquare register .setsq files."));
            }
        });
    }
    auto* historyAction = fileMenu->addAction(tr("Command &History..."));
    historyAction->setToolTip(tr("The commands that built this drawing (saved with it)"));
    connect(historyAction, &QAction::triggered, this, &MainWindow::showHistory);
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
    editMenu->addSeparator();
    // (Text fields such as the Python console keep these keys for their own text.)
    auto* cut = editMenu->addAction(tr("Cu&t"));
    cut->setShortcut(QKeySequence::Cut);
    connect(cut, &QAction::triggered, this, [this] {
        copySelection();
        m_view->deleteSelection();
    });
    auto* copy = editMenu->addAction(tr("&Copy"));
    copy->setShortcut(QKeySequence::Copy);
    connect(copy, &QAction::triggered, this, &MainWindow::copySelection);
    auto* paste = editMenu->addAction(tr("&Paste"));
    paste->setShortcut(QKeySequence::Paste);
    connect(paste, &QAction::triggered, this, &MainWindow::pasteClipboard);

    // --- Sketch tools ---
    auto* sketchMenu = menuBar()->addMenu(tr("&Sketch"));
    auto* toolbar = addToolBar(tr("Sketch"));
    toolbar->setObjectName("SketchToolbar");
    toolbar->setMovable(false);
    toolbar->setToolButtonStyle(Qt::ToolButtonIconOnly);  // names in the tooltips
    toolbar->setIconSize(QSize(26, 26));
    auto* group = new QActionGroup(this);

    struct ToolDef
    {
        ToolKind kind;
        QString text;
        QKeySequence key;
    };
    auto makeTool = [&](const ToolDef& t, QMenu* menu) {
        auto* action = new QAction(t.text, this);
        action->setCheckable(true);
        action->setToolTip(t.text);
        if (!t.key.isEmpty()) {
            action->setShortcut(t.key);
            action->setToolTip(QStringLiteral("%1 (%2)").arg(t.text, t.key.toString(QKeySequence::NativeText)));
        }
        if (const QIcon icon = icons::forName(toolName(t.kind)); !icon.isNull()) {
            action->setIcon(icon);
        }
        group->addAction(action);
        menu->addAction(action);
        const ToolKind kind = t.kind;
        connect(action, &QAction::triggered, this, [this, kind] {
            m_view->setTool(kind);
            m_view->setFocus();  // so typed input (e.g. polygon sides) reaches the canvas
        });
        m_toolActions[kind] = action;
        return action;
    };
    auto addTools = [&](QToolBar* bar, std::initializer_list<ToolDef> defs) {
        for (const ToolDef& t : defs) {
            bar->addAction(makeTool(t, sketchMenu));
        }
    };
    // A split button: one per kind of tool, showing its most used variant (the first), which a
    // click starts; the others are in the arrow menu. It shows as active while any of them is.
    auto addDropdown = [&](QToolBar* bar, const QString& title, std::initializer_list<ToolDef> defs) {
        QMenu* menu = sketchMenu->addMenu(title);
        auto* button = new QToolButton(bar);
        button->setObjectName(QStringLiteral("%1Button").arg(toolName(defs.begin()->kind)));
        button->setPopupMode(QToolButton::MenuButtonPopup);
        button->setToolButtonStyle(bar->toolButtonStyle());
        button->setIconSize(bar->iconSize());
        button->setCheckable(true);
        button->setMenu(menu);
        std::vector<QAction*> actions;
        for (const ToolDef& t : defs) {
            actions.push_back(makeTool(t, menu));
        }
        QAction* primary = actions.front();
        button->setText(title);
        button->setIcon(primary->icon());
        button->setToolTip(tr("%1 (more in the menu)").arg(primary->toolTip()));
        auto sync = [button, actions] {
            button->setChecked(std::ranges::any_of(actions, [](QAction* a) { return a->isChecked(); }));
        };
        for (QAction* action : actions) {
            connect(action, &QAction::toggled, button, sync);
        }
        connect(button, &QToolButton::clicked, button, [primary, sync] {
            primary->trigger();
            sync();  // a click toggles the button itself; put it back to what's active
        });
        bar->addWidget(button);
    };

    addTools(toolbar, {
                          {ToolKind::Select, tr("Select"), {}},  // Esc; S is the search box
                          {ToolKind::Line, tr("Line"), QKeySequence(Qt::Key_L)},
                      });
    addDropdown(toolbar, tr("Rectangle"),
                {
                    {ToolKind::Rectangle, tr("2-Point Rectangle"), QKeySequence(Qt::Key_R)},
                    {ToolKind::Rectangle3Point, tr("3-Point Rectangle"), QKeySequence(Qt::ALT | Qt::Key_R)},
                    {ToolKind::RectangleCenter, tr("Center Rectangle"), QKeySequence(Qt::SHIFT | Qt::Key_R)},
                });
    addDropdown(toolbar, tr("Circle"),
                {
                    {ToolKind::Circle, tr("Center Circle"), QKeySequence(Qt::Key_C)},
                    {ToolKind::Circle2Point, tr("2-Point Circle"), {}},
                    {ToolKind::Circle3Point, tr("3-Point Circle"), {}},
                    {ToolKind::CircleTangent2, tr("2-Tangent Circle"), {}},
                    {ToolKind::CircleTangent3, tr("3-Tangent Circle"), {}},
                });
    addDropdown(toolbar, tr("Arc"),
                {
                    {ToolKind::ArcCenter, tr("Center Point Arc"), QKeySequence(Qt::Key_A)},
                    {ToolKind::Arc3Point, tr("3-Point Arc"), QKeySequence(Qt::SHIFT | Qt::Key_A)},
                    {ToolKind::ArcTangent, tr("Tangent Arc"), {}},
                });
    addTools(toolbar, {{ToolKind::Ellipse, tr("Ellipse"), {}}});
    addDropdown(toolbar, tr("Spline"),
                {
                    {ToolKind::SplineFit, tr("Fit Point Spline"), {}},
                    {ToolKind::SplineControl, tr("Control Point Spline"), {}},
                });
    addDropdown(toolbar, tr("Polygon"),
                {
                    {ToolKind::PolygonCircumscribed, tr("Circumscribed Polygon"), {}},
                    {ToolKind::PolygonInscribed, tr("Inscribed Polygon"), {}},
                    {ToolKind::PolygonEdge, tr("Edge Polygon"), {}},
                });
    addDropdown(toolbar, tr("Slot"),
                {
                    {ToolKind::SlotCenterToCenter, tr("Center to Center Slot"), {}},
                    {ToolKind::SlotOverall, tr("Overall Slot"), {}},
                    {ToolKind::SlotCenterPoint, tr("Center Point Slot"), {}},
                    {ToolKind::SlotArc3Point, tr("Three Point Arc Slot"), {}},
                    {ToolKind::SlotArcCenter, tr("Center Point Arc Slot"), {}},
                });
    addTools(toolbar, {
                          {ToolKind::Point, tr("Point"), QKeySequence(Qt::Key_P)},
                          {ToolKind::Text, tr("Text"), {}},
                          {ToolKind::Project, tr("Project"), {}},
                      });
    toolbar->addAction(m_insertImage);
    // The modify tools follow on the same toolbar, after a group divider: a line with half a tool
    // button's space either side (stronger than the plain separators within a group).
    QToolBar* modifyBar = toolbar;
    {
        auto* divider = new GroupDivider(toolbar);
        divider->setObjectName(QStringLiteral("ModifyDivider"));
        divider->setFixedWidth(toolbar->widgetForAction(m_insertImage)->sizeHint().width());
        toolbar->addWidget(divider);
    }
    addTools(modifyBar, {{ToolKind::Fillet, tr("Fillet"), QKeySequence(Qt::Key_F)}});
    addDropdown(modifyBar, tr("Chamfer"),
                {
                    {ToolKind::ChamferEqual, tr("Equal Distance Chamfer"), {}},
                    {ToolKind::ChamferTwoDistances, tr("Distance-Distance Chamfer"), {}},
                    {ToolKind::ChamferDistanceAngle, tr("Distance-Angle Chamfer"), {}},
                });
    addDropdown(modifyBar, tr("Trim"),
                {
                    {ToolKind::Trim, tr("Trim"), QKeySequence(Qt::Key_T)},
                    {ToolKind::TrimToConstruction, tr("Trim to Construction"), QKeySequence(Qt::SHIFT | Qt::Key_T)},
                });
    addTools(modifyBar, {
                          {ToolKind::Extend, tr("Extend"), {}},
                          {ToolKind::Break, tr("Break"), {}},
                      });
    modifyBar->addSeparator();
    addDropdown(modifyBar, tr("Move"),
                {
                    {ToolKind::Move, tr("Move/Copy"), {}},  // (M is Measure)
                    {ToolKind::Scale, tr("Scale"), {}},
                });
    addTools(modifyBar, {{ToolKind::Mirror, tr("Mirror"), {}}});
    addDropdown(modifyBar, tr("Pattern"),
                {
                    {ToolKind::RectangularPattern, tr("Rectangular Pattern"), {}},
                    {ToolKind::CircularPattern, tr("Circular Pattern"), {}},
                });
    addTools(modifyBar, {
                            {ToolKind::Offset, tr("Offset"), {}},
                            {ToolKind::Divide, tr("Divide"), {}},
                        });
    // (Tool settings are in panels on the canvas, not up here.)

    // Appearance, on a row of its own: presentation view, the stroke brush and the colour bar.
    addToolBarBreak();
    auto* appearanceBar = addToolBar(tr("Appearance"));
    appearanceBar->setObjectName("AppearanceToolbar");
    appearanceBar->setMovable(false);
    appearanceBar->setToolButtonStyle(Qt::ToolButtonIconOnly);  // names in the tooltips
    appearanceBar->setIconSize(QSize(26, 26));
    auto* presentation = new QAction(tr("Presentation"), this);
    presentation->setObjectName(QStringLiteral("presentationAction"));
    presentation->setCheckable(true);
    presentation->setIcon(icons::forName(QStringLiteral("presentation")));
    presentation->setShortcut(Qt::Key_V);
    presentation->setToolTip(tr("Presentation view: the drawing as it will look (V)"));
    connect(presentation, &QAction::toggled, m_view, &SketchView::setPresentation);
    connect(m_view, &SketchView::presentationChanged, presentation, &QAction::setChecked);
    appearanceBar->addAction(presentation);
    QMenu* appearanceMenu = menuBar()->addMenu(tr("&Appearance"));
    appearanceMenu->addAction(presentation);
    addTools(appearanceBar, {
                                {ToolKind::StrokeBrush, tr("Stroke Brush"), QKeySequence(Qt::Key_B)},
                                {ToolKind::Fill, tr("Fill"), QKeySequence(Qt::Key_G)},
                                {ToolKind::Gradient, tr("Gradient"), QKeySequence(Qt::SHIFT | Qt::Key_G)},
                                {ToolKind::Hatch, tr("Hatch"), QKeySequence(Qt::SHIFT | Qt::Key_H)},
                                {ToolKind::ResetFill, tr("Reset Fill"), {}},
                                {ToolKind::Eyedropper, tr("Eyedropper"), QKeySequence(Qt::Key_I)},
                                {ToolKind::Annotate, tr("Annotate (dimensions and notes on the drawing)"),
                                 QKeySequence(Qt::SHIFT | Qt::Key_D)},
                            });
    auto* colorBar = new ColorBar(*m_view, appearanceBar);
    appearanceBar->addWidget(colorBar);
    auto* resetStroke = appearanceMenu->addAction(tr("&Reset Stroke"));
    resetStroke->setToolTip(tr("Take the strokes off the selected curves"));
    connect(resetStroke, &QAction::triggered, colorBar, &ColorBar::resetSelection);

    sketchMenu->addSeparator();
    addToolBarBreak();
    auto* constraintBar = addToolBar(tr("Constraints"));
    constraintBar->setObjectName("ConstraintToolbar");
    constraintBar->setMovable(false);
    constraintBar->setToolButtonStyle(Qt::ToolButtonIconOnly);  // names in the tooltips
    constraintBar->setIconSize(QSize(26, 26));
    addTools(constraintBar, {
                                {ToolKind::Dimension, tr("Dimension"), QKeySequence(Qt::Key_D)},
                                {ToolKind::Coincident, tr("Coincident"), QKeySequence(Qt::Key_O)},
                                {ToolKind::HorizontalVertical, tr("Horiz/Vert"), QKeySequence(Qt::Key_H)},
                                {ToolKind::Parallel, tr("Parallel"), {}},
                                {ToolKind::Perpendicular, tr("Perpendicular"), {}},
                                {ToolKind::Tangent, tr("Tangent"), {}},  // (T is Trim, as in Fusion)
                                {ToolKind::Equal, tr("Equal"), QKeySequence(Qt::Key_E)},
                                {ToolKind::Ratio, tr("Ratio"), {}},
                                {ToolKind::Midpoint, tr("Midpoint"), {}},
                                {ToolKind::Concentric, tr("Concentric"), {}},
                                {ToolKind::Symmetric, tr("Symmetric"), {}},
                                {ToolKind::Collinear, tr("Collinear"), {}},
                                {ToolKind::Fix, tr("Fix/Unfix"), QKeySequence(Qt::SHIFT | Qt::Key_F)},
                                {ToolKind::Group, tr("Group"), QKeySequence(Qt::CTRL | Qt::Key_G)},
                            });
    // Inspecting, after a group divider: Measure.
    {
        auto* divider = new GroupDivider(constraintBar);
        divider->setObjectName(QStringLiteral("MeasureDivider"));
        divider->setFixedWidth(toolbar->widgetForAction(m_insertImage)->sizeHint().width());
        constraintBar->addWidget(divider);
    }
    addTools(constraintBar, {{ToolKind::Measure, tr("Measure"), QKeySequence(Qt::Key_M)}});

    sketchMenu->addSeparator();
    auto* construction = sketchMenu->addAction(tr("Toggle &Construction"));
    construction->setShortcut(Qt::Key_X);
    connect(construction, &QAction::triggered, this, [this] {
        const auto selection = m_view->selection();
        m_view->tryEdit(tr("Construction"), [&](Editor& ed) {
            for (core::Id id : selection) {
                const core::Sketch& s = ed.sketch();
                if (s.hasEntity(id) && s.entity(id).kind() != core::EntityKind::Point && !s.isReference(id)) {
                    ed.setConstruction(id, !s.entity(id).construction);
                }
            }
        });
    });
    auto* breakLink = sketchMenu->addAction(tr("&Break Link"));
    breakLink->setToolTip(tr("Make the selected projections ordinary geometry (no longer following their originals)"));
    connect(breakLink, &QAction::triggered, this, [this] {
        std::vector<core::Id> projections;
        for (core::Id id : m_view->selection()) {
            if (m_doc->sketch().isProjection(id)) {
                projections.push_back(id);
            }
        }
        if (projections.empty()) {
            m_view->message(tr("Select projected geometry (from Project) first"));
            return;
        }
        m_view->tryEdit(tr("Break link"), [&](Editor& ed) { ed.breakLink(projections); });
    });
    m_toolActions[ToolKind::Select]->setChecked(true);
    // The active tool (and other modes that are on, like presentation view) shows green.
    setStyleSheet(QStringLiteral("QToolBar QToolButton:checked { background-color: #2e7d32; color: #ffffff;"
                                 " border: 1px solid #4caf50; border-radius: 3px; }"
                                 " QToolBar::separator { background-color: %1; width: 1px; margin: 4px 3px; }")
                      .arg(separatorColor(palette()).name()));
    connect(m_view, &SketchView::toolChanged, this, [this](ToolKind kind) {
        if (auto it = m_toolActions.find(kind); it != m_toolActions.end()) {
            it->second->setChecked(true);
        }
        // Most recently used first, no repeats; Select doesn't count.
        if (kind != ToolKind::Select) {
            std::erase(m_recentTools, kind);
            m_recentTools.insert(m_recentTools.begin(), kind);
            if (m_recentTools.size() > kRecentTools) {
                m_recentTools.resize(kRecentTools);
            }
        }
    });

    // Right-click on the canvas: the last few tools, at the cursor.
    m_recentMenu = new QMenu(this);
    m_recentMenu->setObjectName(QStringLiteral("recentToolsMenu"));
    connect(m_view, &SketchView::recentToolsRequested, this, [this](QPoint at) {
        m_recentMenu->clear();
        for (ToolKind kind : m_recentTools) {
            if (auto it = m_toolActions.find(kind); it != m_toolActions.end()) {
                QAction* tool = it->second;
                QAction* item = m_recentMenu->addAction(tool->text());
                item->setShortcut(tool->shortcut());  // shown for reference
                connect(item, &QAction::triggered, tool, &QAction::trigger);
            }
        }
        if (m_recentTools.empty()) {
            m_recentMenu->addAction(tr("(no recent tools yet)"))->setEnabled(false);
        }
        m_recentMenu->addSeparator();
        connect(m_recentMenu->addAction(tr("Select")), &QAction::triggered, m_toolActions[ToolKind::Select],
                &QAction::trigger);
        m_recentMenu->popup(at);
    });

    // --- Docks ---
    auto* constraintsDock = new QDockWidget(tr("Constraints"), this);
    constraintsDock->setObjectName("ConstraintsDock");
    constraintsDock->setWidget(new ConstraintPanel(*m_doc, *m_view, constraintsDock));
    addDockWidget(Qt::RightDockWidgetArea, constraintsDock);
    resizeDocks({constraintsDock}, {300}, Qt::Horizontal);
    // The parameters (named values for dimensions), under the constraints.
    auto* parametersDock = new QDockWidget(tr("Parameters"), this);
    parametersDock->setObjectName("ParametersDock");
    parametersDock->setWidget(new ParametersPanel(*m_doc, parametersDock));
    addDockWidget(Qt::RightDockWidgetArea, parametersDock);
    splitDockWidget(constraintsDock, parametersDock, Qt::Vertical);
    resizeDocks({constraintsDock, parametersDock}, {3, 1}, Qt::Vertical);

    auto* browserDock = new QDockWidget(tr("Browser"), this);
    browserDock->setObjectName("BrowserDock");
    browserDock->setWidget(new BrowserPanel(*m_doc, *m_view, browserDock));
    addDockWidget(Qt::LeftDockWidgetArea, browserDock);
    resizeDocks({browserDock}, {220}, Qt::Horizontal);

    m_console = new ScriptConsole(*m_doc, this);
    m_consoleDock = new QDockWidget(tr("Python"), this);
    m_consoleDock->setObjectName("PythonDock");
    m_consoleDock->setWidget(m_console);
    addDockWidget(Qt::BottomDockWidgetArea, m_consoleDock);
    resizeDocks({m_consoleDock}, {180}, Qt::Vertical);
    m_consoleDock->hide();  // out of the way until wanted: View > Python, or running a script

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
        const QString body = QStringLiteral("# Recorded with Setsquare. `doc` and `s` (the sketch) are predefined.\n")
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
    viewMenu->addAction(browserDock->toggleViewAction());
    viewMenu->addAction(constraintsDock->toggleViewAction());
    viewMenu->addAction(parametersDock->toggleViewAction());
    viewMenu->addAction(m_consoleDock->toggleViewAction());
    auto* home = viewMenu->addAction(tr("&Fit All"));
    home->setShortcut(Qt::Key_Home);
    connect(home, &QAction::triggered, m_view, &SketchView::fitAll);
    auto* showConstraints = viewMenu->addAction(tr("Show &Constraints"));
    showConstraints->setCheckable(true);
    showConstraints->setChecked(true);
    showConstraints->setShortcut(QKeySequence(Qt::CTRL | Qt::Key_K));
    connect(showConstraints, &QAction::toggled, m_view, &SketchView::setShowConstraints);
    auto* stats = viewMenu->addAction(tr("Performance &Stats"));
    stats->setCheckable(true);
    stats->setShortcut(Qt::Key_F12);
    connect(stats, &QAction::toggled, m_view, &SketchView::setShowStats);

    // S: find any tool or command by name, at the cursor (as in Fusion).
    m_search = new ToolSearch(this);
    auto* search = new QAction(tr("&Search Tools..."), this);
    search->setShortcut(Qt::Key_S);
    sketchMenu->insertAction(sketchMenu->actions().value(0), search);
    sketchMenu->insertSeparator(sketchMenu->actions().value(1));
    connect(search, &QAction::triggered, this, [this, search] {
        std::vector<ToolSearch::Entry> entries;
        std::function<void(QMenu*, const QString&)> collect = [&](QMenu* menu, const QString& path) {
            for (QAction* a : menu->actions()) {
                const QString name = QString(a->text()).remove(QLatin1Char('&')).remove(QStringLiteral("..."));
                if (a->menu()) {
                    collect(a->menu(), path + QStringLiteral(" ▸ ") + name);
                }
                else if (!a->isSeparator() && a != search && !name.isEmpty()) {
                    entries.push_back({a, name, path});
                }
            }
        };
        for (QAction* top : menuBar()->actions()) {
            if (top->menu()) {
                collect(top->menu(), QString(top->text()).remove(QLatin1Char('&')));
            }
        }
        m_search->setEntries(std::move(entries));
        const QPoint cursor = QCursor::pos();
        m_search->popup(rect().contains(mapFromGlobal(cursor)) ? cursor : m_view->mapToGlobal(m_view->rect().center()));
    });
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
