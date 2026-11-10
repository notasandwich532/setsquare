#pragma once

#include <core/Sketch.h>

#include <QStringList>

// Letter shapes for texts (core::Text::shape), made from the installed fonts.
namespace textshapes
{

// The outline of `text` with its settings (font, bold/italic, height, outline mode, minimum
// thickness), in text space: mm, x along the baseline from the anchor, y up.
// Text along a path needs the sketch (for its curve); without one it's laid out in lines.
std::vector<std::vector<core::Point>> make(const core::Text& text, const core::Sketch* sketch = nullptr);
// The same as the font's lines and curves (core::Text::letters).
std::vector<core::TextContour> makeLetters(const core::Text& text, const core::Sketch* sketch = nullptr);

// How tall a text is in a box `width` wide (mm): from its first line's capitals to its last
// line's descenders, its lines wrapped if it wraps.
double wrappedHeight(const core::Text& text, double width);

// A box text that wraps and grows: its box made as tall as the text, the side away from where
// it's aligned moved (both halfway for the middle). False (and why) if that side can't move
// there; the sketch is then as it was.
bool fitBox(core::Sketch& sketch, core::Id text, std::string* why = nullptr);
// Every such text's box fitted where it can be; true if any changed.
bool fitBoxes(core::Sketch& sketch);

// Makes the shape afresh into `text`.
void update(core::Text& text, const core::Sketch* sketch = nullptr);

// Remakes the shape of each text along a path, or in a box, whose curve or box has changed since
// (after a solve, or as a drag goes).
void refreshPathTexts(core::Sketch& sketch);

// The installed font families whose names contain `search` (any case), sorted; all of them if
// it's empty.
QStringList families(const QString& search = {});

// Without a window system (the offscreen platform: drawapp-cli, tests), Qt only sees the fonts in
// one folder; this adds the user's own installed fonts too (on Windows, those installed "for me
// only"). Call once, after the application object is made.
void addUserFonts();

// A font family to start new text with (installed on this machine).
QString defaultFamily();

}  // namespace textshapes
