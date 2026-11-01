// drawapp-cli: runs Python scripts against a sketch without opening a window.
//
//   drawapp-cli script.py [args...]          run a script (sys.argv = [script.py, args...])
//   drawapp-cli --open part.dsk script.py    ...on an existing file
//   drawapp-cli -c "print(s.dof)"            run a snippet
//
// The exit code is the script's: 0 on success, 1 after an uncaught exception (the traceback
// goes to stderr), or whatever sys.exit() was given. Used by the test suite.

#include "Document.h"
#include "PythonHost.h"

#include <QGuiApplication>
#include <QStringList>

#include <cstdio>

namespace
{
void usage()
{
    std::fputs("usage: drawapp-cli [--open FILE.dsk] (SCRIPT.py [ARGS...] | -c CODE)\n", stderr);
}
}  // namespace

int main(int argc, char* argv[])
{
    // A GUI application (fonts, for text) that never shows a window.
    if (qEnvironmentVariableIsEmpty("QT_QPA_PLATFORM")) {
        qputenv("QT_QPA_PLATFORM", "offscreen");
    }
#ifdef _WIN32
    // The offscreen platform doesn't find the system fonts on Windows by itself.
    if (qEnvironmentVariableIsEmpty("QT_QPA_FONTDIR")) {
        qputenv("QT_QPA_FONTDIR", qgetenv("WINDIR") + "\\Fonts");
    }
#endif
    QGuiApplication app(argc, argv);
    QStringList args = QGuiApplication::arguments().mid(1);

    QString openPath, code, scriptPath;
    while (!args.isEmpty()) {
        const QString a = args.takeFirst();
        if (a == QLatin1String("--open") && !args.isEmpty()) {
            openPath = args.takeFirst();
        }
        else if (a == QLatin1String("-c") && !args.isEmpty()) {
            code = args.takeFirst();
            break;
        }
        else if (a == QLatin1String("-h") || a == QLatin1String("--help")) {
            usage();
            return 0;
        }
        else {
            scriptPath = a;
            break;  // everything after the script belongs to it
        }
    }
    if (scriptPath.isEmpty() && code.isEmpty()) {
        usage();
        return 2;
    }

    Document doc;
    if (!openPath.isEmpty()) {
        QString error;
        if (!doc.open(openPath, &error)) {
            std::fprintf(stderr, "%s\n", qPrintable(error));
            return 2;
        }
    }

    QString error;
    if (!script::initialise(&error)) {
        std::fprintf(stderr, "%s\n", qPrintable(error));
        return 2;
    }
    script::setDocument(&doc);
    const script::RunResult result =
        code.isEmpty() ? script::runFile(scriptPath, args) : script::runSource(code, QStringLiteral("<-c>"));
    script::setDocument(nullptr);
    script::shutdown();
    return result.exitCode;
}
