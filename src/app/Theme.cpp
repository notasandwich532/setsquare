#include "Theme.h"

#include <QApplication>
#include <QPalette>
#include <QStyleFactory>

namespace theme
{

const CanvasColors& canvas()
{
    static const CanvasColors colors;
    return colors;
}

void applyDark(QApplication& app)
{
    app.setStyle(QStyleFactory::create("Fusion"));

    const QColor window(0x25, 0x27, 0x2b);
    const QColor base(0x1e, 0x20, 0x23);
    const QColor alternate(0x2a, 0x2d, 0x31);
    const QColor text(0xdc, 0xdc, 0xdc);
    const QColor disabledText(0x7a, 0x7d, 0x82);
    const QColor accent(0x3d, 0x8e, 0xe0);

    QPalette p;
    p.setColor(QPalette::Window, window);
    p.setColor(QPalette::WindowText, text);
    p.setColor(QPalette::Base, base);
    p.setColor(QPalette::AlternateBase, alternate);
    p.setColor(QPalette::ToolTipBase, alternate);
    p.setColor(QPalette::ToolTipText, text);
    p.setColor(QPalette::PlaceholderText, disabledText);
    p.setColor(QPalette::Text, text);
    p.setColor(QPalette::Button, window);
    p.setColor(QPalette::ButtonText, text);
    p.setColor(QPalette::BrightText, Qt::white);
    p.setColor(QPalette::Light, alternate.lighter(130));
    p.setColor(QPalette::Midlight, alternate);
    p.setColor(QPalette::Mid, window.darker(120));
    p.setColor(QPalette::Dark, window.darker(150));
    p.setColor(QPalette::Shadow, Qt::black);
    p.setColor(QPalette::Link, accent);
    p.setColor(QPalette::Highlight, accent);
    p.setColor(QPalette::HighlightedText, Qt::white);

    for (auto role : {QPalette::WindowText, QPalette::Text, QPalette::ButtonText}) {
        p.setColor(QPalette::Disabled, role, disabledText);
    }

    app.setPalette(p);
}

}  // namespace theme
