#pragma once

#include <QStringList>
#include <QWidget>

class Document;
class QCheckBox;
class QPlainTextEdit;

// Python console dock: run code against the sketch, see output, and watch UI actions appear
// as the Python that reproduces them (the macro recorder).
class ScriptConsole : public QWidget
{
    Q_OBJECT

public:
    explicit ScriptConsole(Document& doc, QWidget* parent = nullptr);
    ~ScriptConsole() override;

    void runFile(const QString& path);
    void execute(const QString& source);  // as if typed in

    // Everything recorded since the console was created or cleared.
    const QStringList& recording() const { return m_recording; }

public slots:
    void clear();

private:
    void append(const QString& text, const QColor& color);
    void onRecorded(const QString& python);
    bool eventFilter(QObject* watched, QEvent* event) override;

    Document& m_doc;
    QPlainTextEdit* m_output;
    QPlainTextEdit* m_input;
    QCheckBox* m_record;
    QStringList m_history;
    int m_historyPos = 0;
    QStringList m_recording;
    bool m_available = false;
};
