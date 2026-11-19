#pragma once

#include <QFrame>
#include <QPushButton>
#include <QStringList>

class QCheckBox;
class QComboBox;
class QDoubleSpinBox;
class QFormLayout;
class QLabel;
class QLineEdit;
class QSpinBox;

// A selection input of a command panel: shows "3 selected" (or "Select"), highlighted while it's
// the input that clicks on the canvas go to. Click it to make it current; × clears it.
class SelectionButton : public QPushButton
{
    Q_OBJECT

public:
    explicit SelectionButton(QWidget* parent = nullptr);
    void setCount(int count);
    void setCurrent(bool current);
    bool isCurrent() const { return m_current; }

signals:
    void cleared();

private:
    bool m_current = false;
};

// A Fusion-style command dialog floating over the top-right of the canvas: a title, rows of
// inputs and (optionally) OK / Cancel. Enter in an input accepts and Esc cancels. Owned by
// whoever shows it; it follows the canvas when that resizes.
class CommandPanel : public QFrame
{
    Q_OBJECT

public:
    CommandPanel(const QString& title, QWidget* canvas);

    // Each input gets `name` as its object name (for tests and scripts finding it).
    SelectionButton* addSelection(const QString& label, const char* name);
    QSpinBox* addCount(const QString& label, const char* name, int value, int min, int max = 1000);
    QDoubleSpinBox* addNumber(const QString& label, const char* name, double value, double min, double max,
                              const QString& suffix = QStringLiteral(" mm"));
    QComboBox* addChoice(const QString& label, const char* name, const QStringList& items, int current = 0);
    QCheckBox* addCheck(const QString& label, const char* name, bool checked);
    QLineEdit* addText(const QString& label, const char* name, const QString& text,
                       const QString& placeholder = {});
    void addWidget(const QString& label, QWidget* input);  // any other input (owned by the panel)
    // A line of warning across the panel, hidden while it's empty (setNote).
    class QLabel* addNote(const char* name);
    void setNote(class QLabel* note, const QString& text);
    void addButtons();  // OK and Cancel
    void setRowVisible(QWidget* input, bool visible);
    void setRowLabel(QWidget* input, const QString& label);

signals:
    void accepted();
    void rejected();
    void changed();  // any input's value changed

protected:
    bool eventFilter(QObject* watched, QEvent* event) override;
    bool event(QEvent* event) override;

private:
    void addRow(const QString& label, QWidget* input);
    void place();

    QWidget* m_canvas;
    QFormLayout* m_form;
};
