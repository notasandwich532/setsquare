#pragma once

#include <QMainWindow>

class QLabel;
class SketchView;

class MainWindow : public QMainWindow
{
    Q_OBJECT

public:
    explicit MainWindow(QWidget* parent = nullptr);

private:
    void createActions();

    SketchView* m_view = nullptr;
    QLabel* m_cursorLabel = nullptr;
};
