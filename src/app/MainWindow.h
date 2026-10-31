#pragma once

#include "tools/Tool.h"

#include <QMainWindow>

#include <map>

class Document;
class QAction;
class QLabel;
class SketchView;

class MainWindow : public QMainWindow
{
    Q_OBJECT

public:
    explicit MainWindow(QWidget* parent = nullptr);

    bool openFile(const QString& path);  // reports errors in a dialog

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

    Document* m_doc = nullptr;
    SketchView* m_view = nullptr;
    QLabel* m_hintLabel = nullptr;
    QLabel* m_solveLabel = nullptr;
    QLabel* m_cursorLabel = nullptr;
    std::map<ToolKind, QAction*> m_toolActions;
};
