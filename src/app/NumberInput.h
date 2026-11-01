#pragma once

#include <QDoubleSpinBox>
#include <QSpinBox>

// Number boxes that take arithmetic as well as numbers: type "20/3" or "(100 - 2*15) / 2" (see
// core/Expression.h) and it's worked out when you press Enter or leave the box. While what's
// typed doesn't read yet, the value stays as it was; something that never reads is put back to
// what it was before you started typing (the value follows what's typed as it goes, for a live
// preview, so "2" of "2*(" would otherwise stick).

class ExpressionSpinBox : public QDoubleSpinBox
{
    Q_OBJECT

public:
    explicit ExpressionSpinBox(QWidget* parent = nullptr);

    // Settles what's typed: its value if it reads (shown as the number), otherwise the value from
    // before typing began, put back. False if it had to be put back.
    bool commitText();

protected:
    void keyPressEvent(QKeyEvent* event) override;
    QValidator::State validate(QString& text, int& pos) const override;
    double valueFromText(const QString& text) const override;
    void fixup(QString& input) const override;
    void focusInEvent(QFocusEvent* event) override;

private:
    bool read(const QString& text, double& value) const;
    double m_before = 0.0;  // the value when editing began
};

class ExpressionCountBox : public QSpinBox
{
    Q_OBJECT

public:
    explicit ExpressionCountBox(QWidget* parent = nullptr);

    bool commitText();  // as ExpressionSpinBox::commitText

protected:
    void keyPressEvent(QKeyEvent* event) override;
    QValidator::State validate(QString& text, int& pos) const override;
    int valueFromText(const QString& text) const override;
    void fixup(QString& input) const override;
    void focusInEvent(QFocusEvent* event) override;

private:
    bool read(const QString& text, int& value) const;
    int m_before = 0;
};

// Reads arithmetic from a plain text field (e.g. the inline dimension editor). False, with why,
// if it doesn't read.
bool readNumber(const QString& text, double& value, QString* why = nullptr);
