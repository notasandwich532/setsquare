#pragma once

#include "Style.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <stdexcept>
#include <string>
#include <variant>
#include <vector>

namespace core
{

class SketchError : public std::runtime_error
{
public:
    using std::runtime_error::runtime_error;
};

// ---------------------------------------------------------------------------------------------
// Geometry
// ---------------------------------------------------------------------------------------------

struct Point
{
    double x = 0.0;
    double y = 0.0;
};

struct Line
{
    Id p1 = kNoId;
    Id p2 = kNoId;
};

struct Circle
{
    Id center = kNoId;
    double radius = 1.0;
};

// Counter-clockwise from start to end. Radius and angles are derived from the three points;
// the solver keeps start and end on the circle through them.
struct Arc
{
    Id center = kNoId;
    Id start = kNoId;
    Id end = kNoId;
};

// An ellipse: its centre, the end of one semi-axis (`major`, which sets its direction) and the
// end of the other (`minor`), which the solver keeps at right angles to it. (Either can be the
// longer: they're named for the order they're drawn in.)
struct Ellipse
{
    Id center = kNoId;
    Id major = kNoId;
    Id minor = kNoId;
};

// Part of an ellipse, counter-clockwise from start to end (both kept on it by the solver).
struct EllipseArc
{
    Id center = kNoId;
    Id major = kNoId;
    Id minor = kNoId;
    Id start = kNoId;
    Id end = kNoId;
};

enum class SplineKind
{
    Fit,      // passes through its points (a centripetal Catmull-Rom curve)
    Control,  // pulled towards its points, through the first and last (a cubic B-spline)
};
const char* toString(SplineKind kind);
bool splineKindFromString(const std::string& name, SplineKind& out);

// A smooth curve defined by its points (at least two; three for a closed one). A piece of one
// (left by a trim) keeps all of them, so it keeps its shape, and runs from `start` to `end`:
// points the solver keeps on the whole curve. Without them it's the whole curve.
struct Spline
{
    std::vector<Id> points;
    SplineKind kind = SplineKind::Fit;
    bool closed = false;
    Id start = kNoId;
    Id end = kNoId;

    bool isPiece() const { return start != kNoId && end != kNoId; }
};

enum class EntityKind
{
    Point,
    Line,
    Circle,
    Arc,
    Ellipse,
    EllipseArc,
    Spline,
};

// A part of the drawing edited on its own: its geometry, constraints and text show (and can be
// changed) while it's the active sublayer; the others show faded, as references that can still be
// snapped to and constrained against. All of them are solved together, so they can depend on each
// other, and colours apply to them all as one (fills find their areas across sublayers).
struct Sublayer
{
    Id id = kNoId;  // its own id space (not the items')
    std::string name;
    bool visible = true;
    Id layer = 1;  // the layer it's part of
};

// A sketch of its own, drawn over the layers below it (the first in the list is the top). Its
// geometry is solved, constrained and coloured apart from the other layers': nothing in it may
// refer to another layer's geometry, except through projections (Project), linked copies that
// follow the original but are fixed here. Its sublayers are edited one at a time.
struct Layer
{
    Id id = kNoId;  // its own id space
    std::string name;
    bool visible = true;
    Id activeSublayer = kNoId;  // the sublayer last edited in it
};

// Where the sketch origin is on the page.
enum class PageOrigin
{
    BottomLeft,
    Center,
    TopLeft,
};
const char* toString(PageOrigin origin);  // "bottom_left", "center", "top_left"
bool pageOriginFromString(const std::string& name, PageOrigin& out);

// The sheet the drawing is on (optional): shown on the canvas, and what the SVG export is the
// size of. Millimetres; width and height as it's turned (landscape: width > height).
struct Page
{
    std::string size = "A4";  // a standard size's name (see pageSizes()), or "custom"
    double width = 210.0;
    double height = 297.0;
    PageOrigin origin = PageOrigin::BottomLeft;
    bool hasColor = false;
    Color color{0xff, 0xff, 0xff, 255};  // the paper's colour, when it has one

