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

struct Constraint
{
    Id id = kNoId;
    ConstraintType type = ConstraintType::Coincident;
    std::vector<Id> refs;
    double value = 0.0;  // dimensions only; lengths are positive, angles in degrees
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

    // Removes an entity or constraint. Removing geometry also removes everything that depends
    // on it (curves using a deleted point, constraints referencing deleted ids) and the
    // endpoints of deleted curves that nothing else uses.
    void remove(Id id);
    void clear();

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
    void validate(ConstraintType type, const std::vector<Id>& refs, double value) const;
    bool referencesPoint(const Entity& e, Id point) const;

    std::map<Id, Entity> m_entities;
    std::map<Id, Constraint> m_constraints;
    Id m_nextId = 1;
};

}  // namespace core
