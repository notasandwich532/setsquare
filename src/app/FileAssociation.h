#pragma once

#include <QString>

class QWidget;

// Double-clicking a .setsq file opens it in Setsquare (Windows): the file type registered for the
// current user only (HKEY_CURRENT_USER\Software\Classes: no admin rights needed), as a
// "Setsquare drawing" with the app's icon, opening with this copy of setsquare.exe. (If someone
// has chosen another program for .setsq in Windows' own settings, that choice still wins: Windows
// doesn't let programs override it.) Elsewhere, none of this does anything.
namespace fileassoc
{

constexpr const char* kExtension = ".setsq";
constexpr const char* kProgId = "Setsquare.Drawing";

// Whether it can be done here (Windows).
bool supported();

// `classes`: where file types are registered; empty: the current user's
// (HKEY_CURRENT_USER\Software\Classes). Tests give their own. `exe`: the program; empty: this one.
bool isRegistered(const QString& classes = {}, const QString& exe = {});  // to open with `exe`
bool registerFiles(const QString& classes = {}, const QString& exe = {});
void unregisterFiles(const QString& classes = {});  // (only if it's ours)

// At start-up: if .setsq doesn't open with this copy, asks once (the answer kept); once said yes,
// a copy that's moved is registered again without asking. `parent`: for the question.
void offerAtStartup(QWidget* parent);

}  // namespace fileassoc
