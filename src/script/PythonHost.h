#pragma once

#include <core/Sketch.h>

#include <QString>
#include <QStringList>

#include <functional>
#include <set>

class Document;

// The embedded Python interpreter (one per process) and the `setsquare` module it exposes.
//
// Python is bundled with the app: initialise() points the interpreter at the standard library
// copied next to the executable, in isolated mode, so whatever Python is installed on the
// machine (or its environment variables) never gets involved.
namespace script
{

// UI hooks for `setsquare.app`; absent when running headless.
struct UiHooks
{
    std::function<void()> fit;
    std::function<std::set<core::Id>()> selection;
    std::function<void(const std::set<core::Id>&)> select;
    std::function<QString()> tool;
    std::function<bool(const QString&)> setTool;  // false if the name is unknown
    std::function<void(const QString&)> message;
    std::function<bool()> presentation;  // presentation view on?
    std::function<void(bool)> setPresentation;
};

// Starts the interpreter. pythonHome defaults to "<executable dir>/python". Returns false (with
// *error set) if the standard library can't be found.
bool initialise(QString* error = nullptr, const QString& pythonHome = {});
void shutdown();
bool isInitialised();

// The document scripts act on (setsquare.document(), `doc` / `s` in scripts and the console).
void setDocument(Document* document);
Document* document();
void setUiHooks(UiHooks hooks);  // empty hooks = headless

// Where print() and tracebacks go. Unset: the process's stdout/stderr.
void setOutput(std::function<void(const QString& text, bool isError)> sink);

struct RunResult
{
    bool ok = true;
    int exitCode = 0;  // from sys.exit(), or 1 after an uncaught exception
    QString error;     // traceback text when !ok
};

// Runs a .py file with `setsquare`, `doc`, `s` and `app` predefined, as one undo step.
RunResult runFile(const QString& path, const QStringList& argv = {});

// Runs source code the same way (name is used in tracebacks).
RunResult runSource(const QString& source, const QString& name = QStringLiteral("<script>"));

// Interactive console input: an expression's value is printed, names like p12 / l7 / d20
// resolve to sketch items, and variables persist between calls. One undo step per call.
RunResult runConsole(const QString& source);

// Forget console variables (e.g. after opening another file).
void resetConsole();

}  // namespace script
