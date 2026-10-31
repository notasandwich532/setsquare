#include "Sketch.h"

#include <algorithm>
#include <cmath>
#include <numbers>
#include <set>
#include <string>
#include <type_traits>

namespace core
{

const char* toString(EntityKind kind)
{
    switch (kind) {
        case EntityKind::Point: return "point";
        case EntityKind::Line: return "line";
        case EntityKind::Circle: return "circle";
        case EntityKind::Arc: return "arc";
    }
    return "?";
}

const char* toString(ConstraintType type)
{
    switch (type) {
        case ConstraintType::Coincident: return "coincident";
        case ConstraintType::Horizontal: return "horizontal";
        case ConstraintType::Vertical: return "vertical";
        case ConstraintType::Parallel: return "parallel";
        case ConstraintType::Perpendicular: return "perpendicular";
        case ConstraintType::Tangent: return "tangent";
        case ConstraintType::Equal: return "equal";
        case ConstraintType::Midpoint: return "midpoint";
        case ConstraintType::Concentric: return "concentric";
        case ConstraintType::Symmetric: return "symmetric";
        case ConstraintType::Distance: return "distance";
        case ConstraintType::HorizontalDistance: return "horizontalDistance";
        case ConstraintType::VerticalDistance: return "verticalDistance";
        case ConstraintType::Radius: return "radius";
        case ConstraintType::Diameter: return "diameter";
        case ConstraintType::Angle: return "angle";
    }
    return "?";
}

bool isDimension(ConstraintType type)
{
    return type >= ConstraintType::Distance;
}

bool constraintTypeFromString(const std::string& name, ConstraintType& out)
{
    for (int i = 0; i <= static_cast<int>(ConstraintType::Angle); ++i) {
        const auto type = static_cast<ConstraintType>(i);
        if (name == toString(type)) {
            out = type;
            return true;
        }
    }
    return false;
}

// ---------------------------------------------------------------------------------------------
// Origin and loading
// ---------------------------------------------------------------------------------------------

Id Sketch::ensureOrigin()
{
    if (m_origin == kNoId) {
        m_origin = addPoint(0.0, 0.0);
        m_entities.at(m_origin).fixed = true;
    }
    return m_origin;
}

void Sketch::setOrigin(Id point)
{
    if (kindOf(point) != EntityKind::Point) {
        throw SketchError("the origin must be a point");
    }
    m_origin = point;
    Entity& e = m_entities.at(point);
    e.fixed = true;
    e.geom = Point{0.0, 0.0};
}

void Sketch::checkGeometry(const Entity& e) const
{
    const std::vector<Id> points = std::visit(
        [](const auto& g) -> std::vector<Id> {
            using T = std::decay_t<decltype(g)>;
            if constexpr (std::is_same_v<T, Line>) {
                return {g.p1, g.p2};
            }
            else if constexpr (std::is_same_v<T, Circle>) {
                return {g.center};
            }
            else if constexpr (std::is_same_v<T, Arc>) {
                return {g.center, g.start, g.end};
            }
            else {
                return {};
            }
        },
        e.geom);
    for (Id p : points) {
        if (!hasEntity(p) || kindOf(p) != EntityKind::Point) {
            throw SketchError(std::string(toString(e.kind())) + " " + std::to_string(e.id) + " refers to missing point "
                              + std::to_string(p));
        }
    }
    if (std::set<Id>(points.begin(), points.end()).size() != points.size()) {
        throw SketchError(std::string(toString(e.kind())) + " " + std::to_string(e.id) + " repeats a point");
    }
    if (const auto* c = std::get_if<Circle>(&e.geom); c && !(c->radius > 0.0)) {
        throw SketchError("circle " + std::to_string(e.id) + " has a non-positive radius");
    }
}

void Sketch::insertEntity(const Entity& e)
{
    if (e.id == kNoId || hasEntity(e.id) || hasConstraint(e.id)) {
        throw SketchError("duplicate or invalid id " + std::to_string(e.id));
    }
    checkGeometry(e);
    m_entities.emplace(e.id, e);
    m_nextId = std::max(m_nextId, e.id + 1);
}

void Sketch::insertConstraint(const Constraint& c)
{
    if (c.id == kNoId || hasEntity(c.id) || hasConstraint(c.id)) {
        throw SketchError("duplicate or invalid id " + std::to_string(c.id));
    }
    std::vector<Id> refs = c.refs;
    normalize(c.type, refs);
    validate(c.type, refs, c.value);
    Constraint stored = c;
    stored.refs = std::move(refs);
    m_constraints.emplace(c.id, std::move(stored));
    m_nextId = std::max(m_nextId, c.id + 1);
}

// ---------------------------------------------------------------------------------------------
// Geometry
// ---------------------------------------------------------------------------------------------

Id Sketch::addPoint(double x, double y)
{
    const Id id = nextId();
    m_entities.emplace(id, Entity{id, Point{x, y}});
    return id;
}

Id Sketch::addLine(Id p1, Id p2)
{
    if (kindOf(p1) != EntityKind::Point || kindOf(p2) != EntityKind::Point) {
        throw SketchError("line endpoints must be points");
    }
    if (p1 == p2) {
        throw SketchError("line endpoints must be different points");
    }
    const Id id = nextId();
    m_entities.emplace(id, Entity{id, Line{p1, p2}});
    return id;
}

Id Sketch::addLine(double x1, double y1, double x2, double y2)
{
    const Id p1 = addPoint(x1, y1);
    const Id p2 = addPoint(x2, y2);
    return addLine(p1, p2);
}

Id Sketch::addCircle(Id center, double radius)
{
    if (kindOf(center) != EntityKind::Point) {
        throw SketchError("circle centre must be a point");
    }
    if (!(radius > 0.0)) {
        throw SketchError("circle radius must be positive");
    }
    const Id id = nextId();
    m_entities.emplace(id, Entity{id, Circle{center, radius}});
    return id;
}

Id Sketch::addCircle(double cx, double cy, double radius)
{
    if (!(radius > 0.0)) {
        throw SketchError("circle radius must be positive");
    }
    return addCircle(addPoint(cx, cy), radius);
}

Id Sketch::addArc(Id center, Id start, Id end)
{
    for (Id p : {center, start, end}) {
        if (kindOf(p) != EntityKind::Point) {
            throw SketchError("arc centre, start and end must be points");
        }
    }
    if (center == start || center == end || start == end) {
        throw SketchError("arc centre, start and end must be different points");
    }
    const Id id = nextId();
    m_entities.emplace(id, Entity{id, Arc{center, start, end}});
    return id;
}

Id Sketch::addArc(double cx, double cy, double radius, double startDeg, double endDeg)
{
    if (!(radius > 0.0)) {
        throw SketchError("arc radius must be positive");
    }
    constexpr double toRad = std::numbers::pi / 180.0;
    const Id c = addPoint(cx, cy);
    const Id s = addPoint(cx + radius * std::cos(startDeg * toRad), cy + radius * std::sin(startDeg * toRad));
    const Id e = addPoint(cx + radius * std::cos(endDeg * toRad), cy + radius * std::sin(endDeg * toRad));
    return addArc(c, s, e);
}

// ---------------------------------------------------------------------------------------------
// Constraints
// ---------------------------------------------------------------------------------------------

Id Sketch::addConstraint(ConstraintType type, std::vector<Id> refs, double value)
{
    normalize(type, refs);
    validate(type, refs, value);

    const Id id = nextId();
    m_constraints.emplace(id, Constraint{id, type, std::move(refs), value});
    return id;
}

bool Sketch::canAddConstraint(ConstraintType type, std::vector<Id> refs, std::string* why) const
{
    try {
        normalize(type, refs);
        validate(type, refs, type == ConstraintType::Angle ? 45.0 : 1.0);
        return true;
    }
    catch (const SketchError& e) {
        if (why) {
            *why = e.what();
        }
        return false;
    }
}

void Sketch::setConstraintLabel(Id id, double dx, double dy)
{
    auto it = m_constraints.find(id);
    if (it == m_constraints.end()) {
        throw SketchError("unknown constraint " + std::to_string(id));
    }
    it->second.labelX = dx;
    it->second.labelY = dy;
}

void Sketch::normalize(ConstraintType type, std::vector<Id>& refs) const
{
    for (Id r : refs) {
        if (!hasEntity(r)) {
            throw SketchError("constraint references unknown entity " + std::to_string(r));
        }
    }

    // Normalise argument order so the solver only has to handle one form.
    auto putFirst = [&](auto pred) {
        if (refs.size() == 2 && !pred(refs[0]) && pred(refs[1])) {
            std::swap(refs[0], refs[1]);
        }
    };
    auto isPoint = [this](Id id) { return kindOf(id) == EntityKind::Point; };
    auto isLine = [this](Id id) { return kindOf(id) == EntityKind::Line; };
    switch (type) {
        case ConstraintType::Coincident:
        case ConstraintType::Midpoint:
        case ConstraintType::Distance: putFirst(isPoint); break;
        case ConstraintType::Tangent: putFirst(isLine); break;
        case ConstraintType::Symmetric:
            // (point, point, line) in any click order.
            std::ranges::stable_partition(refs, isPoint);
            break;
        default: break;
    }
}

void Sketch::setConstraintValue(Id id, double value)
{
    auto it = m_constraints.find(id);
    if (it == m_constraints.end()) {
        throw SketchError("unknown constraint " + std::to_string(id));
    }
    if (!isDimension(it->second.type)) {
        throw SketchError(std::string(toString(it->second.type)) + " constraint has no value");
    }
    validate(it->second.type, it->second.refs, value);
    it->second.value = value;
}

void Sketch::validate(ConstraintType type, const std::vector<Id>& refs, double value) const
{
    const std::string name = toString(type);
    auto fail = [&](const std::string& what) { throw SketchError(name + ": " + what); };

    const std::set<Id> unique(refs.begin(), refs.end());
    if (unique.size() != refs.size()) {
        fail("references must be distinct");
    }

    auto kinds = [&](std::initializer_list<EntityKind> expected) {
        if (refs.size() != expected.size()) {
            return false;
        }
        size_t i = 0;
        for (EntityKind k : expected) {
            if (kindOf(refs[i++]) != k) {
                return false;
            }
        }
        return true;
    };
    auto roundPair = [&] { return refs.size() == 2 && isRound(refs[0]) && isRound(refs[1]); };
    using K = EntityKind;

    switch (type) {
        case ConstraintType::Coincident:
            if (!(kinds({K::Point, K::Point}) || (refs.size() == 2 && kindOf(refs[0]) == K::Point && isCurve(refs[1])))) {
                fail("expects (point, point) or (point, curve)");
            }
            break;
        case ConstraintType::Horizontal:
        case ConstraintType::Vertical:
            if (!(kinds({K::Line}) || kinds({K::Point, K::Point}))) {
                fail("expects (line) or (point, point)");
            }
            break;
        case ConstraintType::Parallel:
        case ConstraintType::Perpendicular:
            if (!kinds({K::Line, K::Line})) {
                fail("expects (line, line)");
            }
            break;
        case ConstraintType::Tangent:
            if (!((refs.size() == 2 && kindOf(refs[0]) == K::Line && isRound(refs[1])) || roundPair())) {
                fail("expects (line, circle|arc) or (circle|arc, circle|arc)");
            }
            break;
        case ConstraintType::Equal:
            if (!(kinds({K::Line, K::Line}) || roundPair())) {
                fail("expects (line, line) or (circle|arc, circle|arc)");
            }
            break;
        case ConstraintType::Midpoint:
            if (!kinds({K::Point, K::Line})) {
                fail("expects (point, line)");
            }
            break;
        case ConstraintType::Concentric:
            if (!roundPair()) {
                fail("expects (circle|arc, circle|arc)");
            }
            break;
        case ConstraintType::Symmetric:
            if (!kinds({K::Point, K::Point, K::Line})) {
                fail("expects (point, point, line)");
            }
            break;
        case ConstraintType::Distance:
            if (!(kinds({K::Point, K::Point}) || kinds({K::Point, K::Line}) || kinds({K::Line}))) {
                fail("expects (point, point), (point, line) or (line)");
            }
            if (!(value > 0.0)) {
                fail("value must be positive");
            }
            break;
        case ConstraintType::HorizontalDistance:
        case ConstraintType::VerticalDistance:
            if (!(kinds({K::Point, K::Point}) || kinds({K::Line}))) {
                fail("expects (point, point) or (line)");
            }
            if (!(value >= 0.0)) {
                fail("value must not be negative");
            }
            break;
        case ConstraintType::Radius:
        case ConstraintType::Diameter:
            if (!(refs.size() == 1 && isRound(refs[0]))) {
                fail("expects (circle|arc)");
            }
            if (!(value > 0.0)) {
                fail("value must be positive");
            }
            break;
        case ConstraintType::Angle:
            if (!kinds({K::Line, K::Line})) {
                fail("expects (line, line)");
            }
            if (!(value >= 0.0 && value <= 180.0)) {
                fail("value must be between 0 and 180 degrees");
            }
            break;
    }
}

// ---------------------------------------------------------------------------------------------
// Removal
// ---------------------------------------------------------------------------------------------

bool Sketch::referencesPoint(const Entity& e, Id p) const
{
    for (Id d : definingPoints(e.id)) {
        if (d == p && d != e.id) {
            return true;
        }
    }
    return false;
}

void Sketch::remove(Id id)
{
    if (m_constraints.erase(id) > 0) {
        return;
    }
    if (!hasEntity(id)) {
        throw SketchError("unknown id " + std::to_string(id));
    }
    if (isOrigin(id)) {
        throw SketchError("the origin can't be deleted");
    }

    std::set<Id> doomed{id};

    // Curves built on a deleted point go too.
    if (kindOf(id) == EntityKind::Point) {
        for (const auto& [eid, e] : m_entities) {
            if (referencesPoint(e, id)) {
                doomed.insert(eid);
            }
        }
    }

    // Points of deleted curves that no surviving curve uses.
    std::set<Id> candidates;
    for (Id d : doomed) {
        if (kindOf(d) != EntityKind::Point) {
            for (Id p : definingPoints(d)) {
                candidates.insert(p);
            }
        }
    }
    for (Id p : candidates) {
        if (isOrigin(p)) {
            continue;  // curves ending on the origin don't take it with them
        }
        const bool used = std::ranges::any_of(m_entities, [&](const auto& kv) {
            return !doomed.contains(kv.first) && referencesPoint(kv.second, p);
        });
        if (!used) {
            doomed.insert(p);
        }
    }

    for (Id d : doomed) {
        m_entities.erase(d);
    }
    std::erase_if(m_constraints, [&](const auto& kv) {
        return std::ranges::any_of(kv.second.refs, [&](Id r) { return doomed.contains(r); });
    });
}

void Sketch::clear()
{
    m_entities.clear();
    m_constraints.clear();
    m_origin = kNoId;
}

// ---------------------------------------------------------------------------------------------
// Lookup
// ---------------------------------------------------------------------------------------------

const Entity& Sketch::entity(Id id) const
{
    auto it = m_entities.find(id);
    if (it == m_entities.end()) {
        throw SketchError("unknown entity " + std::to_string(id));
    }
    return it->second;
}

Entity& Sketch::entity(Id id)
{
    return const_cast<Entity&>(std::as_const(*this).entity(id));
}

const Constraint& Sketch::constraint(Id id) const
{
    auto it = m_constraints.find(id);
    if (it == m_constraints.end()) {
        throw SketchError("unknown constraint " + std::to_string(id));
    }
    return it->second;
}

namespace
{
template<typename T>
const T& geomAs(const Entity& e, const char* what)
{
    if (const T* g = std::get_if<T>(&e.geom)) {
        return *g;
    }
    throw SketchError("entity " + std::to_string(e.id) + " is not a " + what);
}
}  // namespace

const Point& Sketch::point(Id id) const { return geomAs<Point>(entity(id), "point"); }
Point& Sketch::point(Id id) { return const_cast<Point&>(std::as_const(*this).point(id)); }
const Line& Sketch::line(Id id) const { return geomAs<Line>(entity(id), "line"); }
const Circle& Sketch::circle(Id id) const { return geomAs<Circle>(entity(id), "circle"); }
Circle& Sketch::circle(Id id) { return const_cast<Circle&>(std::as_const(*this).circle(id)); }
const Arc& Sketch::arc(Id id) const { return geomAs<Arc>(entity(id), "arc"); }

std::vector<Id> Sketch::definingPoints(Id id) const
{
    const Entity& e = entity(id);
    switch (e.kind()) {
        case EntityKind::Point: return {id};
        case EntityKind::Line: {
            const auto& l = std::get<Line>(e.geom);
            return {l.p1, l.p2};
        }
        case EntityKind::Circle: return {std::get<Circle>(e.geom).center};
        case EntityKind::Arc: {
            const auto& a = std::get<Arc>(e.geom);
            return {a.center, a.start, a.end};
        }
    }
    return {};
}

std::vector<Id> Sketch::constraintsOn(Id entityId) const
{
    std::vector<Id> out;
    for (const auto& [cid, c] : m_constraints) {
        if (std::ranges::find(c.refs, entityId) != c.refs.end()) {
            out.push_back(cid);
        }
    }
    return out;
}

double Sketch::arcRadius(Id id) const
{
    const Arc& a = arc(id);
    const Point& c = point(a.center);
    const Point& s = point(a.start);
    return std::hypot(s.x - c.x, s.y - c.y);
}

EntityKind Sketch::kindOf(Id id) const
{
    return entity(id).kind();
}

bool Sketch::isCurve(Id id) const
{
    return kindOf(id) != EntityKind::Point;
}

bool Sketch::isRound(Id id) const
{
    const EntityKind k = kindOf(id);
    return k == EntityKind::Circle || k == EntityKind::Arc;
}

}  // namespace core
