#pragma once

#include "Style.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <map>
#include <stdexcept>
#include <string>
#include <variant>
#include <vector>

namespace core
{

// Entities and constraints share one id space; 0 is never a valid id.
using Id = std::uint32_t;
inline constexpr Id kNoId = 0;

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

enum class EntityKind
{
    Point,
    Line,
    Circle,
    Arc,
};

struct Entity
{
    Id id = kNoId;
    std::variant<Point, Line, Circle, Arc> geom;
    bool construction = false;  // reference geometry, not part of the drawing
    bool fixed = false;         // locked in place (Fusion's "Fix")

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
};

// A colour filling a closed area of the drawing, as a paint bucket would. The area isn't stored:
// it's found from the geometry (see core/Regions.h), first as the one outlined by `boundary`,
// else the one around `seed`. So a fill follows its outline when the geometry moves, and comes
// back if its outline is broken and then mended.
struct Fill
{
    Id id = kNoId;
    Color color;
    Point seed;                // a point inside the area
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
    std::size_t layoutKey = 0;  // (not saved) what the shape was last made from, to skip remaking
    bool hasColor = false;
    Color color;  // when set: filled in this colour in the drawing (else like plain geometry)

    // The letters: closed contours in text space (mm; x along the baseline from the anchor, y
    // up), filled by the non-zero winding rule. Made by the app from the settings above.
    std::vector<std::vector<Point>> shape;
};

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
    Id m_nextId = 1;
    Id m_origin = kNoId;
    std::array<Id, 2> m_axes{kNoId, kNoId};
    std::array<Id, 2> m_axisEnds{kNoId, kNoId};
};

}  // namespace core
