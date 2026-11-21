#include "Recovery.h"

#include "Document.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLockFile>
#include <QSaveFile>
#include <QStandardPaths>
#include <QTimer>
#include <QUuid>

#include <algorithm>

namespace
{
QString folderOr(const QString& folder)
{
    return folder.isEmpty() ? Recovery::defaultFolder() : folder;
}
}  // namespace

QString Recovery::defaultFolder()
{
    return QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation) + QStringLiteral("/recovery");
}

Recovery::Recovery(Document& doc, QString folder, QObject* parent)
    : QObject(parent)
    , m_doc(doc)
    , m_folder(folderOr(folder))
{
    QDir().mkpath(m_folder);
    const QString id = QUuid::createUuid().toString(QUuid::WithoutBraces);
    m_copy = m_folder + QLatin1Char('/') + id + QStringLiteral(".setsq");
    m_info = m_folder + QLatin1Char('/') + id + QStringLiteral(".json");
    m_lock = std::make_unique<QLockFile>(m_folder + QLatin1Char('/') + id + QStringLiteral(".lock"));
    m_lock->setStaleLockTime(0);  // (only a dead process's lock is stale, however old)
    m_lock->tryLock(0);

    m_timer = new QTimer(this);
    m_timer->setInterval(kInterval);
    connect(m_timer, &QTimer::timeout, this, &Recovery::writeNow);
    m_timer->start();
    // Saved, opened another, started over: nothing unsaved, so no copy.
    connect(&m_doc, &Document::fileStateChanged, this, [this] {
        if (!m_doc.isModified()) {
            removeCopy();
        }
    });
}

Recovery::~Recovery()
{
    removeCopy();
}

void Recovery::setInterval(int ms)
{
    m_timer->setInterval(ms);
}

void Recovery::writeNow()
{
    if (!m_doc.isModified()) {
        removeCopy();
        return;
    }
    if (m_written == m_doc.revision() || !m_lock || !m_lock->isLocked()) {
        return;  // (it has this already; or a test let go of it)
    }
    if (!m_doc.saveCopy(m_copy)) {
        return;  // (it'll try again next time)
    }
    QJsonObject info;
    info[QStringLiteral("original")] = m_doc.filePath();
    info[QStringLiteral("when")] = QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs);
    QSaveFile file(m_info);
    if (file.open(QIODevice::WriteOnly)) {
        file.write(QJsonDocument(info).toJson());
        file.commit();
    }
    m_written = m_doc.revision();
}

void Recovery::removeCopy()
{
    QFile::remove(m_copy);
    QFile::remove(m_info);
    m_written = 0;
}

void Recovery::abandonForTest()
{
    m_timer->stop();
    m_lock->unlock();  // (removes the lock file: as if the process had gone)
    m_lock.reset();
    m_copy.clear();
    m_info.clear();
}

std::vector<Recovery::Found> Recovery::left(const QString& folder)
{
    const QDir dir(folderOr(folder));
    std::vector<Found> out;
    for (const QFileInfo& copy : dir.entryInfoList({QStringLiteral("*.setsq")}, QDir::Files)) {
        const QString base = dir.filePath(copy.completeBaseName());
        // A running session's lock can't be taken: its copy isn't left over. (Taking a dead one's
        // clears it; let go at once.)
        QLockFile lock(base + QStringLiteral(".lock"));
        lock.setStaleLockTime(0);
        if (!lock.tryLock(0)) {
            continue;
        }
        lock.unlock();
        Found f;
        f.copy = copy.absoluteFilePath();
        f.when = copy.lastModified();
        QFile infoFile(base + QStringLiteral(".json"));
        if (infoFile.open(QIODevice::ReadOnly)) {
            const QJsonObject info = QJsonDocument::fromJson(infoFile.readAll()).object();
            f.original = info.value(QStringLiteral("original")).toString();
            const QDateTime when = QDateTime::fromString(info.value(QStringLiteral("when")).toString(), Qt::ISODateWithMs);
            if (when.isValid()) {
                f.when = when.toLocalTime();
            }
        }
        out.push_back(std::move(f));
    }
    std::ranges::sort(out, [](const Found& a, const Found& b) { return a.when > b.when; });
    // Locks of sessions that ended before writing anything: nothing to offer, so cleared away
    // (taking a dead one and letting go removes it).
    for (const QFileInfo& lockInfo : dir.entryInfoList({QStringLiteral("*.lock")}, QDir::Files)) {
        if (QFile::exists(dir.filePath(lockInfo.completeBaseName() + QStringLiteral(".setsq")))) {
            continue;
        }
        QLockFile lock(lockInfo.absoluteFilePath());
        lock.setStaleLockTime(0);
        if (lock.tryLock(0)) {
            lock.unlock();
        }
    }
    return out;
}

void Recovery::remove(const Found& found)
{
    const QFileInfo copy(found.copy);
    const QString base = copy.absolutePath() + QLatin1Char('/') + copy.completeBaseName();
    QFile::remove(found.copy);
    QFile::remove(base + QStringLiteral(".json"));
    QFile::remove(base + QStringLiteral(".lock"));
}
