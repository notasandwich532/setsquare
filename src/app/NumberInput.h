#pragma once

#include <core/Expression.h>

#include <QDoubleSpinBox>
#include <QSpinBox>

// Number boxes that take arithmetic as well as numbers: type "20/3" or "(100 - 2*15) / 2" (see
// core/Expression.h) and it's worked out when you press Enter or leave the box. While what's
// typed doesn't read yet, the value stays as it was; something that never reads is put back to
// what it was before you started typing (the value follows what's typed as it goes, for a live
// preview, so "2" of "2*(" would otherwise stick).

// Names in what's typed (the drawing's parameters: "holes * 2") are looked up with this; unset,
// they don't read. A box given one keeps it as its expression (expression()): it shows ƒ before
// its value, and the expression again to edit. A plain number, or the arrows, drop it.
void setNumberInputNames(core::NameLookup lookup);

class ExpressionSpinBox : public QDoubleSpinBox
{
    Q_OBJECT

public:
    explicit ExpressionSpinBox(QWidget* parent = nullptr);

    // Settles what's typed: its value if it reads (shown as the number), otherwise the value from
    // before typing began, put back. False if it had to be put back.
    bool commitText();

    // What it's worked out from, if that uses parameters (else empty).
    QString expression() const { return m_expression; }
    bool setExpression(const QString& expression);  // its value from it; false (as it was) if it doesn't read
    void clearExpression();                          // (a plain value: e.g. a handle dragged)

protected:
    void keyPressEvent(QKeyEvent* event) override;
    void stepBy(int steps) override;
    void focusOutEvent(QFocusEvent* event) override;
    QValidator::State validate(QString& text, int& pos) const override;
    double valueFromText(const QString& text) const override;
    void fixup(QString& input) const override;
    void focusInEvent(QFocusEvent* event) override;

private:
    bool read(const QString& text, double& value) const;
    void showExpression();  // ƒ before the value while it has one
    double m_before = 0.0;  // the value when editing began
    QString m_beforeExpression;
    mutable QString m_expression;  // (the last read text, when it used names)
    QString m_plainPrefix;
};

class ExpressionCountBox : public QSpinBox
{
    Q_OBJECT

public:
    explicit ExpressionCountBox(QWidget* parent = nullptr);

    bool commitText();  // as ExpressionSpinBox::commitText

    QString expression() const { return m_expression; }  // as ExpressionSpinBox's
    bool setExpression(const QString& expression);
    void clearExpression();

protected:
    void keyPressEvent(QKeyEvent* event) override;
    void stepBy(int steps) override;
    void focusOutEvent(QFocusEvent* event) override;
    QValidator::State validate(QString& text, int& pos) const override;
    int valueFromText(const QString& text) const override;
    void fixup(QString& input) const override;
    void focusInEvent(QFocusEvent* event) override;

private:
    bool read(const QString& text, int& value) const;
    void showExpression();
    int m_before = 0;
    QString m_beforeExpression;
    mutable QString m_expression;
    QString m_plainPrefix;
};

// Reads arithmetic from a plain text field (e.g. the inline dimension editor). False, with why,
// if it doesn't read.
bool readNumber(const QString& text, double& value, QString* why = nullptr);
