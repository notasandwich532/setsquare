#pragma once

#include <array>
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

    // Dimensions (driving, carry a value)
    Distance,            // (point, point), (point, line), or (line) for its length
    HorizontalDistance,  // (point, point) or (line)
    VerticalDistance,    // (point, point) or (line)
    Radius,              // (circle|arc)
    Diameter,            // (circle|arc)
    Angle,               // (line, line), degrees, measured from the first to the second
};

const char* toString(ConstraintType type);
bool isDimension(ConstraintType type);
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
};

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
    Id addConstraint(ConstraintType type, std::vector<Id> refs, double value = 0.0);
    void setConstraintValue(Id constraint, double value);
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
    void validate(ConstraintType type, const std::vector<Id>& refs, double value) const;
    bool referencesPoint(const Entity& e, Id point) const;
    void checkGeometry(const Entity& e) const;  // a curve's points exist and are distinct points

    std::map<Id, Entity> m_entities;
    std::map<Id, Constraint> m_constraints;
    Id m_nextId = 1;
    Id m_origin = kNoId;
    std::array<Id, 2> m_axes{kNoId, kNoId};
    std::array<Id, 2> m_axisEnds{kNoId, kNoId};
};

}  // namespace core
