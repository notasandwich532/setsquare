#include "NumberInput.h"

#include <core/Expression.h>
#include <core/Sketch.h>  // (SketchError)

#include <QKeyEvent>
#include <QLineEdit>
#include <QSignalBlocker>

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

namespace
{
core::NameLookup& names()
{
    static core::NameLookup lookup;
    return lookup;
}
const QString kDriven = QStringLiteral("\u0192 ");  // (ƒ: worked out from parameters)

// What a box's text says, worked out: and whether it uses names (it's an expression then).
bool readWithNames(const QString& text, double& value, bool& usesNames)
{
    const std::string t = text.trimmed().toStdString();
    try {
        value = core::evaluate(t, names());
        usesNames = !core::namesIn(t).empty();
        return true;
    }
    catch (const core::SketchError&) {
        return false;
    }
}
}  // namespace

void setNumberInputNames(core::NameLookup lookup)
{
    names() = std::move(lookup);
}

bool readNumber(const QString& text, double& value, QString* why)
{
    std::string error;
    try {
        value = core::evaluate(text.trimmed().toStdString(), names());
        return true;
    }
    catch (const core::SketchError& e) {
        error = e.what();
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
    setToolTip(tr("A number, or a sum to work out: 20/3, (100 - 2*15)/2, sqrt(2)*10, 1in; or using the "
                  "drawing's parameters (width / 2), which it then follows"));
    connect(this, &QAbstractSpinBox::editingFinished, this, [this] {
        m_before = value();
        showExpression();
    });
}

bool ExpressionSpinBox::commitText()
{
    double v = 0.0;
    const bool ok = read(text(), v);
    if (!ok) {
        m_expression = m_beforeExpression;
    }
    setValue(ok ? v : m_before);
    m_before = value();
    showExpression();
    return ok;
}

bool ExpressionSpinBox::setExpression(const QString& expression)
{
    double v = 0.0;
    bool usesNames = false;
    if (!readWithNames(expression, v, usesNames)) {
        return false;
    }
    m_expression = usesNames ? expression.trimmed() : QString();
    m_beforeExpression = m_expression;
    setValue(v);
    m_before = value();
    showExpression();
    return true;
}

void ExpressionSpinBox::clearExpression()
{
    m_expression.clear();
    m_beforeExpression.clear();
    showExpression();
}

void ExpressionSpinBox::showExpression()
{
    if (m_plainPrefix.isNull()) {
        m_plainPrefix = prefix().startsWith(kDriven) ? QString(QLatin1String("")) : prefix();
    }
    const QSignalBlocker quiet(this);
    setPrefix(m_expression.isEmpty() ? m_plainPrefix : kDriven + m_plainPrefix);
    setToolTip(m_expression.isEmpty() ? QString() : tr("= %1 (worked out from the drawing's parameters)").arg(m_expression));
    lineEdit()->setText(prefix() + textFromValue(value()) + suffix());  // the result, not the sum
}

void ExpressionSpinBox::stepBy(int steps)
{
    clearExpression();  // (a value of its own now)
    QDoubleSpinBox::stepBy(steps);
}

void ExpressionSpinBox::focusOutEvent(QFocusEvent* event)
{
    QDoubleSpinBox::focusOutEvent(event);
    showExpression();
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
    m_beforeExpression = m_expression;
    QDoubleSpinBox::focusInEvent(event);
    if (!m_expression.isEmpty()) {
        // To edit: the expression, not what it comes to.
        const QSignalBlocker quiet(this);
        setPrefix(m_plainPrefix);
        lineEdit()->setText(m_plainPrefix + m_expression + suffix());
        selectAll();
    }
}

bool ExpressionSpinBox::read(const QString& text, double& value) const
{
    bool usesNames = false;
    QString typed = bare(text, prefix(), suffix());
    if (typed.startsWith(kDriven.trimmed())) {
        typed = typed.mid(kDriven.trimmed().size()).trimmed();
    }
    if (!readWithNames(typed, value, usesNames) || value < minimum() || value > maximum()) {
        return false;
    }
    // (What's typed, as it's read: an expression if it uses names. A number shown back after an
    // expression is the expression's, so it keeps it.)
    bool number = false;
    typed.toDouble(&number);
    if (usesNames) {
        m_expression = typed;
    }
    else if (!(number && !m_expression.isEmpty() && std::abs(typed.toDouble() - value) < 1e-12
               && textFromValue(value) == typed)) {
        m_expression.clear();
    }
    return true;
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
    setToolTip(tr("A whole number, or a sum to work out: 12/4, 2*3+1; or using the drawing's parameters "
                  "(holes), which it then follows"));
    connect(this, &QAbstractSpinBox::editingFinished, this, [this] {
        m_before = value();
        showExpression();
    });
}

bool ExpressionCountBox::commitText()
{
    int v = 0;
    const bool ok = read(text(), v);
    if (!ok) {
        m_expression = m_beforeExpression;
    }
    setValue(ok ? v : m_before);
    m_before = value();
    showExpression();
    return ok;
}

bool ExpressionCountBox::setExpression(const QString& expression)
{
    double v = 0.0;
    bool usesNames = false;
    if (!readWithNames(expression, v, usesNames) || std::abs(v - std::round(v)) > 1e-9) {
        return false;
    }
    m_expression = usesNames ? expression.trimmed() : QString();
    m_beforeExpression = m_expression;
    setValue(static_cast<int>(std::lround(v)));
    m_before = value();
    showExpression();
    return true;
}

void ExpressionCountBox::clearExpression()
{
    m_expression.clear();
    m_beforeExpression.clear();
    showExpression();
}

void ExpressionCountBox::showExpression()
{
    if (m_plainPrefix.isNull()) {
        m_plainPrefix = prefix().startsWith(kDriven) ? QString(QLatin1String("")) : prefix();
    }
    const QSignalBlocker quiet(this);
    setPrefix(m_expression.isEmpty() ? m_plainPrefix : kDriven + m_plainPrefix);
    setToolTip(m_expression.isEmpty() ? QString() : tr("= %1 (worked out from the drawing's parameters)").arg(m_expression));
    lineEdit()->setText(prefix() + textFromValue(value()) + suffix());
}

void ExpressionCountBox::stepBy(int steps)
{
    clearExpression();
    QSpinBox::stepBy(steps);
}

void ExpressionCountBox::focusOutEvent(QFocusEvent* event)
{
    QSpinBox::focusOutEvent(event);
    showExpression();
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
    m_beforeExpression = m_expression;
    QSpinBox::focusInEvent(event);
    if (!m_expression.isEmpty()) {
        const QSignalBlocker quiet(this);
        setPrefix(m_plainPrefix);
        lineEdit()->setText(m_plainPrefix + m_expression + suffix());
        selectAll();
    }
}

bool ExpressionCountBox::read(const QString& text, int& value) const
{
    double v = 0.0;
    bool usesNames = false;
    QString typed = bare(text, prefix(), suffix());
    if (typed.startsWith(kDriven.trimmed())) {
        typed = typed.mid(kDriven.trimmed().size()).trimmed();
    }
    if (!readWithNames(typed, v, usesNames)) {
        return false;
    }
    const double rounded = std::round(v);
    if (std::abs(v - rounded) > 1e-9 || rounded < minimum() || rounded > maximum()) {
        return false;
    }
    value = static_cast<int>(rounded);
    if (usesNames) {
        m_expression = typed;
    }
    else if (!(!m_expression.isEmpty() && textFromValue(value) == typed)) {
        m_expression.clear();
    }
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
