#pragma once

#include <core/Geometry.h>
#include <core/Sketch.h>

#include <QByteArray>
#include <QString>

#include <array>
#include <map>
#include <optional>
#include <string>
#include <vector>

// Importing SVG and DXF drawings: read into shapes (in millimetres, y up), then built into the
// sketch as geometry with strokes, fills and gradients, one group per layer of the file (see
// core/Groups.h); pictures come in as reference images. What can't be brought in (clip paths,
// masks, ...) is skipped and reported.

namespace importer
{

// One piece of an outline, in drawing coordinates (mm, y up).
struct Segment
{
    enum class Kind
    {
        Line,     // p[0] to p[1]
        Cubic,    // a cubic Bezier p[0]..p[3]
        Ellipse,  // round c: radii rx (along phi) and ry, from parameter t0 sweeping dt (> 0:
                  // counter-clockwise); |dt| >= 2 pi: the whole ellipse. rx == ry: a circle.
    };
    Kind kind = Kind::Line;
    core::Vec2 p[4];
    core::Vec2 c;
    double rx = 0, ry = 0, phi = 0, t0 = 0, dt = 0;

    bool whole() const;
    core::Vec2 start() const;
    core::Vec2 end() const;
    std::vector<core::Vec2> sample(int perPiece = 16) const;  // from start to end
};

struct Contour
{
    std::vector<Segment> segments;
    bool closed = false;
};

// A gradient as the file places it, in drawing coordinates. Built as sketch geometry for it to
// follow (construction points; a construction circle or ellipse), held in the layer's group.
struct GradientDef
{
    core::GradientKind kind = core::GradientKind::Linear;
    core::GradientSpread spread = core::GradientSpread::Pad;
    std::vector<core::GradientStop> stops;
    core::Vec2 start, end;  // linear: offset 0 to offset 1 (the colours change straight across)
    core::Vec2 center;      // radial: offset 1 on the ellipse round `center`, radii rx (along phi), ry
    double rx = 0, ry = 0, phi = 0;
    std::optional<core::Vec2> focal;  // radial: offset 0, when it isn't the centre
    bool operator==(const GradientDef& o) const;
};

struct Style
{
    std::optional<core::Color> stroke;  // none: no line
    double strokeWidth = 0.25;          // mm
    core::Dash dash = core::Dash::Solid;
    std::optional<core::Color> fill;
    // Painted with these where given (`fill` and `stroke` are then their average colour).
    std::optional<GradientDef> fillGradient;
    std::optional<GradientDef> strokeGradient;
    bool evenOdd = false;       // the fill rule (else nonzero)
    bool construction = false;  // reference geometry (e.g. our own exports' Construction layer)
};

struct Shape
{
    std::vector<Contour> contours;
    Style style;
    std::string layer;  // the file's layer it's on
};

// A picture in the file: it comes in as a reference image (see core::Image).
struct ImageDef
{
    core::Image image;                  // its picture, placed (centre, width, angle, flip) in drawing coordinates
    std::string layer;                  // the file's layer it's on
    std::array<core::Vec2, 4> corners;  // where it is (for the drawing's extent)
};

// Text in the file: it comes in as text (see core::Text), in the same font where that's installed.
struct TextDef
{
    core::Text text;  // its words and settings: font, height (of capitals, mm), angle, alignment, colour
    core::Vec2 at;    // its anchor, in drawing coordinates
    std::string layer;
};

// A file, read.
struct Drawing
{
    std::vector<Shape> shapes;
    std::vector<ImageDef> images;
    std::vector<TextDef> texts;
    std::vector<std::string> layers;  // the file's layers, in order (bottom first)
    // The file's own canvas (an SVG's page), in drawing coordinates: x0, y0, x1, y1. None for DXF.
    std::optional<std::array<double, 4>> canvas;
    std::map<std::string, int> skipped;  // what couldn't be brought in, and how many
    std::vector<std::string> notes;      // what was brought in only approximately
    std::string units;                   // how its units were read, e.g. "mm (from the file)"

    bool bounds(double& x0, double& y0, double& x1, double& y1) const;
};

// How curves the sketch has no exact match for come in: Bezier curves (SVG paths, DXF splines).
enum class Curves
{
    Exact,   // a control-point spline per Bezier piece: exactly as drawn, but many pieces
    Smooth,  // fit splines through points along runs of smooth curve: fewer pieces, very close
};

// Reading. Throw core::SketchError if the file can't be read at all.
// SVG lengths without units are pixels, `pxPerInch` of them to the inch (CSS's 96; some programs
// use 72 or 90). DXF: `mmPerUnit` if given, else from the file ($INSUNITS), else millimetres.
// (Curves come in as Beziers, made exact or smooth when built.) Pictures the file links to (rather
// than holding) are looked for by their path as given, then by name in `folder` (the file's own).
Drawing readSvg(const QByteArray& data, double pxPerInch = 96.0, const QString& folder = {});
Drawing readDxf(const QByteArray& data, std::optional<double> mmPerUnit = std::nullopt, const QString& folder = {});
Drawing read(const QString& path, double pxPerInch = 96.0, std::optional<double> mmPerUnit = std::nullopt);

enum class Into
{
    Sublayer,     // the sublayer being edited
    NewSublayer,  // a new sublayer of the active layer (one per file layer, if kept)
    NewLayer,     // a new layer on top (with a sublayer per file layer, if kept)
};

enum class Placement
{
    AsInFile,  // the file's coordinates (an SVG's page: its bottom-left corner on the origin)
    Center,    // its middle at `at`
    Page,      // an SVG's page on the sketch's page (its bottom-left corner); else as in the file
};

struct Options
{
    Into into = Into::NewSublayer;
    bool keepLayers = true;  // the file's layers become sublayers (else all in one)
    Placement placement = Placement::AsInFile;
    core::Vec2 at;           // Placement::Center
    Curves curves = Curves::Exact;
    std::string name;        // for a new sublayer / layer (the file's name)
};

struct Report
{
    int curves = 0;
    int fills = 0;
    int gradients = 0;
    int images = 0;
    int texts = 0;
    int groups = 0;
    std::vector<std::string> sublayers;  // where it went
    std::map<std::string, int> skipped;
    std::vector<std::string> notes;
    QString summary() const;  // e.g. "312 curves, 4 fills in 2 sublayers; skipped 3 texts"
};

// Builds a drawing into the sketch (an undoable edit's working copy).
Report build(core::Sketch& sketch, const Drawing& drawing, const Options& options);

}  // namespace importer
