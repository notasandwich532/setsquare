#pragma once

#include "Sketch.h"

#include <array>
#include <optional>
#include <string>

// Writing drawings out for other programs.

namespace core::exporter
{

struct SvgOptions
{
    bool fills = true;
    bool undecorated = true;    // curves without a stroke, drawn in `plain`
    bool construction = false;  // construction geometry, dashed grey
    StrokeStyle plain{{0, 0, 0, 255}, 0.25, Dash::Solid, {}};
    double margin = 2.0;        // mm around the drawing
    // With a page set (Sketch::page): the SVG is the page, at its size and in its colour, with
    // what's off it cut off. Off, or without a page: fitted round the drawing, with `margin`.
    bool page = true;

    // For icons: a fixed canvas (sketch x0, y0, x1, y1) instead of fitting the drawing. The SVG
    // is then in canvas units (no mm, no margin), e.g. a 24 x 24 icon grid.
    std::optional<std::array<double, 4>> canvas;

    // Text as its letters' outlines (filled paths: exact everywhere, but no longer text), rather
    // than as text in its font (editable elsewhere; shown in that program's copy of the font).
    // Text that can't be text (mirrored, merged or thickened outlines) is outlines anyway.
    bool textAsOutlines = false;
    // Hatches as their lines, cut to their areas (for plotters and cutters), rather than as
    // <pattern>s (one repeating tile: small, and editable as a pattern elsewhere).
    bool hatchLines = false;
};

// SVG of the drawing as Presentation view shows it: fills (with their holes) under strokes, in
// millimetres (width/height in mm, a viewBox in mm, so it prints and cuts at true size), with
// y pointing up as in the sketch. Lines, circles and arcs stay exact; so do fills' outlines.
// Each layer is an Inkscape layer: its fills (a group), then its sublayers (a group each, by
// name: texts, then lines, then construction). Hatched fills are <pattern>s where the hatch
// repeats in a tile, else their lines. Throws SketchError if there's nothing to export.
std::string toSvg(const Sketch& sketch, const SvgOptions& options = {});

struct DxfOptions
{
    bool styles = true;         // strokes keep their colour, line style and weight (else all plain)
    bool construction = false;  // construction geometry, on its own layer (switched off)
    bool fills = true;          // solid and hatched fills, as HATCH entities
    // Text as its letters' outlines (closed polylines and splines) rather than TEXT in its font.
    // Text that can't be text (mirrored, merged or thickened outlines) is outlines anyway.
    bool textAsOutlines = false;
};

// DXF (AutoCAD 2004, AC1018) of the drawing in millimetres, y up, every curve exact: LINE, CIRCLE,
// ARC, ELLIPSE, SPLINE; TEXT in its font; fills as HATCH (solid, or the hatch's own pattern). Layers:
// GEOMETRY (curves without a stroke), STROKES (with one: its colour, as true colour and the nearest
// AutoCAD colour, line style and weight), TEXT, FILLS, CONSTRUCTION (optional, off).
// Throws SketchError if there's nothing to export.
std::string toDxf(const Sketch& sketch, const DxfOptions& options = {});

// The AutoCAD colour index (1-255) nearest a colour. For tests and the DXF writer.
int nearestAci(const Color& color);
// The colour of an AutoCAD colour index (1-255); black for anything else.
Color aciColor(int index);

}  // namespace core::exporter
