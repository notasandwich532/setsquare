#include "NumberInput.h"

#include <core/Expression.h>

#include <QKeyEvent>
#include <QLineEdit>

#include <cmath>

namespace
{
// The typed part, without the box's prefix and suffix (" mm", " °").
QString bare(const QString& text, const QString& prefix, const QString& suffix)
{
    QString t = text;
    if (!prefix.isEmpty() && t.startsWith(prefix)) {
        t.remove(0, prefix.size());
    }
    const QString trimmedSuffix = suffix.trimmed();
    t = t.trimmed();
    if (!trimmedSuffix.isEmpty() && t.endsWith(trimmedSuffix)) {
        t.chop(trimmedSuffix.size());
    }
    return t.trimmed();
}
}  // namespace

bool readNumber(const QString& text, double& value, QString* why)
{
    std::string error;
    if (core::tryEvaluate(text.trimmed().toStdString(), value, &error)) {
        return true;
    }
    if (why) {
        *why = QString::fromStdString(error);
    }
    return false;
}

// ---------------------------------------------------------------------------------------------

ExpressionSpinBox::ExpressionSpinBox(QWidget* parent)
    : QDoubleSpinBox(parent)
{
    setToolTip(tr("A number, or a sum to work out: 20/3, (100 - 2*15)/2, sqrt(2)*10, 1in"));
    connect(this, &QAbstractSpinBox::editingFinished, this, [this] { m_before = value(); });
}

bool ExpressionSpinBox::commitText()
{
    double v = 0.0;
    const bool ok = read(text(), v);
    setValue(ok ? v : m_before);
    lineEdit()->setText(prefix() + textFromValue(value()) + suffix());  // the result, not the sum
    m_before = value();
    return ok;
}

void ExpressionSpinBox::keyPressEvent(QKeyEvent* event)
{
    if (event->key() == Qt::Key_Return || event->key() == Qt::Key_Enter) {
        commitText();
    }
    QDoubleSpinBox::keyPressEvent(event);
}

void ExpressionSpinBox::focusInEvent(QFocusEvent* event)
{
    m_before = value();
    QDoubleSpinBox::focusInEvent(event);
}

bool ExpressionSpinBox::read(const QString& text, double& value) const
{
    return readNumber(bare(text, prefix(), suffix()), value) && value >= minimum() && value <= maximum();
}

QValidator::State ExpressionSpinBox::validate(QString& text, int& /*pos*/) const
{
    // Anything may be on its way to a sum ("2+", "(3"), so typing is never refused.
    double v = 0.0;
    return read(text, v) ? QValidator::Acceptable : QValidator::Intermediate;
}

double ExpressionSpinBox::valueFromText(const QString& text) const
{
    double v = m_before;  // unreadable (Enter on "2*("): as it was before typing
    read(text, v);
    return v;
}

void ExpressionSpinBox::fixup(QString& input) const
{
    double v = 0.0;
    input = textFromValue(read(input, v) ? v : m_before);  // unreadable: put it back
}

// ---------------------------------------------------------------------------------------------

ExpressionCountBox::ExpressionCountBox(QWidget* parent)
    : QSpinBox(parent)
{
    setToolTip(tr("A whole number, or a sum to work out: 12/4, 2*3+1"));
    connect(this, &QAbstractSpinBox::editingFinished, this, [this] { m_before = value(); });
}

bool ExpressionCountBox::commitText()
{
    int v = 0;
    const bool ok = read(text(), v);
    setValue(ok ? v : m_before);
    lineEdit()->setText(prefix() + textFromValue(value()) + suffix());
    m_before = value();
    return ok;
}

void ExpressionCountBox::keyPressEvent(QKeyEvent* event)
{
    if (event->key() == Qt::Key_Return || event->key() == Qt::Key_Enter) {
        commitText();
    }
    QSpinBox::keyPressEvent(event);
}

void ExpressionCountBox::focusInEvent(QFocusEvent* event)
{
    m_before = value();
    QSpinBox::focusInEvent(event);
}

bool ExpressionCountBox::read(const QString& text, int& value) const
{
    double v = 0.0;
    if (!readNumber(bare(text, prefix(), suffix()), v)) {
        return false;
    }
    const double rounded = std::round(v);
    if (std::abs(v - rounded) > 1e-9 || rounded < minimum() || rounded > maximum()) {
        return false;
    }
    value = static_cast<int>(rounded);
    return true;
}

QValidator::State ExpressionCountBox::validate(QString& text, int& /*pos*/) const
{
    int v = 0;
    return read(text, v) ? QValidator::Acceptable : QValidator::Intermediate;
}

int ExpressionCountBox::valueFromText(const QString& text) const
{
    int v = m_before;
    read(text, v);
    return v;
}

void ExpressionCountBox::fixup(QString& input) const
{
    int v = 0;
    input = textFromValue(read(input, v) ? v : m_before);
}
