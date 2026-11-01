#pragma once

#include <QIcon>
#include <QString>

// Tool icons: SVGs drawn with the app itself (icons/build_icons.py), compiled in as resources
// (":/icons/<name>.svg"). They're drawn fresh at whatever size is asked, so they stay sharp at any
// scaling, and recoloured per state: as drawn normally, white when checked (the green active
// tool), faded when disabled.
namespace icons
{

// The icon for a tool or command by name (as MainWindow::toolName gives it), or a null icon if
// there isn't one yet.
QIcon forName(const QString& name);

// The icon's SVG for a state: QIcon::On gives the white version. Empty if there's none.
QByteArray svg(const QString& name, QIcon::State state = QIcon::Off);

// The app's own icon (icons/app/drawapp.svg), drawn at the usual window/taskbar sizes.
QIcon appIcon();

}  // namespace icons
