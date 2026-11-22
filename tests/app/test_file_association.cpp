// Registering .setsq files to open in Setsquare (Windows): in a registry key of the test's own,
// never the real file types.

#include "FileAssociation.h"

#include <catch2/catch_test_macros.hpp>

#include <QCoreApplication>
#include <QSettings>

TEST_CASE("File association: registered for a program, seen as moved when it's elsewhere, removed only if ours", "[fileassoc]")
{
    if (!fileassoc::supported()) {
        SKIP("Windows only");
    }
    const QString base = QStringLiteral("HKEY_CURRENT_USER\\Software\\SetsquareTests");
    const QString classes = base + QStringLiteral("\\Classes%1").arg(QCoreApplication::applicationPid());
    const QString exe = QStringLiteral("C:\\Apps\\Setsquare\\setsquare.exe");
    struct Cleanup
    {
        QString key;
        ~Cleanup() { QSettings(key, QSettings::NativeFormat).remove(QString()); }
    } cleanup{base};  // (all of it, the parent too)

    CHECK(!fileassoc::isRegistered(classes, exe));
    REQUIRE(fileassoc::registerFiles(classes, exe));
    CHECK(fileassoc::isRegistered(classes, exe));
    QSettings reg(classes, QSettings::NativeFormat);
    CHECK(reg.value(".setsq/Default").toString() == "Setsquare.Drawing");
    CHECK(reg.value("Setsquare.Drawing/shell/open/command/Default").toString() == "\"C:\\Apps\\Setsquare\\setsquare.exe\" \"%1\"");
    CHECK(reg.value("Setsquare.Drawing/DefaultIcon/Default").toString() == "C:\\Apps\\Setsquare\\setsquare.exe,0");

    // The folder moved: not registered for this copy (asked again, or redone if said yes before).
    CHECK(!fileassoc::isRegistered(classes, QStringLiteral("D:\\Elsewhere\\setsquare.exe")));

    fileassoc::unregisterFiles(classes);
    CHECK(!fileassoc::isRegistered(classes, exe));
    CHECK(!QSettings(classes, QSettings::NativeFormat).contains(".setsq/Default"));

    // .setsq given to another program: left alone.
    REQUIRE(fileassoc::registerFiles(classes, exe));
    QSettings(classes, QSettings::NativeFormat).setValue(".setsq/Default", "Other.App");
    fileassoc::unregisterFiles(classes);
    CHECK(QSettings(classes, QSettings::NativeFormat).value(".setsq/Default").toString() == "Other.App");
}