#pragma once

#include <core/Sketch.h>

#include <QStringList>

// Letter shapes for texts (core::Text::shape), made from the installed fonts.
namespace textshapes
{

// The outline of `text` with its settings (font, bold/italic, height, outline mode, minimum
// thickness), in text space: mm, x along the baseline from the anchor, y up.
std::vector<std::vector<core::Point>> make(const core::Text& text);

// Makes the shape afresh into `text`.
void update(core::Text& text);

// The installed font families whose names contain `search` (any case), sorted; all of them if
// it's empty.
QStringList families(const QString& search = {});

// A font family to start new text with (installed on this machine).
QString defaultFamily();

}  // namespace textshapes
