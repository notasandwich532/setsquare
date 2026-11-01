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
        case ConstraintType::Collinear: return "collinear";
        case ConstraintType::Ratio: return "ratio";
        case ConstraintType::Distance: return "distance";
        case ConstraintType::HorizontalDistance: return "horizontalDistance";
        case ConstraintType::VerticalDistance: return "verticalDistance";
        case ConstraintType::Radius: return "radius";
        case ConstraintType::Diameter: return "diameter";
        case ConstraintType::Angle: return "angle";
        case ConstraintType::Offset: return "offset";
        case ConstraintType::LinearPattern: return "linearPattern";
        case ConstraintType::CircularPattern: return "circularPattern";
        case ConstraintType::Divide: return "divide";
    }
    return "?";
}

bool isDimension(ConstraintType type)
{
    return type >= ConstraintType::Distance && type <= ConstraintType::Angle;
}

bool isFeature(ConstraintType type)
{
    return type >= ConstraintType::Offset;
}

bool hasValue(ConstraintType type)
{
    return isDimension(type) || isFeature(type) || type == ConstraintType::Ratio;
}

bool constraintTypeFromString(const std::string& name, ConstraintType& out)
{
    for (int i = 0; i <= static_cast<int>(ConstraintType::Divide); ++i) {
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
    if (m_axes[0] == kNoId) {
        for (int i = 0; i < 2; ++i) {
            m_axisEnds[i] = addPoint(i == 0 ? 1.0 : 0.0, i == 0 ? 0.0 : 1.0);
            m_entities.at(m_axisEnds[i]).fixed = true;
            m_axes[i] = addLine(m_origin, m_axisEnds[i]);
            m_entities.at(m_axes[i]).fixed = true;
            m_entities.at(m_axes[i]).construction = true;
        }
    }
    return m_origin;
}

bool Sketch::isReference(Id id) const
{
    return isOrigin(id) || isAxis(id) || isHiddenReference(id);
}

void Sketch::setAxes(Id xAxis, Id yAxis)
{
    const std::array<Id, 2> axes{xAxis, yAxis};
    for (int i = 0; i < 2; ++i) {
        const Line& l = line(axes[i]);  // throws if it isn't a line
        if (l.p1 != m_origin) {
            throw SketchError("an axis must start at the origin");
        }
        m_axes[i] = axes[i];
        m_axisEnds[i] = l.p2;
        Entity& end = m_entities.at(l.p2);
        end.fixed = true;
        end.geom = Point{i == 0 ? 1.0 : 0.0, i == 0 ? 0.0 : 1.0};
        m_entities.at(axes[i]).fixed = true;
        m_entities.at(axes[i]).construction = true;
    }
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
    if (e.id == kNoId || hasEntity(e.id) || hasConstraint(e.id) || hasFill(e.id)) {
        throw SketchError("duplicate or invalid id " + std::to_string(e.id));
    }
    checkGeometry(e);
    m_entities.emplace(e.id, e);
    m_nextId = std::max(m_nextId, e.id + 1);
}

void Sketch::insertConstraint(const Constraint& c)
{
    if (c.id == kNoId || hasEntity(c.id) || hasConstraint(c.id) || hasFill(c.id)) {
        throw SketchError("duplicate or invalid id " + std::to_string(c.id));
    }
    std::vector<Id> refs = c.refs;
    normalize(c.type, refs);
    validate(c.type, refs, c.value, c.params);
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

Id Sketch::addConstraint(ConstraintType type, std::vector<Id> refs, double value, std::vector<double> params)
{
    normalize(type, refs);
    validate(type, refs, value, params);

    const Id id = nextId();
    Constraint c{id, type, std::move(refs), value};
    c.params = std::move(params);
    m_constraints.emplace(id, std::move(c));
    return id;
}

void Sketch::setConstraintParams(Id id, std::vector<double> params)
{
    auto it = m_constraints.find(id);
    if (it == m_constraints.end()) {
        throw SketchError("unknown constraint " + std::to_string(id));
    }
    validate(it->second.type, it->second.refs, it->second.value, params);
    it->second.params = std::move(params);
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

void Sketch::setConstraintFlip(Id id, std::uint8_t flip)
{
    auto it = m_constraints.find(id);
    if (it == m_constraints.end()) {
        throw SketchError("unknown constraint " + std::to_string(id));
    }
    it->second.flip = flip & 3;
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
    if (!hasValue(it->second.type)) {
        throw SketchError(std::string(toString(it->second.type)) + " constraint has no value");
    }
    validate(it->second.type, it->second.refs, value, it->second.params);
    it->second.value = value;
}

void Sketch::validate(ConstraintType type, const std::vector<Id>& refs, double value,
                      const std::vector<double>& params) const
{
    const std::string name = toString(type);
    auto fail = [&](const std::string& what) { throw SketchError(name + ": " + what); };

    const std::set<Id> unique(refs.begin(), refs.end());
    if (unique.size() != refs.size()) {
        fail("references must be distinct");
    }
    if (isFeature(type)) {
        validateFeature(type, refs, value, params);
        return;
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
        case ConstraintType::Collinear:
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
        case ConstraintType::Ratio:
            if (!(kinds({K::Line, K::Line}) || roundPair())) {
                fail("expects (line, line) or (circle|arc, circle|arc)");
            }
            if (!(value > 0.0) || !std::isfinite(value)) {
                fail("the ratio must be positive");
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
        case ConstraintType::Offset:
        case ConstraintType::LinearPattern:
        case ConstraintType::CircularPattern:
        case ConstraintType::Divide: break;  // validateFeature
    }
}

void Sketch::validateFeature(ConstraintType type, const std::vector<Id>& refs, double value,
                             const std::vector<double>& params) const
{
    const std::string name = toString(type);
    auto fail = [&](const std::string& what) { throw SketchError(name + ": " + what); };
    auto count = [&](size_t i) {
        const double v = i < params.size() ? params[i] : 0.0;
        if (!(v >= 1.0 && v <= 10000.0 && v == std::floor(v))) {
            fail("counts must be whole numbers from 1");
        }
        return static_cast<size_t>(v);
    };
    // `groups` runs of `n` entities each, starting at `first`; every run like the first.
    auto matchingGroups = [&](size_t first, size_t groups, const char* what) {
        if (groups == 0 || refs.size() <= first || (refs.size() - first) % groups != 0) {
            fail(std::string("expects ") + what);
        }
        const size_t n = (refs.size() - first) / groups;
        for (size_t g = 1; g < groups; ++g) {
            for (size_t i = 0; i < n; ++i) {
                if (kindOf(refs[first + g * n + i]) != kindOf(refs[first + i])) {
                    fail(std::string("expects ") + what + " of the same kinds");
                }
            }
        }
        return n;
    };

    switch (type) {
        case ConstraintType::Offset: {
            // params {2[, caps]}: offset to both sides (sources, one side's offsets, the other's),
            // then that many arcs capping the ends.
            if (params.size() > 2 || (!params.empty() && params[0] != 1.0 && params[0] != 2.0)) {
                fail("expects params {} or {sides[, caps]}");
            }
            const size_t caps = params.size() > 1 ? static_cast<size_t>(params[1]) : 0;
            if (caps > 2 || caps > refs.size() || (caps > 0 && params[0] != 2.0)) {
                fail("caps only close a both-sides offset");
            }
            for (size_t i = refs.size() - caps; i < refs.size(); ++i) {
                if (kindOf(refs[i]) != EntityKind::Arc) {
                    fail("caps must be arcs");
                }
            }
            const std::vector<Id> curves(refs.begin(), refs.end() - static_cast<std::ptrdiff_t>(caps));
            const size_t groups = params.empty() ? 2 : static_cast<size_t>(params[0]) + 1;
            if (curves.empty() || curves.size() % groups != 0) {
                fail("expects sources then their offsets");
            }
            const size_t n = curves.size() / groups;
            for (size_t g = 1; g < groups; ++g) {
                for (size_t i = 0; i < n; ++i) {
                    if (kindOf(curves[g * n + i]) != kindOf(curves[i])) {
                        fail("expects offsets of the same kinds as their sources");
                    }
                }
            }
            for (size_t i = 0; i < n; ++i) {
                if (!isCurve(refs[i])) {
                    fail("only curves can be offset");
                }
            }
            if (!(value > 0.0)) {
                fail("distance must be positive");
            }
            break;
        }
        case ConstraintType::LinearPattern: {
            if (params.size() != 4 && params.size() != 6) {
                fail("expects params {count, angle, count2, spacing2[, symmetric, symmetric2]}");
            }
            // Symmetric: `count` each way, sharing the original.
            auto along = [&](size_t countIndex, size_t symmetricIndex) {
                const size_t n = count(countIndex);
                return params.size() > symmetricIndex && params[symmetricIndex] != 0.0 ? 2 * n - 1 : n;
            };
            const size_t total = along(0, 4) * along(2, 5);
            if (total < 2) {
                fail("needs at least two instances");
            }
            matchingGroups(0, total, "sources then each copy");
            // The second spacing may be negative: rows on the other side of the first direction.
            if (!(value > 0.0) || !(params[3] != 0.0) || !std::isfinite(params[3])) {
                fail("spacing must be positive (the second one non-zero)");
            }
            break;
        }
        case ConstraintType::CircularPattern: {
            if (params.size() != 1) {
                fail("expects params {count}");
            }
            const size_t n = count(0);
            if (n < 2) {
                fail("needs at least two instances");
            }
            if (refs.empty() || kindOf(refs[0]) != EntityKind::Point) {
                fail("expects a centre point first");
            }
            matchingGroups(1, n, "the centre, sources then each copy");
            if (!(value > 0.0 && value <= 360.0)) {
                fail("angle must be more than 0 and at most 360 degrees");
            }
            break;
        }
        case ConstraintType::Divide: {
            if (refs.size() < 2 || kindOf(refs[0]) != EntityKind::Line) {
                fail("expects a line, then its points");
            }
            for (size_t i = 1; i < refs.size(); ++i) {
                if (kindOf(refs[i]) != EntityKind::Point) {
                    fail("expects a line, then its points");
                }
            }
            if (params.size() != refs.size() + 2) {  // 2 flags, then a part per point and one more
                fail("expects params {by ratio, reversed, parts...} with one more part than points");
            }
            for (size_t i = 2; i < params.size(); ++i) {
                if (!(params[i] > 0.0) || !std::isfinite(params[i])) {
                    fail("the parts must be positive");
                }
            }
            if (value != double(refs.size() - 1)) {
                fail("the value is the number of points (change the spacing to change it)");
            }
            break;
        }
        default: break;
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
    if (m_constraints.erase(id) > 0 || m_fills.erase(id) > 0) {
        return;
    }
    if (auto t = m_texts.find(id); t != m_texts.end()) {
        // The text, and its anchor too unless something else is built on it.
        const Id anchor = t->second.anchor;
        m_texts.erase(t);
        const bool constrained = std::ranges::any_of(m_constraints, [&](const auto& kv) {
            return std::ranges::find(kv.second.refs, anchor) != kv.second.refs.end();
        });
        if (hasEntity(anchor) && !isReference(anchor) && !isPointUsed(anchor) && !constrained) {
            remove(anchor);
        }
        return;
    }
    if (!hasEntity(id)) {
        throw SketchError("unknown id " + std::to_string(id));
    }
    if (isReference(id)) {
        throw SketchError(isOrigin(id) ? "the origin can't be deleted" : "the axes can't be deleted");
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
        if (isReference(p)) {
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
        m_strokes.erase(d);
    }
    std::erase_if(m_texts, [&](const auto& kv) { return doomed.contains(kv.second.anchor); });
    std::erase_if(m_constraints, [&](const auto& kv) {
        return std::ranges::any_of(kv.second.refs, [&](Id r) { return doomed.contains(r); });
    });
}

void Sketch::rebindPoint(Id curve, Id oldPoint, Id newPoint)
{
    if (kindOf(newPoint) != EntityKind::Point) {
        throw SketchError("rebind: not a point");
    }
    Entity& e = entity(curve);
    bool found = false;
    auto swap = [&](Id& slot) {
        if (slot == oldPoint) {
            slot = newPoint;
            found = true;
        }
    };
    if (auto* l = std::get_if<Line>(&e.geom)) {
        swap(l->p1);
        swap(l->p2);
    }
    else if (auto* a = std::get_if<Arc>(&e.geom)) {
        swap(a->center);
        swap(a->start);
        swap(a->end);
    }
    else if (auto* c = std::get_if<Circle>(&e.geom)) {
        swap(c->center);
    }
    if (!found) {
        throw SketchError("rebind: the curve doesn't use that point");
    }
    checkGeometry(e);
}

void Sketch::setStroke(Id curve, const StrokeStyle& style)
{
    if (!hasEntity(curve) || !isCurve(curve) || isReference(curve)) {
        throw SketchError("only lines, circles and arcs can have a stroke");
    }
    if (!(style.width >= 0.0)) {
        throw SketchError("stroke width can't be negative");
    }
    m_strokes[curve] = style;
}

void Sketch::clearStroke(Id curve)
{
    m_strokes.erase(curve);
}

const StrokeStyle* Sketch::stroke(Id curve) const
{
    auto it = m_strokes.find(curve);
    return it == m_strokes.end() ? nullptr : &it->second;
}

void Sketch::copyStroke(Id from, Id to)
{
    if (const StrokeStyle* s = stroke(from)) {
        const StrokeStyle style = *s;  // (inserting may not move it, but be safe)
        setStroke(to, style);
    }
}

Id Sketch::addFill(const Fill& fill)
{
    Fill f = fill;
    f.id = nextId();
    std::ranges::sort(f.boundary);
    m_fills.emplace(f.id, std::move(f));
    return m_nextId - 1;
}

void Sketch::setFill(const Fill& fill)
{
    auto it = m_fills.find(fill.id);
    if (it == m_fills.end()) {
        throw SketchError("unknown fill " + std::to_string(fill.id));
    }
    it->second = fill;
    std::ranges::sort(it->second.boundary);
}

void Sketch::insertFill(const Fill& fill)
{
    if (fill.id == kNoId || hasEntity(fill.id) || hasConstraint(fill.id) || hasFill(fill.id)) {
        throw SketchError("duplicate or invalid id " + std::to_string(fill.id));
    }
    m_fills.emplace(fill.id, fill);
    std::ranges::sort(m_fills.at(fill.id).boundary);
    m_nextId = std::max(m_nextId, fill.id + 1);
}

const Fill& Sketch::fill(Id id) const
{
    auto it = m_fills.find(id);
    if (it == m_fills.end()) {
        throw SketchError("unknown fill " + std::to_string(id));
    }
    return it->second;
}

bool Sketch::isPointUsed(Id point) const
{
    return std::ranges::any_of(m_entities, [&](const auto& kv) { return referencesPoint(kv.second, point); })
        || std::ranges::any_of(m_texts, [&](const auto& kv) { return kv.second.anchor == point; });
}

// --- texts ---

const char* toString(TextOutline outline)
{
    switch (outline) {
        case TextOutline::AsDesigned: return "as_designed";
        case TextOutline::Merged: return "merged";
        case TextOutline::Silhouette: return "silhouette";
    }
    return "?";
}

bool textOutlineFromString(const std::string& name, TextOutline& out)
{
    for (TextOutline o : {TextOutline::AsDesigned, TextOutline::Merged, TextOutline::Silhouette}) {
        if (name == toString(o)) {
            out = o;
            return true;
        }
    }
    return false;
}

Point textToWorld(const Point& anchor, double angleDeg, const Point& local)
{
    const double a = angleDeg * std::numbers::pi / 180.0;
    const double c = std::cos(a), s = std::sin(a);
    return {anchor.x + c * local.x - s * local.y, anchor.y + s * local.x + c * local.y};
}

namespace
{
void checkText(const Sketch& s, const Text& t)
{
    if (!s.hasEntity(t.anchor) || s.entity(t.anchor).kind() != EntityKind::Point) {
        throw SketchError("text: its anchor must be a point");
    }
    if (!(t.height > 0.0) || !std::isfinite(t.height)) {
        throw SketchError("text: the height must be positive");
    }
    if (!(t.minThickness >= 0.0) || !std::isfinite(t.minThickness)) {
        throw SketchError("text: the minimum thickness can't be negative");
    }
    if (!std::isfinite(t.angle)) {
        throw SketchError("text: the angle must be a number");
    }
}
}  // namespace

Id Sketch::addText(const Text& text)
{
    checkText(*this, text);
    Text t = text;
    t.id = nextId();
    m_texts.emplace(t.id, std::move(t));
    return m_nextId - 1;
}

void Sketch::setText(const Text& text)
{
    auto it = m_texts.find(text.id);
    if (it == m_texts.end()) {
        throw SketchError("unknown text " + std::to_string(text.id));
    }
    checkText(*this, text);
    it->second = text;
}

void Sketch::insertText(const Text& text)
{
    if (text.id == kNoId || hasEntity(text.id) || hasConstraint(text.id) || hasFill(text.id) || hasText(text.id)) {
        throw SketchError("duplicate or invalid id " + std::to_string(text.id));
    }
    checkText(*this, text);
    m_texts.emplace(text.id, text);
    m_nextId = std::max(m_nextId, text.id + 1);
}

const Text& Sketch::text(Id id) const
{
    auto it = m_texts.find(id);
    if (it == m_texts.end()) {
        throw SketchError("unknown text " + std::to_string(id));
    }
    return it->second;
}

bool Sketch::removeIfUnused(Id point)
{
    if (!hasEntity(point) || isReference(point) || isPointUsed(point)) {
        return false;
    }
    remove(point);
    return true;
}

void Sketch::retarget(Id from, Id to)
{
    std::map<Id, Constraint> updated = m_constraints;
    for (auto& [id, c] : updated) {
        if (std::ranges::find(c.refs, from) == c.refs.end()) {
            continue;
        }
        std::ranges::replace(c.refs, from, to);
        normalize(c.type, c.refs);
        validate(c.type, c.refs, hasValue(c.type) ? c.value : 1.0, c.params);
    }
    m_constraints = std::move(updated);
}

void Sketch::clear()
{
    m_entities.clear();
    m_constraints.clear();
    m_strokes.clear();
    m_fills.clear();
    m_texts.clear();
    m_origin = kNoId;
    m_axes = {kNoId, kNoId};
    m_axisEnds = {kNoId, kNoId};
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