    bool landscape() const { return width > height; }
    std::array<double, 4> rect() const;  // in sketch coordinates: x0, y0, x1, y1
    bool operator==(const Page&) const = default;
};

struct PageSize
{
    const char* name;
    double width, height;  // portrait, mm
};
const std::vector<PageSize>& pageSizes();  // A0..A5, Letter, Legal, Tabloid
// A standard size by name (either way round); false if there's none.
bool pageSizeByName(const std::string& name, double& width, double& height);

struct Entity
{
    Id id = kNoId;
    std::variant<Point, Line, Circle, Arc, Ellipse, EllipseArc, Spline> geom;
    bool construction = false;  // reference geometry, not part of the drawing
    bool fixed = false;         // locked in place (Fusion's "Fix")
    Id sublayer = kNoId;        // which sublayer it's in (none: the origin and axes)
    // A projection: a linked copy of this entity from another layer, kept where the original is
    // (and fixed). If the original is deleted, the copy stays put, unlinked.
    Id projectedFrom = kNoId;

    EntityKind kind() const { return static_cast<EntityKind>(geom.index()); }
};

const char* toString(EntityKind kind);

// ---------------------------------------------------------------------------------------------
// Constraints and dimensions
// ---------------------------------------------------------------------------------------------

enum class ConstraintType
{
    // Geometric
    Coincident,     // (point, point) or (point, curve)
    Horizontal,     // (line) or (point, point)
    Vertical,       // (line) or (point, point)
    Parallel,       // (line, line)
    Perpendicular,  // (line, line)
    Tangent,        // (line, circle|arc) or (circle|arc, circle|arc)
    Equal,          // (line, line) lengths, or (circle|arc, circle|arc) radii
    Midpoint,       // (point, line)
    Concentric,     // (circle|arc, circle|arc)
    Symmetric,      // (point, point, line)
    Collinear,      // (line, line)
    Ratio,          // (line, line) lengths, or (circle|arc, circle|arc) radii: the first is 
                    // alue times the second (value > 0; editable like a dimension's)

    // Dimensions (driving, carry a value)
    Distance,            // (point, point), (point, line), or (line) for its length
    HorizontalDistance,  // (point, point) or (line)
    VerticalDistance,    // (point, point) or (line)
    Radius,              // (circle|arc)
    Diameter,            // (circle|arc)
    Angle,               // (line, line), degrees, measured from the first to the second

    // Features: generated geometry kept tied to its source by one constraint. They carry a
    // value (edited like a dimension) and further settings in Constraint::params. See
    // core/Features.h for the layout of the references.
    Offset,           // (sources..., offsets...): value = distance
    LinearPattern,    // (sources..., copies...): value = spacing;
                      // params = {count, angle, count2, spacing2, symmetric, symmetric2}
    CircularPattern,  // (centre point, sources..., copies...): value = total angle; params = {count}
    Divide,           // (line, points...): value = the number of points;
                      // params = {by ratio, reversed, parts...} (see feature::DivideParams)
};

const char* toString(ConstraintType type);
bool isDimension(ConstraintType type);  // a dimension drawn on the canvas
bool isFeature(ConstraintType type);    // offset, pattern or divide
bool hasValue(ConstraintType type);     // dimensions, features and ratios
bool constraintTypeFromString(const std::string& name, ConstraintType& out);

struct Constraint
{
    Id id = kNoId;
    ConstraintType type = ConstraintType::Coincident;
    std::vector<Id> refs;
    double value = 0.0;  // dimensions only; lengths are positive, angles in degrees

    // Dimensions only: where the label sits, as an offset from the dimension's anchor (see
    // layoutDimension). Zero means "choose a default".
    double labelX = 0.0;
    double labelY = 0.0;

    // Angle only: which of the four angles between the lines is meant. The angle is measured
    // between rays along each line; bit 0 / bit 1 reverse the first / second line's ray
    // (normally p1 -> p2). The UI picks these from where the label is placed.
    std::uint8_t flip = 0;

    // Features only: settings beyond the value (see ConstraintType).
    std::vector<double> params;

