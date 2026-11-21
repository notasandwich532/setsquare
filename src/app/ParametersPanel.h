#pragma once

#include <core/Sketch.h>

#include <QWidget>

#include <vector>

class Document;
class QLabel;
class QPushButton;
class QTableWidget;

// The drawing's parameters (see core/Parameters.h): a table of name, expression and what it comes
// to. Edit a name to rename it (and everywhere it's used), an expression to change it (everything
// using it follows); Add and Remove. What can't be done is said under the table, and the cell goes
// back. A dimension uses one by typing it into the dimension ("width / 2").
class ParametersPanel : public QWidget
{
    Q_OBJECT

public:
    ParametersPanel(Document& doc, QWidget* parent = nullptr);

private:
    void rebuild();
    void cellEdited(int row, int column);
    void add();
    void removeSelected();
    void say(const QString& problem);  // (empty: nothing to say)

    Document& m_doc;
    QTableWidget* m_table;
    QPushButton* m_remove;
    QLabel* m_problem;
    bool m_filling = false;

    struct Shown
    {
        std::string name, expression;
        double value;
        std::vector<core::Id> users;  // (for the tooltips)
        bool operator==(const Shown&) const = default;
    };
    std::vector<Shown> m_shown;
};
