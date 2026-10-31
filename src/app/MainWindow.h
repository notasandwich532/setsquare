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

private:
    void createActions();
    void updateSolveStatus();

    Document* m_doc = nullptr;
    SketchView* m_view = nullptr;
    QLabel* m_hintLabel = nullptr;
    QLabel* m_solveLabel = nullptr;
    QLabel* m_cursorLabel = nullptr;
    std::map<ToolKind, QAction*> m_toolActions;
};