    Id sublayer = kNoId;  // the sublayer it was made in (its symbol shows while that's active)
};

// A colour filling a closed area of the drawing, as a paint bucket would. The area isn't stored:
// it's found from the geometry (see core/Regions.h), first as the one outlined by `boundary`,
// else the one around `seed`. So a fill follows its outline when the geometry moves, and comes
// back if its outline is broken and then mended.
struct Fill
{
    Id id = kNoId;
    Color color;
    Gradient gradient;         // when active: filled with this rather than `color`
    Point seed;                // a point inside the area
    Id layer = kNoId;          // the layer whose geometry outlines it
    std::vector<Id> boundary;  // the curves around it when it was last found (sorted)
};

// Text in the drawing, anchored at a point (the start of its baseline), so it can be constrained
// and dragged like any other geometry. The letter shapes come from the font through the app (they
// need Qt) and are kept here as outlines, so the core can draw, save and export text without
// fonts, and a file opens with its text where the font isn't installed.
enum class TextOutline
{
    AsDesigned,  // the font's own contours, overlaps and all
    Merged,      // overlapping strokes joined into one outline (holes like an "o" kept)
    Silhouette,  // only the outside edge: nothing inside the letters at all
};
const char* toString(TextOutline outline);
bool textOutlineFromString(const std::string& name, TextOutline& out);

// Which part of the text sits on its anchor. Along a path: horizontally, where on the curve it
// goes (its start, middle or end); vertically, which side of the curve (letters standing on it,
// centred on it, or hanging below it).
enum class TextAlign
{
    Left,
    Center,
    Right,
};
enum class TextVAlign
{
    Baseline,  // the first line's baseline (along a path: standing on it)
    Top,       // the top of the first line's capitals (along a path: hanging below it)
    Middle,    // halfway between those (along a path: centred on it)
    Bottom,    // the last line's baseline
};
const char* toString(TextAlign align);
const char* toString(TextVAlign align);
bool textAlignFromString(const std::string& name, TextAlign& out);
bool textVAlignFromString(const std::string& name, TextVAlign& out);

struct Text
{
    Id id = kNoId;
    Id anchor = kNoId;  // a point: the left end of the baseline
    std::string text;
    std::string font;  // family
    bool bold = false;
    bool italic = false;
    double height = 10.0;  // capital letter height, mm
    double angle = 0.0;    // degrees, baseline direction from +x
    TextOutline outline = TextOutline::AsDesigned;
    double minThickness = 0.0;  // mm; 0 = as the font has it
    double spacing = 0.0;       // extra space between letters, % of the font size (may be negative)
    TextAlign align = TextAlign::Left;
    TextVAlign valign = TextVAlign::Baseline;
    bool flipH = false;  // mirrored left to right (along a path: running the other way)
    bool flipV = false;  // mirrored top to bottom (along a path: on the other side of it)
    // Text along a curve (a line, arc or circle) instead of in lines: it follows the curve as it
    // moves. Its anchor is then the curve's first point, and the shape is remade as it changes.
    Id path = kNoId;
    // Text in a box (a construction rectangle): anchor is its corner at the start of the bottom
    // side, boxSide the other end of that side, boxCorner the corner opposite the anchor. The
    // text is aligned in it (left/centre/right; top/middle/bottom), turned with it, and remade as
    // it changes.
    Id boxSide = kNoId;
    Id boxCorner = kNoId;
    Id sublayer = kNoId;
    bool inBox() const { return boxSide != kNoId && boxCorner != kNoId; }
    std::size_t layoutKey = 0;  // (not saved) what the shape was last made from, to skip remaking
    bool hasColor = false;
    Color color;  // when set: filled in this colour in the drawing (else like plain geometry)
    Gradient gradient;  // when active (hasColor too): filled with this rather than `color`

