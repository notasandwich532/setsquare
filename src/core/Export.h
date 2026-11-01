#pragma once

#include "Sketch.h"

#include <string>

// Writing drawings out for other programs.

namespace core::exporter
{

struct SvgOptions
{
    bool fills = true;
    bool undecorated = true;    // curves without a stroke, drawn in `plain`
    bool construction = false;  // construction geometry, dashed grey
    StrokeStyle plain{{0, 0, 0, 255}, 0.25, Dash::Solid};
    double margin = 2.0;        // mm around the drawing
};

// SVG of the drawing as Presentation view shows it: fills (with their holes) under strokes, in
// millimetres (width/height in mm, a viewBox in mm, so it prints and cuts at true size), with
// y pointing up as in the sketch. Lines, circles and arcs stay exact; fills follow arcs closely.
// Groups are Inkscape layers: Fills, Strokes, Geometry, Construction. Throws SketchError if
// there's nothing to export.
std::string toSvg(const Sketch& sketch, const SvgOptions& options = {});

}  // namespace core::exporter
