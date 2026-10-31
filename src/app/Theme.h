#pragma once

#include <QColor>

class QApplication;

namespace theme
{

// Colours used by the sketch canvas. Dark mode is the default (and for now only) theme.
struct CanvasColors
{
    QColor background{0x1b, 0x1d, 0x21};
    QColor gridMinor{0x26, 0x29, 0x2e};
    QColor gridMajor{0x31, 0x35, 0x3b};
    QColor axisX{0xc0, 0x4b, 0x4b};
    QColor axisY{0x4b, 0xa0, 0x5a};

    QColor underConstrained{0x4d, 0xa3, 0xff};  // blue: free geometry
    QColor fullyConstrained{0xe8, 0xe8, 0xe8};  // white: fully defined
    QColor conflict{0xff, 0x55, 0x55};
    QColor dimension{0xff, 0xb3, 0x47};         // amber
    QColor construction{0x8a, 0x8f, 0x98};
    QColor selection{0xff, 0xd2, 0x4d};
    QColor hover{0x9f, 0xd0, 0xff};
};

const CanvasColors& canvas();

// Applies the Fusion style and a dark palette to the whole application.
void applyDark(QApplication& app);

}  // namespace theme