    // The letters: closed contours in text space (mm; x along the baseline from the anchor, y
    // up), filled by the non-zero winding rule. Made by the app from the settings above.
    std::vector<std::vector<Point>> shape;
};

// The points a text sits on: its anchor, and a box text's box corners (all four). Moving them all
// moves the text; deleting the text deletes its box.
std::vector<Id> textPoints(const class Sketch& sketch, const Text& text);

// The world position of a point in a text's own space.
Point textToWorld(const Point& anchor, double angleDeg, const Point& local);

// ---------------------------------------------------------------------------------------------
// Sketch: owns geometry and constraints. Solving lives in Solver.
// ---------------------------------------------------------------------------------------------

class Sketch
{
public:
    // --- geometry ---
    Id addPoint(double x, double y);
    Id addLine(Id p1, Id p2);
    Id addLine(double x1, double y1, double x2, double y2);  // creates both endpoints
    Id addCircle(Id center, double radius);
    Id addCircle(double cx, double cy, double radius);
    Id addArc(Id center, Id start, Id end);
    Id addArc(double cx, double cy, double radius, double startDeg, double endDeg);
    Id addEllipse(Id center, Id major, Id minor);
    Id addEllipseArc(Id center, Id major, Id minor, Id start, Id end);
    Id addSpline(const Spline& spline);  // a whole curve or a piece (see Spline)

    // --- constraints ---
    // Validates the references for the type; throws SketchError when they don't fit.
    Id addConstraint(ConstraintType type, std::vector<Id> refs, double value = 0.0, std::vector<double> params = {});
    void setConstraintValue(Id constraint, double value);
    void setConstraintParams(Id constraint, std::vector<double> params);  // features
    void setConstraintLabel(Id constraint, double dx, double dy);
    void setConstraintFlip(Id constraint, std::uint8_t flip);  // see Constraint::flip

    // Whether addConstraint would accept these references (value checks use a valid
    // placeholder unless one is given). On failure, *why receives the reason.
    bool canAddConstraint(ConstraintType type, std::vector<Id> refs, std::string* why = nullptr) const;

    // Removes an entity or constraint. Removing geometry also removes everything that depends
    // on it (curves using a deleted point, constraints referencing deleted ids) and the
    // endpoints of deleted curves that nothing else uses. The origin can't be removed.
    void remove(Id id);
    void clear();

    // --- low-level editing (used by trim, fillet, ...) ---
    // Makes `curve` use `newPoint` where it used `oldPoint` (line ends, arc start/end/centre).
    void rebindPoint(Id curve, Id oldPoint, Id newPoint);
    // Whether any curve is built on this point.
    bool isPointUsed(Id point) const;
    // Removes a point if no curve uses it (and it isn't the origin/axes); returns whether it did.
    bool removeIfUnused(Id point);
    // Moves every constraint reference from one entity to another (e.g. a circle that became an
    // arc). Throws SketchError, changing nothing, if a constraint wouldn't be valid afterwards.
    void retarget(Id from, Id to);

    // --- appearance (presentation only; the solver ignores it) ---
    // A curve's stroke in the drawing. Curves without one are drawn as plain sketch geometry.
    // Strokes go with their curve: deleting the curve deletes its stroke.
    void setStroke(Id curve, const StrokeStyle& style);  // throws unless `curve` is a user curve
    void clearStroke(Id curve);
    const StrokeStyle* stroke(Id curve) const;  // nullptr if it has none
    const std::map<Id, StrokeStyle>& strokes() const { return m_strokes; }
    void copyStroke(Id from, Id to);  // gives `to` the stroke of `from` (if it has one)

    // Fills share the id space with entities and constraints; later ones draw on top. Deleting
    // curves doesn't delete fills (they wait to be found again); remove() deletes a fill.
    Id addFill(const Fill& fill);  // fill.id is ignored; returns the new id
    void setFill(const Fill& fill);  // replaces an existing fill (by id)
    void insertFill(const Fill& fill);  // with its id (loading)
    bool hasFill(Id id) const { return m_fills.contains(id); }
    const Fill& fill(Id id) const;
    const std::map<Id, Fill>& fills() const { return m_fills; }

