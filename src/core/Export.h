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

struct DxfOptions
{
    bool styles = true;         // strokes keep their colour and line style (else all plain)
    bool construction = false;  // construction geometry, on its own layer (switched off)
};

// DXF (R12, the version every CAD/CAM program reads) of the geometry: exact LINE, CIRCLE and ARC
// entities in millimetres, y up. Layers: GEOMETRY (curves without a stroke), STROKES (with one,
// each in its colour, matched to the nearest AutoCAD colour, and line style), CONSTRUCTION
// (optional, off). R12 has no fills, true colours or line weights, so those aren't written.
// Throws SketchError if there's nothing to export.
std::string toDxf(const Sketch& sketch, const DxfOptions& options = {});

// The AutoCAD colour index (1-255) nearest a colour. For tests and the DXF writer.
int nearestAci(const Color& color);

}  // namespace core::exporter
