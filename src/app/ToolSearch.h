#pragma once

#include <QFrame>

#include <vector>

class QAction;
class QLineEdit;
class QListWidget;

// Fusion's "S" box: a popup at the cursor to find a tool or command by typing part of its name.
// Up/Down pick, Enter runs, Esc closes. Searches the given actions (with their menu paths).
class ToolSearch : public QFrame
{
    Q_OBJECT

public:
    explicit ToolSearch(QWidget* parent);

    struct Entry
    {
        QAction* action = nullptr;
        QString name;      // without mnemonics
        QString context;   // e.g. "Sketch ▸ Chamfer"
    };
    void setEntries(std::vector<Entry> entries);

    // Shows the box with its top-left near `globalPos`, empty and ready to type.
    void popup(QPoint globalPos);

    // Entries matching `query`, best first (for tests).
    std::vector<const Entry*> matches(const QString& query) const;

protected:
    bool eventFilter(QObject* watched, QEvent* event) override;

private:
    void refill();
    void runCurrent();

    QLineEdit* m_input;
    QListWidget* m_list;
    std::vector<Entry> m_entries;
};
