#pragma once

#include "tools/Tool.h"

#include "Document.h"

#include <core/Export.h>

#include <QMainWindow>

#include <map>
#include <optional>
#include <vector>

class Document;
class QAction;
class QLabel;
class SketchView;

class MainWindow : public QMainWindow
{
    Q_OBJECT

public:
    explicit MainWindow(QWidget* parent = nullptr);
    ~MainWindow() override;

    bool openFile(const QString& path);  // reports errors in a dialog
    void runScript(const QString& path);  // in the Python console
    // File > Insert Image: a reference image in the middle of the view (empty path: ask which).
    void insertImage(const QString& path = {});
    // File > Import: an SVG or DXF drawing (empty path: ask which), with its options.
    void importDrawing(const QString& path = {});

    // Tool names as used by Python's app.tool.
    static QString toolName(ToolKind kind);
    static std::optional<ToolKind> toolFromName(const QString& name);

protected:
    void closeEvent(QCloseEvent* event) override;

private:
    void createActions();
    void updateSolveStatus();
    void updateTitle();

    // File actions; each returns false if the user cancelled or it failed.
    bool maybeSave();  // offers to save unsaved changes before they'd be lost
    bool save();
    bool saveAs();
    void open();
    void newSketch();

    // Clipboard: the selection (and the constraints among it) as a sketch fragment.
    void showHistory();  // the document's command history, to read, copy or save as a macro
    void pageSetup();    // File > Page Setup
    void exportSvg();    // options, then where to
    void exportDxf();
    void exportPdf();
    void exportImage(bool jpeg);  // PNG, or JPG
    // Where to export to (next to the sketch file, with this suffix); empty if cancelled.
    QString exportPath(const QString& suffix, const QString& title, const QString& filter);
    void copySelection();
    void pasteClipboard();  // starts the Paste tool with the clipboard's fragment

    Document* m_doc = nullptr;
    SketchView* m_view = nullptr;
    QLabel* m_hintLabel = nullptr;
    QLabel* m_solveLabel = nullptr;
    QLabel* m_cursorLabel = nullptr;
    std::map<ToolKind, QAction*> m_toolActions;
    static constexpr size_t kRecentTools = 5;
    std::vector<ToolKind> m_recentTools;  // most recent first
    class QMenu* m_recentMenu = nullptr;
    class ScriptConsole* m_console = nullptr;
    class QDockWidget* m_consoleDock = nullptr;
    class ToolSearch* m_search = nullptr;
    core::exporter::SvgOptions m_svgOptions;  // as last used
    core::exporter::DxfOptions m_dxfOptions;
    core::exporter::SvgOptions m_pdfOptions;  // (the PDF is drawn from the SVG export)
    core::exporter::SvgOptions m_imageSvgOptions;  // (so are PNGs and JPGs)
    Document::ImageOptions m_imageOptions;
    QAction* m_insertImage = nullptr;
};
