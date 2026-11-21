#pragma once

#include <QDateTime>
#include <QObject>
#include <QString>

#include <memory>
#include <vector>

class Document;
class QLockFile;
class QTimer;

// Autosave, for recovering work after a crash. While a window is open, a copy of its unsaved work
// is written every so often to a folder of the app's own (never over the drawing's file), held by
// a lock file for as long as the window lives. Saving, a clean close, or nothing left unsaved
// removes the copy; one whose lock no running Setsquare holds was left by a session that ended
// without closing (a crash, a power cut): left() finds those, to offer them back.
class Recovery : public QObject
{
public:
    // `folder`: where the copies go (made if needed); empty: the app's own (defaultFolder()).
    explicit Recovery(Document& doc, QString folder = {}, QObject* parent = nullptr);
    ~Recovery() override;  // a clean close: its copy removed

    static QString defaultFolder();  // <app data>/recovery
    static constexpr int kInterval = 60 * 1000;  // ms between copies (while there are changes)

    // Writes the copy now, if there's unsaved work it doesn't have yet; removes it if there's none.
    void writeNow();
    void setInterval(int ms);

    // A copy left by a session that didn't close.
    struct Found
    {
        QString copy;      // the autosaved drawing
        QString original;  // the file it was unsaved work on (empty: untitled)
        QDateTime when;    // when it was written
    };
    // Those in `folder`, newest first (not ones a running Setsquare still holds).
    static std::vector<Found> left(const QString& folder = {});
    static void remove(const Found& found);  // gone for good

    // For tests: lets go of its copy as a crash would (no lock, files left behind).
    void abandonForTest();

    QString copyPath() const { return m_copy; }  // (exists only while there's unsaved work)

private:
    void removeCopy();

    Document& m_doc;
    QString m_folder;
    QString m_copy, m_info;  // <id>.setsq and <id>.json
    std::unique_ptr<QLockFile> m_lock;
    QTimer* m_timer = nullptr;
    quint64 m_written = 0;  // the revision the copy has (0: none)
};
