#include "FileAssociation.h"

#include <QCoreApplication>
#include <QDir>
#include <QMessageBox>
#include <QPushButton>
#include <QSettings>

#ifdef Q_OS_WIN
#include <windows.h>
#include <shlobj.h>
#endif

namespace fileassoc
{

namespace
{
QString root(const QString& classes)
{
    return classes.isEmpty() ? QStringLiteral("HKEY_CURRENT_USER\\Software\\Classes") : classes;
}
QString program(const QString& exe)
{
    return QDir::toNativeSeparators(exe.isEmpty() ? QCoreApplication::applicationFilePath() : exe);
}
QString openCommand(const QString& exe)
{
    return QStringLiteral("\"%1\" \"%2\"").arg(program(exe), QStringLiteral("%1"));
}
void tellExplorer(const QString& classes)
{
#ifdef Q_OS_WIN
    if (classes.isEmpty()) {
        SHChangeNotify(SHCNE_ASSOCCHANGED, SHCNF_IDLIST, nullptr, nullptr);  // (icons and "Open" update)
    }
#else
    Q_UNUSED(classes);
#endif
}
}  // namespace

bool supported()
{
#ifdef Q_OS_WIN
    return true;
#else
    return false;
#endif
}

bool isRegistered(const QString& classes, const QString& exe)
{
    if (!supported()) {
        return false;
    }
    QSettings reg(root(classes), QSettings::NativeFormat);
    return reg.value(QStringLiteral("%1/Default").arg(QLatin1String(kExtension))).toString() == QLatin1String(kProgId)
        && reg.value(QStringLiteral("%1/shell/open/command/Default").arg(QLatin1String(kProgId))).toString()
               .compare(openCommand(exe), Qt::CaseInsensitive) == 0;
}

bool registerFiles(const QString& classes, const QString& exe)
{
    if (!supported()) {
        return false;
    }
    QSettings reg(root(classes), QSettings::NativeFormat);
    const QString ext = QLatin1String(kExtension), id = QLatin1String(kProgId);
    reg.setValue(ext + QStringLiteral("/Default"), id);
    reg.setValue(id + QStringLiteral("/Default"), QStringLiteral("Setsquare drawing"));
    reg.setValue(id + QStringLiteral("/DefaultIcon/Default"), program(exe) + QStringLiteral(",0"));
    reg.setValue(id + QStringLiteral("/shell/open/command/Default"), openCommand(exe));
    reg.sync();
    if (reg.status() != QSettings::NoError) {
        return false;
    }
    tellExplorer(classes);
    return isRegistered(classes, exe);
}

void unregisterFiles(const QString& classes)
{
    if (!supported()) {
        return;
    }
    QSettings reg(root(classes), QSettings::NativeFormat);
    const QString ext = QLatin1String(kExtension), id = QLatin1String(kProgId);
    if (reg.value(ext + QStringLiteral("/Default")).toString() == id) {
        reg.remove(ext);  // (only if .setsq is ours)
    }
    reg.remove(id);
    reg.sync();
    tellExplorer(classes);
}

void offerAtStartup(QWidget* parent)
{
    if (!supported() || isRegistered()) {
        return;
    }
    QSettings settings;
    const QString answer = settings.value(QStringLiteral("fileAssociation")).toString();
    if (answer == QLatin1String("yes")) {
        registerFiles();  // (said yes before: this copy's moved, so registered where it is now)
        return;
    }
    if (answer == QLatin1String("no")) {
        return;
    }
    QMessageBox box(QMessageBox::Question, QObject::tr("Open drawings with Setsquare?"),
                    QObject::tr("Open .setsq drawings in Setsquare when you double-click them?"), QMessageBox::NoButton,
                    parent);
    box.setInformativeText(QObject::tr("You can change this later in the File menu."));
    QPushButton* yes = box.addButton(QObject::tr("Yes"), QMessageBox::YesRole);
    box.addButton(QObject::tr("No"), QMessageBox::NoRole);
    box.setDefaultButton(yes);
    box.exec();
    const bool accepted = box.clickedButton() == yes;
    settings.setValue(QStringLiteral("fileAssociation"), accepted ? QStringLiteral("yes") : QStringLiteral("no"));
    if (accepted && !registerFiles()) {
        QMessageBox::warning(parent, QObject::tr("Open drawings with Setsquare"),
                             QObject::tr("Windows didn't let Setsquare register .setsq files."));
    }
}

}  // namespace fileassoc