    // Texts share the id space too. Removing a text's anchor point removes the text; remove()
    // deletes a text (and its anchor, if nothing else uses it).
    Id addText(const Text& text);     // text.id is ignored; returns the new id
    void setText(const Text& text);   // replaces an existing text (by id)
    void insertText(const Text& text);  // with its id (loading)
    bool hasText(Id id) const { return m_texts.contains(id); }
    const Text& text(Id id) const;
    const std::map<Id, Text>& texts() const { return m_texts; }

    // --- sublayers ---
    // Always at least one. New items go into the active one.
    const std::vector<Sublayer>& sublayers() const { return m_sublayers; }
    bool hasSublayer(Id id) const;
    const Sublayer& sublayer(Id id) const;
    Id activeSublayer() const { return m_activeSublayer; }
    void setActiveSublayer(Id id);
    Id addSublayer(const std::string& name);  // after the others; returns its id
    void renameSublayer(Id id, const std::string& name);
    void setSublayerVisible(Id id, bool visible);
    void moveSublayer(Id id, size_t index);  // to that place in the order
    // Deletes a sublayer and everything in it (not the last one).
    void removeSublayer(Id id);
    // Which sublayer an item (entity, constraint or text) is in; kNoId for the origin/axes, fills
    // and anything unknown.
    Id sublayerOf(Id item) const;
    void setSublayerOf(Id item, Id sublayer);
    // Whether an item is in a sublayer that's shown (the origin and axes always are).
    bool isShown(Id item) const;
    // Loading: replaces the list (and the active one).
    void setSublayers(std::vector<Sublayer> sublayers, Id active);

    // --- layers ---
    // Always at least one, each with at least one sublayer. The first is drawn on top. The active
    // layer is the active sublayer's; new sublayers go into it.
    const std::vector<Layer>& layers() const { return m_layers; }
    bool hasLayer(Id id) const;
    const Layer& layer(Id id) const;
    Id activeLayer() const;
    void setActiveLayer(Id id);  // (its last active sublayer becomes the active one)
    // A new layer (on top), with one sublayer; returns its id. Not made active.
    Id addLayer(const std::string& name, const std::string& sublayerName);
    void renameLayer(Id id, const std::string& name);
    void setLayerVisible(Id id, bool visible);
    void moveLayer(Id id, size_t index);
    // Deletes a layer, its sublayers and everything in them, and its fills (not the last layer).
    // Projections of its geometry in other layers stay, unlinked.
    void removeLayer(Id id);
    std::vector<Id> sublayersOf(Id layer) const;  // in order
    // The layer an item is in (entity, constraint, text: through its sublayer; fill: its own);
    // kNoId for the origin and axes (which belong to every layer) and anything unknown.
    Id layerOf(Id item) const;
    Id layerOfSublayer(Id sublayer) const;
    // Loading: replaces the list (sublayers must already name these layers).
    void setLayers(std::vector<Layer> layers);

    // --- page ---
    const std::optional<Page>& page() const { return m_page; }
    void setPage(std::optional<Page> page);  // throws unless its size is positive

    // --- projections ---
    // Copies `source` (an entity in another layer, with the points it's built on) into the active
    // layer, as fixed construction geometry linked to it; returns the copy. Already projected
    // here: the existing copy. Refuses what would make layers depend on each other in a circle.
    Id project(Id source);
    bool isProjection(Id id) const;  // projected (linked or not)
    bool isLinked(Id id) const;      // projected, and the original is still there
    // Makes a projection ordinary geometry (no longer fixed or following the original), with the
    // points it's built on.
    void breakLink(Id id);
    // Moves projections (in `layer`, or everywhere) to where their originals are now; returns
    // whether anything moved.
    bool syncProjections(Id layer = kNoId);
    // Whether `layer` uses geometry of `other` (directly or through other layers).
    bool dependsOn(Id layer, Id other) const;

