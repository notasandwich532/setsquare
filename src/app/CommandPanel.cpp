#include "CommandPanel.h"

#include "NumberInput.h"

#include <QCheckBox>
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QEvent>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QPlainTextEdit>
#include <QSpinBox>
#include <QStyle>
#include <QToolButton>
#include <QVBoxLayout>

namespace
{
constexpr int kMargin = 10;  // from the canvas edges

const char* kStyle = R"(
CommandPanel { background: #2b2f36; border: 1px solid #454b55; border-radius: 3px; }
CommandPanel QLabel#commandTitle { color: #c9d1db; font-weight: bold; padding: 2px 0 4px 0; }
CommandPanel QLabel { color: #c9d1db; }
SelectionButton { text-align: left; padding: 2px 8px; border: 1px solid #555c66; border-radius: 2px;
                  background: #353a42; color: #dfe5ec; }
SelectionButton[current="true"] { border: 1px solid #4da3ff; background: #2f4a66; }
)";
}  // namespace

// ---------------------------------------------------------------------------------------------

SelectionButton::SelectionButton(QWidget* parent)
    : QPushButton(parent)
{
    setCount(0);
    setCursor(Qt::PointingHandCursor);
}

void SelectionButton::setCount(int count)
{
    setText(count == 0 ? tr("▸ Select") : tr("▸ %n selected", nullptr, count));
}

void SelectionButton::setCurrent(bool current)
{
    m_current = current;
    setProperty("current", current);
    style()->unpolish(this);  // re-apply the stylesheet for the property
    style()->polish(this);
}

// ---------------------------------------------------------------------------------------------

CommandPanel::CommandPanel(const QString& title, QWidget* canvas)
    : QFrame(canvas)
    , m_canvas(canvas)
{
    setObjectName(QStringLiteral("commandPanel"));
    setStyleSheet(QString::fromLatin1(kStyle));
    setAutoFillBackground(true);
    auto* outer = new QVBoxLayout(this);
    outer->setContentsMargins(10, 6, 10, 10);
    outer->setSpacing(4);
    auto* heading = new QLabel(QStringLiteral("— ") + title.toUpper(), this);
    heading->setObjectName(QStringLiteral("commandTitle"));
    outer->addWidget(heading);
    m_form = new QFormLayout;
    m_form->setLabelAlignment(Qt::AlignLeft);
    m_form->setHorizontalSpacing(14);
    m_form->setVerticalSpacing(5);
    outer->addLayout(m_form);
    canvas->installEventFilter(this);  // to stay in its corner
    show();
    place();
}

void CommandPanel::addRow(const QString& label, QWidget* input)
{
    input->setMinimumWidth(130);
    input->installEventFilter(this);  // Enter / Esc
    m_form->addRow(label, input);
    adjustSize();
    place();
}

SelectionButton* CommandPanel::addSelection(const QString& label, const char* name)
{
    auto* row = new QWidget(this);
    auto* layout = new QHBoxLayout(row);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(4);
    auto* button = new SelectionButton(row);
    button->setObjectName(QLatin1String(name));
    auto* clear = new QToolButton(row);
    clear->setText(QStringLiteral("×"));
    clear->setAutoRaise(true);
    clear->setToolTip(tr("Clear"));
    connect(clear, &QToolButton::clicked, button, &SelectionButton::cleared);
    layout->addWidget(button, 1);
    layout->addWidget(clear);
    addRow(label, row);
    return button;
}

QSpinBox* CommandPanel::addCount(const QString& label, const char* name, int value, int min, int max)
{
    auto* box = new ExpressionCountBox(this);
    box->setObjectName(QLatin1String(name));
    box->setRange(min, max);
    box->setValue(value);
    connect(box, &QSpinBox::valueChanged, this, &CommandPanel::changed);
    addRow(label, box);
    return box;
}

QDoubleSpinBox* CommandPanel::addNumber(const QString& label, const char* name, double value, double min, double max,
                                        const QString& suffix)
{
    auto* box = new ExpressionSpinBox(this);
    box->setObjectName(QLatin1String(name));
    box->setDecimals(3);
    box->setRange(min, max);
    box->setValue(value);
    box->setSuffix(suffix);
    connect(box, &QDoubleSpinBox::valueChanged, this, &CommandPanel::changed);
    addRow(label, box);
    return box;
}

QLineEdit* CommandPanel::addText(const QString& label, const char* name, const QString& text,
                                 const QString& placeholder)
{
    auto* edit = new QLineEdit(text, this);
    edit->setObjectName(QLatin1String(name));
    edit->setPlaceholderText(placeholder);
    connect(edit, &QLineEdit::textChanged, this, &CommandPanel::changed);
    addRow(label, edit);
    return edit;
}

QComboBox* CommandPanel::addChoice(const QString& label, const char* name, const QStringList& items, int current)
{
    auto* box = new QComboBox(this);
    box->setObjectName(QLatin1String(name));
    box->addItems(items);
    box->setCurrentIndex(current);
    connect(box, &QComboBox::currentIndexChanged, this, &CommandPanel::changed);
    addRow(label, box);
    return box;
}

QCheckBox* CommandPanel::addCheck(const QString& label, const char* name, bool checked)
{
    auto* box = new QCheckBox(this);
    box->setObjectName(QLatin1String(name));
    box->setChecked(checked);
    connect(box, &QCheckBox::toggled, this, &CommandPanel::changed);
    addRow(label, box);
    return box;
}

QLabel* CommandPanel::addNote(const char* name)
{
    auto* note = new QLabel(this);
    note->setObjectName(QLatin1String(name));
    note->setWordWrap(true);
    note->setStyleSheet(QStringLiteral("color: #f2b33d;"));  // (a warning's amber)
    m_form->addRow(note);
    m_form->setRowVisible(note, false);
    adjustSize();
    place();
    return note;
}

void CommandPanel::setNote(QLabel* note, const QString& text)
{
    if (note->text() == text && m_form->isRowVisible(note) == !text.isEmpty()) {
        return;
    }
    note->setText(text);
    m_form->setRowVisible(note, !text.isEmpty());
    adjustSize();
    place();
}
void CommandPanel::addWidget(const QString& label, QWidget* input)
{
    input->setParent(this);
    addRow(label, input);
}

void CommandPanel::addButtons()
{
    auto* row = new QWidget(this);
    auto* layout = new QHBoxLayout(row);
    layout->setContentsMargins(0, 6, 0, 0);
    layout->addStretch();
    auto* ok = new QPushButton(tr("OK"), row);
    ok->setObjectName(QStringLiteral("ok"));
    auto* cancel = new QPushButton(tr("Cancel"), row);
    cancel->setObjectName(QStringLiteral("cancel"));
    layout->addWidget(ok);
    layout->addWidget(cancel);
    connect(ok, &QPushButton::clicked, this, &CommandPanel::accepted);
    connect(cancel, &QPushButton::clicked, this, &CommandPanel::rejected);
    static_cast<QVBoxLayout*>(this->layout())->addWidget(row);
    adjustSize();
    place();
}

void CommandPanel::setRowVisible(QWidget* input, bool visible)
{
    QWidget* field = input;
    while (field->parentWidget() && field->parentWidget() != this) {
        field = field->parentWidget();  // a selection's row widget
    }
    m_form->setRowVisible(field, visible);
    adjustSize();
    place();
}

void CommandPanel::setRowLabel(QWidget* input, const QString& label)
{
    QWidget* field = input;
    while (field->parentWidget() && field->parentWidget() != this) {
        field = field->parentWidget();
    }
    if (auto* l = qobject_cast<QLabel*>(m_form->labelForField(field))) {
        if (l->text() != label) {
            l->setText(label);
            m_form->invalidate();
            adjustSize();
            place();
        }
    }
}

void CommandPanel::place()
{
    move(m_canvas->width() - width() - kMargin, kMargin);
}

bool CommandPanel::event(QEvent* event)
{
    const bool handled = QFrame::event(event);
    if (event->type() == QEvent::LayoutRequest) {
        // Inputs show up after the panel itself does (and rows come and go): fit them.
        adjustSize();
        place();
    }
    return handled;
}

bool CommandPanel::eventFilter(QObject* watched, QEvent* event)
{
    if (watched == m_canvas && event->type() == QEvent::Resize) {
        place();
    }
    else if (watched != m_canvas && event->type() == QEvent::KeyPress) {
        const auto* keyEvent = static_cast<QKeyEvent*>(event);
        const int key = keyEvent->key();
        if ((key == Qt::Key_Return || key == Qt::Key_Enter) && qobject_cast<QPlainTextEdit*>(watched)
            && !(keyEvent->modifiers() & Qt::ControlModifier)) {
            return false;  // a new line in a multi-line box (Ctrl+Enter accepts)
        }
        if (auto* combo = qobject_cast<QComboBox*>(watched);
            combo && combo->isEditable() && (key == Qt::Key_Return || key == Qt::Key_Enter)) {
            // Takes what's typed in it (e.g. a width): its own Enter, not the panel's, and no further
            // (the canvas would take it as "done").
            emit combo->lineEdit()->editingFinished();
            return true;
        }
        if (key == Qt::Key_Return || key == Qt::Key_Enter) {
            // A sum typed in a number box is worked out first; one that doesn't read is put back,
            // and that Enter doesn't go ahead (so nothing is made from a half-typed sum).
            bool settled = true;
            if (auto* number = qobject_cast<ExpressionSpinBox*>(watched)) {
                settled = number->commitText();
            }
            else if (auto* count = qobject_cast<ExpressionCountBox*>(watched)) {
                settled = count->commitText();
            }
            if (settled) {
                emit accepted();
            }
            return true;
        }
        if (key == Qt::Key_Escape) {
            emit rejected();
            return true;
        }
    }
    return QFrame::eventFilter(watched, event);
}
