#include "ScriptConsole.h"

#include "Document.h"
#include "PythonHost.h"
#include "Theme.h"

#include <QCheckBox>
#include <QFileDialog>
#include <QFileInfo>
#include <QFontDatabase>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QScrollBar>
#include <QStandardPaths>
#include <QTextBlock>
#include <QVBoxLayout>

namespace
{
const QColor kInputColor(0x9f, 0xd0, 0xff);
const QColor kOutputColor(0xdc, 0xdc, 0xdc);
const QColor kErrorColor(0xff, 0x6b, 0x6b);
const QColor kRecordedColor(0x7f, 0xc9, 0x7f);
const QColor kInfoColor(0x8a, 0x8f, 0x98);
}  // namespace

ScriptConsole::ScriptConsole(Document& doc, QWidget* parent)
    : QWidget(parent)
    , m_doc(doc)
    , m_output(new QPlainTextEdit(this))
    , m_input(new QPlainTextEdit(this))
    , m_record(new QCheckBox(tr("Record UI actions"), this))
{
    const QFont mono = QFontDatabase::systemFont(QFontDatabase::FixedFont);
    m_output->setReadOnly(true);
    m_output->setFont(mono);
    m_output->setObjectName(QStringLiteral("consoleOutput"));
    m_output->setMaximumBlockCount(5000);
    m_input->setFont(mono);
    m_input->setObjectName(QStringLiteral("consoleInput"));
    m_input->setPlaceholderText(tr("Python  (Enter runs, Shift+Enter for a new line, Up/Down for history)"));
    m_input->setFixedHeight(QFontMetrics(mono).lineSpacing() * 3 + 12);
    m_input->installEventFilter(this);
    m_record->setChecked(true);
    m_record->setToolTip(tr("Show the Python equivalent of each action you take in the UI"));

    auto* runButton = new QPushButton(tr("Run Script..."), this);
    auto* clearButton = new QPushButton(tr("Clear"), this);
    connect(runButton, &QPushButton::clicked, this, [this] {
        const QString start = QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation);
        const QString path = QFileDialog::getOpenFileName(this, tr("Run Script"), start, tr("Python (*.py)"));
        if (!path.isEmpty()) {
            runFile(path);
        }
    });
    connect(clearButton, &QPushButton::clicked, this, &ScriptConsole::clear);

    auto* buttons = new QHBoxLayout;
    buttons->addWidget(m_record);
    buttons->addStretch();
    buttons->addWidget(runButton);
    buttons->addWidget(clearButton);

    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(4, 4, 4, 4);
    layout->addWidget(m_output, 1);
    layout->addWidget(m_input);
    layout->addLayout(buttons);

    connect(&m_doc, &Document::recorded, this, &ScriptConsole::onRecorded);

    m_available = script::isInitialised();
    if (m_available) {
        script::setOutput([this](const QString& text, bool isError) {
            append(text, isError ? kErrorColor : kOutputColor);
        });
        append(tr("Python ready. `doc`, `s` (the sketch) and `app` are predefined; try  s.line((0, 0), (40, 0))\n"),
               kInfoColor);
    }
    else {
        m_input->setEnabled(false);
        runButton->setEnabled(false);
        append(tr("Python is not available, so scripting is disabled.\n"), kErrorColor);
    }
}

ScriptConsole::~ScriptConsole()
{
    if (m_available) {
        script::setOutput(nullptr);  // the sink points at this widget
    }
}

void ScriptConsole::append(const QString& text, const QColor& color)
{
    QTextCharFormat format;
    format.setForeground(color);
    QTextCursor cursor(m_output->document());
    cursor.movePosition(QTextCursor::End);
    cursor.insertText(text, format);
    m_output->verticalScrollBar()->setValue(m_output->verticalScrollBar()->maximum());
}

void ScriptConsole::onRecorded(const QString& python)
{
    m_recording << python;
    if (m_record->isChecked()) {
        append(python + QLatin1Char('\n'), kRecordedColor);
    }
}

void ScriptConsole::clear()
{
    m_output->clear();
    m_recording.clear();
}

void ScriptConsole::execute(const QString& source)
{
    if (!m_available || source.trimmed().isEmpty()) {
        return;
    }
    // Echo the input like an interactive prompt.
    QStringList lines = source.split(QLatin1Char('\n'));
    for (int i = 0; i < lines.size(); ++i) {
        lines[i].prepend(i == 0 ? QStringLiteral(">>> ") : QStringLiteral("... "));
    }
    append(lines.join(QLatin1Char('\n')) + QLatin1Char('\n'), kInputColor);
    m_history << source;
    m_historyPos = static_cast<int>(m_history.size());
    script::runConsole(source);
}

void ScriptConsole::runFile(const QString& path)
{
    if (!m_available) {
        return;
    }
    append(tr("# running %1\n").arg(QFileInfo(path).fileName()), kInfoColor);
    const script::RunResult r = script::runFile(path);
    append(r.ok ? tr("# done\n") : tr("# failed (exit code %1)\n").arg(r.exitCode), r.ok ? kInfoColor : kErrorColor);
}

bool ScriptConsole::eventFilter(QObject* watched, QEvent* event)
{
    if (watched == m_input && event->type() == QEvent::KeyPress) {
        auto* key = static_cast<QKeyEvent*>(event);
        const bool plainEnter = (key->key() == Qt::Key_Return || key->key() == Qt::Key_Enter)
            && !(key->modifiers() & Qt::ShiftModifier);
        if (plainEnter) {
            const QString source = m_input->toPlainText();
            m_input->clear();
            execute(source);
            return true;
        }
        // History, when the cursor is on the first/last line.
        const QTextCursor cursor = m_input->textCursor();
        if (key->key() == Qt::Key_Up && cursor.blockNumber() == 0 && m_historyPos > 0) {
            m_input->setPlainText(m_history[--m_historyPos]);
            m_input->moveCursor(QTextCursor::End);
            return true;
        }
        if (key->key() == Qt::Key_Down && cursor.blockNumber() == m_input->document()->blockCount() - 1) {
            if (m_historyPos < m_history.size() - 1) {
                m_input->setPlainText(m_history[++m_historyPos]);
            }
            else {
                m_historyPos = static_cast<int>(m_history.size());
                m_input->clear();
            }
            m_input->moveCursor(QTextCursor::End);
            return true;
        }
    }
    return QWidget::eventFilter(watched, event);
}