    // --- origin and axes ---
    // A fixed point at (0, 0) plus fixed X and Y axis lines through it, which geometry can snap
    // to and be constrained/dimensioned against (e.g. an angle to the X axis). Created on
    // demand; they can't be moved, unfixed or deleted. Each axis is an ordinary line from the
    // origin to a hidden fixed point at (1, 0) / (0, 1); the UI draws it as infinite.
    Id ensureOrigin();  // also creates the axes
    Id origin() const { return m_origin; }
    Id xAxis() const { return m_axes[0]; }
    Id yAxis() const { return m_axes[1]; }
    bool isOrigin(Id id) const { return id != kNoId && id == m_origin; }
    bool isAxis(Id id) const { return id != kNoId && (id == m_axes[0] || id == m_axes[1]); }
    // Origin, axes and the axes' hidden end points: permanent, not user geometry.
    bool isReference(Id id) const;
    // The axes' hidden end points: never shown or picked.
    bool isHiddenReference(Id id) const { return id != kNoId && (id == m_axisEnds[0] || id == m_axisEnds[1]); }

    // --- loading ---
    // Insert items with known ids (e.g. from a file). Points must go in before the curves that
    // use them, and geometry before constraints. Throws SketchError on bad references.
    void insertEntity(const Entity& entity);
    void insertConstraint(const Constraint& constraint);
    void setOrigin(Id point);           // marks an existing point as the origin
    void setAxes(Id xAxis, Id yAxis);   // marks existing lines (from the origin) as the axes

    // --- lookup ---
    bool hasEntity(Id id) const { return m_entities.contains(id); }
    bool hasConstraint(Id id) const { return m_constraints.contains(id); }
    const Entity& entity(Id id) const;
    Entity& entity(Id id);
    const Constraint& constraint(Id id) const;

    const Point& point(Id id) const;
    Point& point(Id id);
    const Line& line(Id id) const;
    const Circle& circle(Id id) const;
    Circle& circle(Id id);
    const Arc& arc(Id id) const;
    const Ellipse& ellipse(Id id) const;
    const EllipseArc& ellipseArc(Id id) const;
    const Spline& spline(Id id) const;
    // Ellipse or elliptical arc: its centre, major and minor points.
    bool isElliptic(Id id) const;
    std::array<Id, 3> ellipseAxes(Id id) const;  // centre, major, minor

    // The largest id handed out so far; anything added later has a higher id.
    Id highestId() const { return m_nextId - 1; }

    const std::map<Id, Entity>& entities() const { return m_entities; }
    const std::map<Id, Constraint>& constraints() const { return m_constraints; }

    // Point ids that define an entity (the point itself, a line's endpoints, a circle's centre,
    // an arc's centre/start/end).
    std::vector<Id> definingPoints(Id entity) const;

    // Ids of constraints that reference the given entity.
    std::vector<Id> constraintsOn(Id entity) const;

    // Arc helpers (derived from its points).
    double arcRadius(Id arc) const;

private:
    Id nextId() { return m_nextId++; }
    EntityKind kindOf(Id id) const;
    bool isCurve(Id id) const;
    bool isRound(Id id) const;  // circle or arc
    void normalize(ConstraintType type, std::vector<Id>& refs) const;
    void validate(ConstraintType type, const std::vector<Id>& refs, double value,
                  const std::vector<double>& params = {}) const;
    void validateFeature(ConstraintType type, const std::vector<Id>& refs, double value,
                         const std::vector<double>& params) const;
    bool referencesPoint(const Entity& e, Id point) const;
    void checkGeometry(const Entity& e) const;  // a curve's points exist and are distinct points

    std::map<Id, Entity> m_entities;
    std::map<Id, Constraint> m_constraints;
    std::map<Id, StrokeStyle> m_strokes;
    std::map<Id, Fill> m_fills;
    std::map<Id, Text> m_texts;
    // Throws unless every one of these items is in `layer` (or is the origin/axes).
    void requireLayer(const std::vector<Id>& items, Id layer) const;

    std::vector<Sublayer> m_sublayers{{1, "Sublayer 1", true, 1}};
    Id m_activeSublayer = 1;
    std::vector<Layer> m_layers{{1, "Layer 1", true, 1}};
    std::optional<Page> m_page;
    Id m_nextId = 1;
    Id m_origin = kNoId;
    std::array<Id, 2> m_axes{kNoId, kNoId};
    std::array<Id, 2> m_axisEnds{kNoId, kNoId};
};

}  // namespace core
