#include "Sketch.h"

#include <algorithm>
#include <cmath>
#include <functional>
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
        case EntityKind::Ellipse: return "ellipse";
        case EntityKind::EllipseArc: return "ellipse_arc";
        case EntityKind::Spline: return "spline";
    }
    return "?";
}

const char* toString(SplineKind kind)
{
    return kind == SplineKind::Control ? "control" : "fit";
}

bool splineKindFromString(const std::string& name, SplineKind& out)
{
    if (name == "fit") {
        out = SplineKind::Fit;
        return true;
    }
    if (name == "control") {
        out = SplineKind::Control;
        return true;
    }
    return false;
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
        case ConstraintType::Group: return "group";
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
        m_entities.at(m_origin).sublayer = kNoId;  // (in every sublayer)
    }
    if (m_axes[0] == kNoId) {
        for (int i = 0; i < 2; ++i) {
            m_axisEnds[i] = addPoint(i == 0 ? 1.0 : 0.0, i == 0 ? 0.0 : 1.0);
            m_entities.at(m_axisEnds[i]).fixed = true;
            m_axes[i] = addLine(m_origin, m_axisEnds[i]);
            m_entities.at(m_axes[i]).fixed = true;
            m_entities.at(m_axes[i]).construction = true;
            m_entities.at(m_axisEnds[i]).sublayer = m_entities.at(m_axes[i]).sublayer = kNoId;
        }
    }
    return m_origin;
}

// --- sublayers ---

bool Sketch::hasSublayer(Id id) const
{
    return std::ranges::any_of(m_sublayers, [&](const Sublayer& l) { return l.id == id; });
}

const Sublayer& Sketch::sublayer(Id id) const
{
    for (const Sublayer& l : m_sublayers) {
        if (l.id == id) {
            return l;
        }
    }
    throw SketchError("unknown sublayer " + std::to_string(id));
}

void Sketch::setActiveSublayer(Id id)
{
    const Id layerId = sublayer(id).layer;  // (throws if unknown)
    m_activeSublayer = id;
    for (Layer& l : m_layers) {
        if (l.id == layerId) {
            l.activeSublayer = id;
        }
    }
}

Id Sketch::addSublayer(const std::string& name)
{
    Id id = 1;
    for (const Sublayer& l : m_sublayers) {
        id = std::max(id, l.id + 1);
    }
    m_sublayers.push_back({id, name, true, activeLayer()});
    return id;
}

void Sketch::renameSublayer(Id id, const std::string& name)
{
    for (Sublayer& l : m_sublayers) {
        if (l.id == id) {
            l.name = name;
            return;
        }
    }
    throw SketchError("unknown sublayer " + std::to_string(id));
}

void Sketch::setSublayerVisible(Id id, bool visible)
{
    for (Sublayer& l : m_sublayers) {
        if (l.id == id) {
            l.visible = visible;
            return;
        }
    }
    throw SketchError("unknown sublayer " + std::to_string(id));
}

void Sketch::moveSublayer(Id id, size_t index)
{
    // (`index` counts among the sublayers of its own layer)
    auto it = std::ranges::find_if(m_sublayers, [&](const Sublayer& l) { return l.id == id; });
    if (it == m_sublayers.end()) {
        throw SketchError("unknown sublayer " + std::to_string(id));
    }
    const Sublayer moved = *it;
    m_sublayers.erase(it);
    std::vector<size_t> siblings;
    for (size_t i = 0; i < m_sublayers.size(); ++i) {
        if (m_sublayers[i].layer == moved.layer) {
            siblings.push_back(i);
        }
    }
    const size_t at = siblings.empty() ? m_sublayers.size()
        : index < siblings.size()     ? siblings[index]
                                      : siblings.back() + 1;
    m_sublayers.insert(m_sublayers.begin() + static_cast<std::ptrdiff_t>(at), moved);
}

void Sketch::removeSublayer(Id id)
{
    const Id layerId = sublayer(id).layer;
    if (sublayersOf(layerId).size() == 1) {
        throw SketchError(m_layers.size() == 1 ? "the last sublayer can't be deleted"
                                               : "a layer's last sublayer can't be deleted (delete the layer)");
    }
    // Its contents: texts and constraints, then geometry (whatever depends on it goes too).
    std::erase_if(m_constraints, [&](const auto& kv) { return kv.second.sublayer == id; });
    std::erase_if(m_drawnDimensions, [&](const auto& kv) { return kv.second.sublayer == id; });
    std::vector<Id> doomed;
    for (const auto& [tid, t] : m_texts) {
        if (t.sublayer == id) {
            doomed.push_back(tid);
        }
    }
    for (const auto& [eid, e] : m_entities) {
        if (e.sublayer == id && e.kind() != EntityKind::Point) {
            doomed.push_back(eid);
        }
    }
    for (const auto& [eid, e] : m_entities) {
        if (e.sublayer == id && e.kind() == EntityKind::Point) {
            doomed.push_back(eid);
        }
    }
    for (Id d : doomed) {
        if ((hasEntity(d) && !isReference(d)) || hasText(d)) {
            remove(d);
        }
    }
    std::vector<Id> siblings = sublayersOf(layerId);
    const size_t at = static_cast<size_t>(std::ranges::find(siblings, id) - siblings.begin());
    siblings.erase(siblings.begin() + static_cast<std::ptrdiff_t>(at));
    const Id replacement = siblings[std::min(at, siblings.size() - 1)];
    std::erase_if(m_sublayers, [&](const Sublayer& l) { return l.id == id; });
    for (Layer& l : m_layers) {
        if (l.activeSublayer == id) {
            l.activeSublayer = replacement;
        }
    }
    if (m_activeSublayer == id) {
        m_activeSublayer = replacement;
    }
}

Id Sketch::sublayerOf(Id item) const
{
    if (auto e = m_entities.find(item); e != m_entities.end()) {
        return e->second.sublayer;
    }
    if (auto c = m_constraints.find(item); c != m_constraints.end()) {
        return c->second.sublayer;
    }
    if (auto t = m_texts.find(item); t != m_texts.end()) {
        return t->second.sublayer;
    }
    if (auto d = m_drawnDimensions.find(item); d != m_drawnDimensions.end()) {
        return d->second.sublayer;
    }
    return kNoId;
}

void Sketch::setSublayerOf(Id item, Id sublayerId)
{
    sublayer(sublayerId);
    if (auto e = m_entities.find(item); e != m_entities.end()) {
        if (!isReference(item)) {
            e->second.sublayer = sublayerId;
        }
    }
    else if (auto c = m_constraints.find(item); c != m_constraints.end()) {
        c->second.sublayer = sublayerId;
    }
    else if (auto t = m_texts.find(item); t != m_texts.end()) {
        t->second.sublayer = sublayerId;
    }
    else if (auto d = m_drawnDimensions.find(item); d != m_drawnDimensions.end()) {
        d->second.sublayer = sublayerId;
    }
    else {
        throw SketchError("unknown item " + std::to_string(item));
    }
}

bool Sketch::isShown(Id item) const
{
    const Id l = sublayerOf(item);
    if (l == kNoId) {
        if (const auto f = m_fills.find(item); f != m_fills.end() && hasLayer(f->second.layer)) {
            return layer(f->second.layer).visible;
        }
        if (const auto i = m_images.find(item); i != m_images.end()) {
            return i->second.visible && (!hasLayer(i->second.layer) || layer(i->second.layer).visible);
        }
        return true;
    }
    for (const Sublayer& s : m_sublayers) {
        if (s.id == l) {
            return s.visible && (!hasLayer(s.layer) || layer(s.layer).visible);
        }
    }
    return true;
}

void Sketch::setSublayers(std::vector<Sublayer> sublayers, Id active)
{
    if (sublayers.empty()) {
        throw SketchError("a sketch needs a sublayer");
    }
    m_sublayers = std::move(sublayers);
    m_activeSublayer = hasSublayer(active) ? active : m_sublayers.front().id;
}

// --- layers ---

bool Sketch::hasLayer(Id id) const
{
    return std::ranges::any_of(m_layers, [&](const Layer& l) { return l.id == id; });
}

const Layer& Sketch::layer(Id id) const
{
    for (const Layer& l : m_layers) {
        if (l.id == id) {
            return l;
        }
    }
    throw SketchError("unknown layer " + std::to_string(id));
}

Id Sketch::activeLayer() const
{
    return hasSublayer(m_activeSublayer) ? sublayer(m_activeSublayer).layer : m_layers.front().id;
}

void Sketch::setActiveLayer(Id id)
{
    const Layer& l = layer(id);
    const std::vector<Id> own = sublayersOf(id);
    setActiveSublayer(std::ranges::find(own, l.activeSublayer) != own.end() ? l.activeSublayer : own.front());
}

Id Sketch::addLayer(const std::string& name, const std::string& sublayerName)
{
    Id id = 1;
    for (const Layer& l : m_layers) {
        id = std::max(id, l.id + 1);
    }
    Id sub = 1;
    for (const Sublayer& l : m_sublayers) {
        sub = std::max(sub, l.id + 1);
    }
    m_sublayers.push_back({sub, sublayerName, true, id});
    m_layers.insert(m_layers.begin(), Layer{id, name, true, sub});
    return id;
}

void Sketch::renameLayer(Id id, const std::string& name)
{
    for (Layer& l : m_layers) {
        if (l.id == id) {
            l.name = name;
            return;
        }
    }
    throw SketchError("unknown layer " + std::to_string(id));
}

void Sketch::setLayerVisible(Id id, bool visible)
{
    for (Layer& l : m_layers) {
        if (l.id == id) {
            l.visible = visible;
            return;
        }
    }
    throw SketchError("unknown layer " + std::to_string(id));
}

void Sketch::moveLayer(Id id, size_t index)
{
    auto it = std::ranges::find_if(m_layers, [&](const Layer& l) { return l.id == id; });
    if (it == m_layers.end()) {
        throw SketchError("unknown layer " + std::to_string(id));
    }
    const Layer moved = *it;
    m_layers.erase(it);
    m_layers.insert(m_layers.begin() + static_cast<std::ptrdiff_t>(std::min(index, m_layers.size())), moved);
}

void Sketch::removeLayer(Id id)
{
    layer(id);
    if (m_layers.size() == 1) {
        throw SketchError("the last layer can't be deleted");
    }
    const std::vector<Id> own = sublayersOf(id);
    // Leave one sublayer to take the rest down with, then the layer itself.
    const bool wasActive = activeLayer() == id;
    const auto at = static_cast<size_t>(std::ranges::find_if(m_layers, [&](const Layer& l) { return l.id == id; })
                                        - m_layers.begin());
    for (size_t i = 1; i < own.size(); ++i) {
        removeSublayer(own[i]);
    }
    // (the last one: as removeSublayer does, but without the "last sublayer" rule)
    const Id last = own.front();
    std::erase_if(m_constraints, [&](const auto& kv) { return kv.second.sublayer == last; });
    std::erase_if(m_drawnDimensions, [&](const auto& kv) { return kv.second.sublayer == last; });
    std::vector<Id> doomed;
    for (const auto& [tid, t] : m_texts) {
        if (t.sublayer == last) {
            doomed.push_back(tid);
        }
    }
    for (int points = 0; points < 2; ++points) {
        for (const auto& [eid, e] : m_entities) {
            if (e.sublayer == last && (e.kind() == EntityKind::Point) == (points == 1)) {
                doomed.push_back(eid);
            }
        }
    }
    for (Id d : doomed) {
        if ((hasEntity(d) && !isReference(d)) || hasText(d)) {
            remove(d);
        }
    }
    std::erase_if(m_fills, [&](const auto& kv) { return kv.second.layer == id; });
    std::erase_if(m_images, [&](const auto& kv) { return kv.second.layer == id; });
    std::erase_if(m_sublayers, [&](const Sublayer& l) { return l.id == last; });
    m_layers.erase(m_layers.begin() + static_cast<std::ptrdiff_t>(at));
    if (wasActive) {
        setActiveLayer(m_layers[std::min(at, m_layers.size() - 1)].id);
    }
}

std::vector<Id> Sketch::sublayersOf(Id layerId) const
{
    std::vector<Id> out;
    for (const Sublayer& l : m_sublayers) {
        if (l.layer == layerId) {
            out.push_back(l.id);
        }
    }
    return out;
}

Id Sketch::layerOfSublayer(Id sublayerId) const
{
    for (const Sublayer& l : m_sublayers) {
        if (l.id == sublayerId) {
            return l.layer;
        }
    }
    return kNoId;
}

Id Sketch::layerOf(Id item) const
{
    if (const auto f = m_fills.find(item); f != m_fills.end()) {
        return f->second.layer;
    }
    if (const auto i = m_images.find(item); i != m_images.end()) {
        return i->second.layer;
    }
    const Id sub = sublayerOf(item);
    return sub == kNoId ? kNoId : layerOfSublayer(sub);
}

void Sketch::setLayers(std::vector<Layer> layers)
{
    if (layers.empty()) {
        throw SketchError("a sketch needs a layer");
    }
    m_layers = std::move(layers);
    for (Layer& l : m_layers) {
        const std::vector<Id> own = sublayersOf(l.id);
        if (own.empty()) {
            throw SketchError("layer " + l.name + " has no sublayers");
        }
        if (std::ranges::find(own, l.activeSublayer) == own.end()) {
            l.activeSublayer = own.front();
        }
    }
    for (const Sublayer& s : m_sublayers) {
        if (!hasLayer(s.layer)) {
            throw SketchError("sublayer " + s.name + " is in an unknown layer");
        }
    }
    if (hasSublayer(m_activeSublayer)) {
        setActiveSublayer(m_activeSublayer);
    }
}

void Sketch::requireLayer(const std::vector<Id>& items, Id layerId) const
{
    for (Id id : items) {
        if (id == kNoId || isReference(id) || !(hasEntity(id) || hasConstraint(id) || hasText(id))) {
            continue;
        }
        const Id other = layerOf(id);
        if (other != kNoId && other != layerId) {
            const std::string name = hasLayer(other) ? layer(other).name : std::to_string(other);
            throw SketchError("that's on another layer (" + name + "): project it into this one first");
        }
    }
}

// --- projections ---

bool Sketch::isProjection(Id id) const
{
    const auto it = m_entities.find(id);
    return it != m_entities.end() && it->second.projectedFrom != kNoId;
}

bool Sketch::isLinked(Id id) const
{
    const auto it = m_entities.find(id);
    if (it == m_entities.end() || it->second.projectedFrom == kNoId) {
        return false;
    }
    const auto src = m_entities.find(it->second.projectedFrom);
    return src != m_entities.end() && src->second.kind() == it->second.kind();
}

bool Sketch::dependsOn(Id layerId, Id other) const
{
    // Layers each layer takes projections from.
    std::map<Id, std::set<Id>> from;
    for (const auto& [id, e] : m_entities) {
        if (e.projectedFrom != kNoId && hasEntity(e.projectedFrom)) {
            const Id src = layerOf(e.projectedFrom), dst = layerOf(id);
            if (src != kNoId && dst != kNoId && src != dst) {
                from[dst].insert(src);
            }
        }
    }
    std::set<Id> seen;
    std::vector<Id> todo{layerId};
    while (!todo.empty()) {
        const Id l = todo.back();
        todo.pop_back();
        for (Id s : from[l]) {
            if (s == other) {
                return true;
            }
            if (seen.insert(s).second) {
                todo.push_back(s);
            }
        }
    }
    return false;
}

Id Sketch::project(Id source)
{
    const Entity& src = entity(source);  // (throws if unknown)
    if (isReference(source)) {
        throw SketchError("the origin and axes are in every layer already");
    }
    const Id here = activeLayer();
    const Id there = layerOf(source);
    if (there == here) {
        throw SketchError("that's in this layer already");
    }
    if (dependsOn(there, here)) {
        throw SketchError("that layer uses this one's geometry: projecting back would make them depend on each other");
    }
    for (const auto& [id, e] : m_entities) {
        if (e.projectedFrom == source && layerOf(id) == here) {
            return id;
        }
    }
    const Entity copy = src;  // (adding may move it)
    // Its points: projected too, but the origin (and the axes' ends), in every layer, are shared.
    auto point = [this](Id p) { return isReference(p) ? p : project(p); };
    Id id = kNoId;
    if (const auto* p = std::get_if<Point>(&copy.geom)) {
        id = addPoint(p->x, p->y);
    }
    else if (const auto* l = std::get_if<Line>(&copy.geom)) {
        const Id a = point(l->p1);
        id = addLine(a, point(l->p2));
    }
    else if (const auto* c = std::get_if<Circle>(&copy.geom)) {
        id = addCircle(point(c->center), c->radius);
    }
    else if (const auto* a = std::get_if<Arc>(&copy.geom)) {
        const Id centre = point(a->center);
        const Id start = point(a->start);
        id = addArc(centre, start, point(a->end));
    }
    else if (const auto* el = std::get_if<Ellipse>(&copy.geom)) {
        const Id centre = point(el->center);
        const Id major = point(el->major);
        id = addEllipse(centre, major, point(el->minor));
    }
    else if (const auto* ea = std::get_if<EllipseArc>(&copy.geom)) {
        const Id centre = point(ea->center);
        const Id major = point(ea->major);
        const Id minor = point(ea->minor);
        const Id start = point(ea->start);
        id = addEllipseArc(centre, major, minor, start, point(ea->end));
    }
    else if (const auto* sp = std::get_if<Spline>(&copy.geom)) {
        // Its points (and a piece's ends) projected: the same curve, following the original's.
        Spline projected = *sp;
        for (Id& q : projected.points) {
            q = point(q);
        }
        if (projected.isPiece()) {
            projected.start = point(sp->start);
            projected.end = point(sp->end);
        }
        id = addSpline(projected);
    }
    if (id == kNoId) {
        throw SketchError("that can't be projected");
    }
    Entity& e = m_entities.at(id);
    e.projectedFrom = source;
    e.fixed = true;
    e.construction = e.kind() != EntityKind::Point;  // reference geometry, until made solid
    return id;
}

void Sketch::breakLink(Id id)
{
    Entity& e = entity(id);
    if (e.projectedFrom == kNoId) {
        return;
    }
    e.projectedFrom = kNoId;
    e.fixed = false;
    for (Id p : definingPoints(id)) {
        // (its points, unless another projection is built on them too)
        const bool shared = std::ranges::any_of(m_entities, [&](const auto& kv) {
            if (kv.first == id || kv.second.projectedFrom == kNoId || kv.second.kind() == EntityKind::Point) {
                return false;
            }
            const std::vector<Id> on = definingPoints(kv.first);
            return std::ranges::find(on, p) != on.end();
        });
        if (p != id && !shared) {
            Entity& pe = m_entities.at(p);
            if (pe.projectedFrom != kNoId) {
                pe.projectedFrom = kNoId;
                pe.fixed = false;
            }
        }
    }
}

bool Sketch::syncProjections(Id layerId)
{
    bool moved = false;
    for (auto& [id, e] : m_entities) {
        if (e.projectedFrom == kNoId || (layerId != kNoId && layerOf(id) != layerId)) {
            continue;
        }
        const auto src = m_entities.find(e.projectedFrom);
        if (src == m_entities.end() || src->second.kind() != e.kind()) {
            continue;  // unlinked: stays put
        }
        if (auto* p = std::get_if<Point>(&e.geom)) {
            const Point& to = std::get<Point>(src->second.geom);
            if (p->x != to.x || p->y != to.y) {
                *p = to;
                moved = true;
            }
        }
        else if (auto* c = std::get_if<Circle>(&e.geom)) {
            const double r = std::get<Circle>(src->second.geom).radius;
            if (c->radius != r) {
                c->radius = r;
                moved = true;
            }
        }
    }
    return moved;
}

bool Sketch::isReference(Id id) const
{
    return isOrigin(id) || isAxis(id) || isHiddenReference(id) || isPageFrame(id);
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
            else if constexpr (std::is_same_v<T, Ellipse>) {
                return {g.center, g.major, g.minor};
            }
            else if constexpr (std::is_same_v<T, EllipseArc>) {
                return {g.center, g.major, g.minor, g.start, g.end};
            }
            else if constexpr (std::is_same_v<T, Spline>) {
                std::vector<Id> all = g.points;
                if (g.isPiece()) {
                    all.push_back(g.start);
                    all.push_back(g.end);
                }
                return all;
            }
            else {
                return {};
            }
        },
        e.geom);
    if (const auto* sp = std::get_if<Spline>(&e.geom)) {
        if (sp->points.size() < (sp->closed ? 3u : 2u)) {
            throw SketchError(std::string("spline ") + std::to_string(e.id) + " needs at least "
                              + (sp->closed ? "three points (closed)" : "two points"));
        }
        if ((sp->start == kNoId) != (sp->end == kNoId)) {
            throw SketchError("spline " + std::to_string(e.id) + ": a piece needs both ends");
        }
    }
    for (Id p : points) {
        if (!hasEntity(p) || kindOf(p) != EntityKind::Point) {
            throw SketchError(std::string(toString(e.kind())) + " " + std::to_string(e.id) + " refers to missing point "
                              + std::to_string(p));
        }
    }
    // (An elliptical arc may end on its own axis points: they're on the ellipse.)
    std::vector<Id> mustDiffer = points;
    if (const auto* spl = std::get_if<Spline>(&e.geom)) {
        // (a piece may end on one of its points: one it passes through, or the first or last)
        mustDiffer = spl->points;
        if (spl->isPiece() && spl->start == spl->end) {
            throw SketchError("spline " + std::to_string(e.id) + ": a piece's ends must differ");
        }
    }
    if (const auto* ea = std::get_if<EllipseArc>(&e.geom)) {
        mustDiffer = {ea->center, ea->major, ea->minor};
        if (ea->start == ea->end || ea->start == ea->center || ea->end == ea->center) {
            throw SketchError("elliptical arc " + std::to_string(e.id) + " repeats a point");
        }
    }
    if (std::set<Id>(mustDiffer.begin(), mustDiffer.end()).size() != mustDiffer.size()) {
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
    Entity e{id, Point{x, y}};
    e.sublayer = m_activeSublayer;
    m_entities.emplace(id, std::move(e));
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
    requireLayer({p1, p2}, activeLayer());
    const Id id = nextId();
    Entity line{id, Line{p1, p2}};
    line.sublayer = m_activeSublayer;
    m_entities.emplace(id, std::move(line));
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
    requireLayer({center}, activeLayer());
    const Id id = nextId();
    Entity circle{id, Circle{center, radius}};
    circle.sublayer = m_activeSublayer;
    m_entities.emplace(id, std::move(circle));
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
    requireLayer({center, start, end}, activeLayer());
    const Id id = nextId();
    Entity arc{id, Arc{center, start, end}};
    arc.sublayer = m_activeSublayer;
    m_entities.emplace(id, std::move(arc));
    return id;
}

Id Sketch::addEllipse(Id center, Id major, Id minor)
{
    requireLayer({center, major, minor}, activeLayer());
    Entity e{nextId(), Ellipse{center, major, minor}};
    e.sublayer = m_activeSublayer;
    try {
        checkGeometry(e);
    }
    catch (...) {
        --m_nextId;
        throw;
    }
    m_entities.emplace(e.id, e);
    return e.id;
}

Id Sketch::addEllipseArc(Id center, Id major, Id minor, Id start, Id end)
{
    requireLayer({center, major, minor, start, end}, activeLayer());
    Entity e{nextId(), EllipseArc{center, major, minor, start, end}};
    e.sublayer = m_activeSublayer;
    try {
        checkGeometry(e);
    }
    catch (...) {
        --m_nextId;
        throw;
    }
    m_entities.emplace(e.id, e);
    return e.id;
}

Id Sketch::addSpline(const Spline& spline)
{
    std::vector<Id> on = spline.points;
    on.push_back(spline.start);
    on.push_back(spline.end);
    requireLayer(on, activeLayer());
    Entity e{nextId(), spline};
    e.sublayer = m_activeSublayer;
    try {
        checkGeometry(e);
    }
    catch (...) {
        --m_nextId;
        throw;
    }
    m_entities.emplace(e.id, e);
    return e.id;
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
    requireLayer(refs, activeLayer());

    const Id id = nextId();
    Constraint c{id, type, std::move(refs), value};
    c.params = std::move(params);
    c.sublayer = m_activeSublayer;
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
            // (point, point, line) in any click order; two circles or arcs and a line, likewise.
            // (Three lines: the last is the line of symmetry.)
            if (std::ranges::count_if(refs, isLine) == 1) {
                std::ranges::stable_partition(refs, [&](Id id) { return !isLine(id); });
            }
            break;
        default: break;
    }
    // The gap between two circles' radii: the bigger one first (it's that one less the other).
    if (type == ConstraintType::Distance && refs.size() == 2 && isRound(refs[0]) && isRound(refs[1])) {
        auto radius = [&](Id id) {
            return kindOf(id) == EntityKind::Circle ? circle(id).radius : arcRadius(id);
        };
        if (radius(refs[0]) < radius(refs[1])) {
            std::swap(refs[0], refs[1]);
        }
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
        case ConstraintType::Tangent: {
            // Lines, circles and arcs as PlaneGCS has them; ellipses and splines with any curve.
            auto fancy = [&](Id r) { return !isRound(r) && kindOf(r) != K::Line; };  // ellipse or spline
            const bool curvy = refs.size() == 2 && isCurve(refs[0]) && isCurve(refs[1]) && (fancy(refs[0]) || fancy(refs[1]));
            if (!((refs.size() == 2 && kindOf(refs[0]) == K::Line && isRound(refs[1])) || roundPair() || curvy)) {
                fail("expects two curves, not both lines");
            }
            break;
        }
        case ConstraintType::Equal:
            if (!(kinds({K::Line, K::Line}) || roundPair() || kinds({K::Point, K::Point, K::Point}))) {
                fail("expects (line, line), (circle|arc, circle|arc) or (point, point, point)");
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
            if (!(kinds({K::Point, K::Point, K::Line}) || kinds({K::Line, K::Line, K::Line})
                  || kinds({K::Circle, K::Circle, K::Line}) || kinds({K::Arc, K::Arc, K::Line}))) {
                fail("expects two points, two lines, two circles or two arcs, then the line of symmetry");
            }
            break;
        case ConstraintType::Distance:
            if (!(kinds({K::Point, K::Point}) || kinds({K::Point, K::Line}) || kinds({K::Line}))) {
                if (!roundPair()) {
                    fail("expects (point, point), (point, line), (line) or two circles/arcs (the gap between their radii)");
                }
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
            if (!(refs.size() == 1 && (isRound(refs[0]) || isElliptic(refs[0])))) {
                fail("expects (circle|arc|ellipse)");  // (an ellipse's: its major semi-axis)
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
        case ConstraintType::Group: {
            size_t points = 0;
            while (points < refs.size() && kindOf(refs[points]) == K::Point) {
                ++points;
            }
            for (size_t i = points; i < refs.size(); ++i) {
                if (kindOf(refs[i]) != K::Circle) {
                    fail("expects points, then circles");
                }
            }
            if (points < 2) {
                fail("needs at least two points");
            }
            if (params.size() != 1 + 2 * (points - 2) + (refs.size() - points) || !(params[0] > 0.0)) {
                fail("its shape doesn't match its members (make it with group::create)");
            }
            break;
        }
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
                    // (an ellipse's or spline's offset is a spline)
                    const EntityKind source = kindOf(curves[i]), made = kindOf(curves[g * n + i]);
                    const bool curvy = source == EntityKind::Ellipse || source == EntityKind::EllipseArc
                        || source == EntityKind::Spline;
                    if (made != source && !(curvy && made == EntityKind::Spline)) {
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
    if (m_constraints.erase(id) > 0 || m_fills.erase(id) > 0 || m_images.erase(id) > 0
        || m_drawnDimensions.erase(id) > 0) {
        return;
    }
    if (auto t = m_texts.find(id); t != m_texts.end()) {
        // The text, and its anchor too unless something else is built on it. A box text takes
        // its box (the construction lines between its corners) with it.
        const Id anchor = t->second.anchor;
        if (t->second.inBox()) {
            const std::vector<Id> corners = textPoints(*this, t->second);
            m_texts.erase(t);
            std::vector<Id> sides;
            for (const auto& [eid, e] : m_entities) {
                const auto* l = std::get_if<Line>(&e.geom);
                if (l && e.construction && std::ranges::find(corners, l->p1) != corners.end()
                    && std::ranges::find(corners, l->p2) != corners.end()) {
                    sides.push_back(eid);
                }
            }
            for (Id side : sides) {
                if (hasEntity(side)) {
                    remove(side);  // (corners nothing else uses go with them)
                }
            }
            for (Id corner : corners) {
                if (hasEntity(corner) && !isReference(corner) && !isPointUsed(corner)) {
                    remove(corner);
                }
            }
            return;
        }
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
        throw SketchError(isOrigin(id)      ? "the origin can't be deleted"
                          : isPageFrame(id) ? "the page's edges can't be deleted (they go with the page)"
                                            : "the axes can't be deleted");
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

    // Points of deleted curves that no surviving curve uses, and that aren't a surviving divide's
    // own points (a curve drawn through them doesn't take them, nor the divide, with it).
    // A deleted line's divide points go with it (unless a surviving curve is drawn through them).
    std::set<Id> divided, candidates;
    for (const auto& [cid, c] : m_constraints) {
        if (c.type == ConstraintType::Divide && !c.refs.empty()) {
            (doomed.contains(c.refs.front()) ? candidates : divided).insert(c.refs.begin() + 1, c.refs.end());
        }
    }
    for (Id d : doomed) {
        if (kindOf(d) != EntityKind::Point) {
            for (Id p : definingPoints(d)) {
                candidates.insert(p);
            }
        }
    }
    for (Id p : candidates) {
        if (isReference(p) || divided.contains(p)) {
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
    // A text along a chain: cut short where a curve of it goes (it goes with its first).
    for (auto& [key, t] : m_texts) {
        const auto gone = std::ranges::find_if(t.pathMore, [&](Id c) { return doomed.contains(c); });
        t.pathMore.erase(gone, t.pathMore.end());
    }
    std::erase_if(m_texts, [&](const auto& kv) {
        return doomed.contains(kv.second.anchor) || doomed.contains(kv.second.path) || doomed.contains(kv.second.boxSide)
            || doomed.contains(kv.second.boxCorner);
    });
    std::erase_if(m_constraints, [&](const auto& kv) {
        return std::ranges::any_of(kv.second.refs, [&](Id r) { return doomed.contains(r); });
    });
    std::erase_if(m_drawnDimensions, [&](const auto& kv) {
        return std::ranges::any_of(kv.second.refs, [&](Id r) { return doomed.contains(r); });
    });
}

void Sketch::rebindPoint(Id curve, Id oldPoint, Id newPoint)
{
    if (kindOf(newPoint) != EntityKind::Point) {
        throw SketchError("rebind: not a point");
    }
    requireLayer({newPoint}, layerOf(curve));
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
    else if (auto* el = std::get_if<Ellipse>(&e.geom)) {
        swap(el->center);
        swap(el->major);
        swap(el->minor);
    }
    else if (auto* ea = std::get_if<EllipseArc>(&e.geom)) {
        swap(ea->center);
        swap(ea->major);
        swap(ea->minor);
        swap(ea->start);
        swap(ea->end);
    }
    else if (auto* sp = std::get_if<Spline>(&e.geom)) {
        for (Id& p : sp->points) {
            swap(p);
        }
        swap(sp->start);
        swap(sp->end);
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
    if (!(style.width >= 0.0) && style.width != StrokeStyle::kNone) {
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
    if (!hasLayer(f.layer)) {
        f.layer = activeLayer();
    }
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

Point Image::at(double u, double v) const
{
    const double a = angle * std::numbers::pi / 180.0;
    const double dx = (u - 0.5) * width, dy = (v - 0.5) * height();
    return {x + dx * std::cos(a) - dy * std::sin(a), y + dx * std::sin(a) + dy * std::cos(a)};
}

Point Image::fractions(const Point& world) const
{
    const double a = angle * std::numbers::pi / 180.0;
    const double dx = world.x - x, dy = world.y - y;
    const double lx = dx * std::cos(a) + dy * std::sin(a), ly = -dx * std::sin(a) + dy * std::cos(a);
    return {lx / width + 0.5, ly / height() + 0.5};
}

bool Image::contains(const Point& world) const
{
    const Point f = fractions(world);
    return f.x >= 0.0 && f.x <= 1.0 && f.y >= 0.0 && f.y <= 1.0;
}

Id Sketch::addImage(const Image& image)
{
    Image i = image;
    i.id = nextId();
    if (!hasLayer(i.layer)) {
        i.layer = activeLayer();
    }
    if (!(i.width > 0.0) || i.pixelWidth <= 0 || i.pixelHeight <= 0) {
        throw SketchError("an image needs a size");
    }
    m_images.emplace(i.id, std::move(i));
    return m_nextId - 1;
}

void Sketch::setImage(const Image& image)
{
    auto it = m_images.find(image.id);
    if (it == m_images.end()) {
        throw SketchError("unknown image " + std::to_string(image.id));
    }
    if (!(image.width > 0.0)) {
        throw SketchError("an image's width must be more than 0");
    }
    it->second = image;
}

void Sketch::insertImage(const Image& image)
{
    if (image.id == kNoId || hasEntity(image.id) || hasConstraint(image.id) || hasFill(image.id) || hasImage(image.id)
        || hasText(image.id)) {
        throw SketchError("duplicate or invalid id " + std::to_string(image.id));
    }
    m_images.emplace(image.id, image);
    m_nextId = std::max(m_nextId, image.id + 1);
}

const Image& Sketch::image(Id id) const
{
    auto it = m_images.find(id);
    if (it == m_images.end()) {
        throw SketchError("unknown image " + std::to_string(id));
    }
    return it->second;
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

const char* toString(PageOrigin origin)
{
    switch (origin) {
        case PageOrigin::BottomLeft: return "bottom_left";
        case PageOrigin::Center: return "center";
        case PageOrigin::TopLeft: return "top_left";
    }
    return "bottom_left";
}

bool pageOriginFromString(const std::string& name, PageOrigin& out)
{
    for (PageOrigin o : {PageOrigin::BottomLeft, PageOrigin::Center, PageOrigin::TopLeft}) {
        if (name == toString(o)) {
            out = o;
            return true;
        }
    }
    return false;
}

std::array<double, 4> Page::rect() const
{
    switch (origin) {
        case PageOrigin::Center: return {-width / 2, -height / 2, width / 2, height / 2};
        case PageOrigin::TopLeft: return {0.0, -height, width, 0.0};
        case PageOrigin::BottomLeft: break;
    }
    return {0.0, 0.0, width, height};
}

const std::vector<PageSize>& pageSizes()
{
    static const std::vector<PageSize> sizes = {
        {"A0", 841, 1189},  {"A1", 594, 841},     {"A2", 420, 594},       {"A3", 297, 420},
        {"A4", 210, 297},   {"A5", 148, 210},     {"Letter", 215.9, 279.4}, {"Legal", 215.9, 355.6},
        {"Tabloid", 279.4, 431.8},
    };
    return sizes;
}

bool pageSizeByName(const std::string& name, double& width, double& height)
{
    for (const PageSize& s : pageSizes()) {
        if (name == s.name) {
            width = s.width;
            height = s.height;
            return true;
        }
    }
    return false;
}

void Sketch::setPage(std::optional<Page> page)
{
    if (page && !(page->width > 0.0 && page->height > 0.0)) {
        throw SketchError("a page's width and height must be more than 0");
    }
    m_page = std::move(page);
    syncPageFrame();
}

void Sketch::syncPageFrame()
{
    const bool have = m_pageCorners[0] != kNoId && hasEntity(m_pageCorners[0]);
    if (!m_page) {
        if (!have) {
            m_pageCorners = m_pageEdges = {kNoId, kNoId, kNoId, kNoId};
            return;
        }
        // Gone with the page, and what's tied to it (constraints onto its edges).
        std::set<Id> doomed(m_pageCorners.begin(), m_pageCorners.end());
        doomed.insert(m_pageEdges.begin(), m_pageEdges.end());
        for (Id d : doomed) {
            m_entities.erase(d);
        }
        std::erase_if(m_constraints, [&](const auto& kv) {
            return std::ranges::any_of(kv.second.refs, [&](Id r) { return doomed.contains(r); });
        });
        m_pageCorners = m_pageEdges = {kNoId, kNoId, kNoId, kNoId};
        return;
    }
    const std::array<double, 4> r = m_page->rect();
    const std::array<Point, 4> at{Point{r[0], r[1]}, Point{r[2], r[1]}, Point{r[2], r[3]}, Point{r[0], r[3]}};
    if (!have) {
        for (int i = 0; i < 4; ++i) {
            m_pageCorners[i] = addPoint(at[i].x, at[i].y);
            Entity& c = m_entities.at(m_pageCorners[i]);
            c.fixed = true;
            c.sublayer = kNoId;  // (in every layer)
        }
        for (int i = 0; i < 4; ++i) {
            m_pageEdges[i] = addLine(m_pageCorners[i], m_pageCorners[(i + 1) % 4]);
            Entity& e = m_entities.at(m_pageEdges[i]);
            e.fixed = true;
            e.construction = true;
            e.sublayer = kNoId;
        }
        return;
    }
    for (int i = 0; i < 4; ++i) {
        m_entities.at(m_pageCorners[i]).geom = at[i];
    }
}

void Sketch::setPageFrame(const std::array<Id, 4>& corners, const std::array<Id, 4>& edges)
{
    for (int i = 0; i < 4; ++i) {
        if (kindOf(corners[i]) != EntityKind::Point || kindOf(edges[i]) != EntityKind::Line) {
            throw SketchError("the page's frame must be four points and four lines");
        }
        const Line& l = line(edges[i]);
        if (l.p1 != corners[i] || l.p2 != corners[(i + 1) % 4]) {
            throw SketchError("the page's edges must join its corners in turn");
        }
    }
    m_pageCorners = corners;
    m_pageEdges = edges;
    for (int i = 0; i < 4; ++i) {
        for (Id id : {corners[i], edges[i]}) {
            Entity& e = m_entities.at(id);
            e.fixed = true;
            e.sublayer = kNoId;
        }
        m_entities.at(edges[i]).construction = true;
    }
}

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

const char* toString(TextAlign align)
{
    switch (align) {
        case TextAlign::Left: return "left";
        case TextAlign::Center: return "center";
        case TextAlign::Right: return "right";
    }
    return "?";
}

const char* toString(TextVAlign align)
{
    switch (align) {
        case TextVAlign::Baseline: return "baseline";
        case TextVAlign::Top: return "top";
        case TextVAlign::Middle: return "middle";
        case TextVAlign::Bottom: return "bottom";
    }
    return "?";
}

bool textAlignFromString(const std::string& name, TextAlign& out)
{
    for (TextAlign a : {TextAlign::Left, TextAlign::Center, TextAlign::Right}) {
        if (name == toString(a)) {
            out = a;
            return true;
        }
    }
    return false;
}

bool textVAlignFromString(const std::string& name, TextVAlign& out)
{
    for (TextVAlign a : {TextVAlign::Baseline, TextVAlign::Top, TextVAlign::Middle, TextVAlign::Bottom}) {
        if (name == toString(a)) {
            out = a;
            return true;
        }
    }
    return false;
}

std::vector<Id> textPoints(const Sketch& s, const Text& t)
{
    std::vector<Id> out{t.anchor};
    if (!t.inBox()) {
        return out;
    }
    out.push_back(t.boxSide);
    out.push_back(t.boxCorner);
    // The fourth corner: joined by a construction line to both the anchor and the far corner.
    auto ends = [&](Id p) {
        std::vector<Id> others;
        for (const auto& [id, e] : s.entities()) {
            if (const auto* l = std::get_if<Line>(&e.geom); l && e.construction) {
                if (l->p1 == p) {
                    others.push_back(l->p2);
                }
                else if (l->p2 == p) {
                    others.push_back(l->p1);
                }
            }
        }
        return others;
    };
    const std::vector<Id> fromAnchor = ends(t.anchor), fromCorner = ends(t.boxCorner);
    for (Id p : fromAnchor) {
        if (p != t.boxSide && std::ranges::find(fromCorner, p) != fromCorner.end()) {
            out.push_back(p);
            break;
        }
    }
    return out;
}

Point textToWorld(const Point& anchor, double angleDeg, const Point& local)
{
    const double a = angleDeg * std::numbers::pi / 180.0;
    const double c = std::cos(a), s = std::sin(a);
    return {anchor.x + c * local.x - s * local.y, anchor.y + s * local.x + c * local.y};
}

bool quadraticOf(const Point& from, const Point& c1, const Point& c2, const Point& to, Point& control)
{
    // A quadratic's cubic has its control points two thirds of the way to the quadratic's one,
    // from each end.
    const Point a{(3 * c1.x - from.x) / 2, (3 * c1.y - from.y) / 2};
    const Point b{(3 * c2.x - to.x) / 2, (3 * c2.y - to.y) / 2};
    const double scale = std::max({1.0, std::abs(from.x), std::abs(from.y), std::abs(to.x), std::abs(to.y)});
    if (std::hypot(a.x - b.x, a.y - b.y) > 1e-9 * scale) {
        return false;
    }
    control = {(a.x + b.x) / 2, (a.y + b.y) / 2};
    return true;
}

double letterTolerance(double height)
{
    return std::max(1e-5, height * 5e-4);
}

std::vector<std::vector<Point>> flattenLetters(const std::vector<TextContour>& letters, double tolerance)
{
    std::vector<std::vector<Point>> out;
    // A cubic, split in halves until each is flat to within the tolerance (its control points
    // that close to the line between its ends).
    std::function<void(Point, Point, Point, Point, int, std::vector<Point>&)> cubic =
        [&](Point a, Point b, Point c, Point d, int depth, std::vector<Point>& to) {
            auto off = [&](Point p) {
                const double dx = d.x - a.x, dy = d.y - a.y, len = std::hypot(dx, dy);
                return len > 1e-12 ? std::abs(dx * (p.y - a.y) - dy * (p.x - a.x)) / len : std::hypot(p.x - a.x, p.y - a.y);
            };
            if (depth >= 12 || std::max(off(b), off(c)) <= tolerance) {
                to.push_back(d);
                return;
            }
            auto mid = [](Point p, Point q) { return Point{(p.x + q.x) / 2, (p.y + q.y) / 2}; };
            const Point ab = mid(a, b), bc = mid(b, c), cd = mid(c, d), abc = mid(ab, bc), bcd = mid(bc, cd);
            const Point m = mid(abc, bcd);
            cubic(a, ab, abc, m, depth + 1, to);
            cubic(m, bcd, cd, d, depth + 1, to);
        };
    for (const TextContour& c : letters) {
        if (c.points.empty()) {
            continue;
        }
        std::vector<Point> poly{c.points.front()};
        size_t i = 1;
        for (char op : c.ops) {
            if (op == 'C' && i + 2 < c.points.size()) {
                cubic(poly.back(), c.points[i], c.points[i + 1], c.points[i + 2], 0, poly);
                i += 3;
            }
            else if (op == 'L' && i < c.points.size()) {
                poly.push_back(c.points[i]);
                i += 1;
            }
        }
        if (poly.size() > 2 && std::hypot(poly.front().x - poly.back().x, poly.front().y - poly.back().y) < 1e-9) {
            poly.pop_back();  // (closed anyway)
        }
        if (poly.size() >= 3) {
            out.push_back(std::move(poly));
        }
    }
    return out;
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
    if (!std::isfinite(t.angle) || !std::isfinite(t.spacing)) {
        throw SketchError("text: the angle and spacing must be numbers");
    }
    for (Id corner : {t.boxSide, t.boxCorner}) {
        if (corner != kNoId && (!s.hasEntity(corner) || s.entity(corner).kind() != EntityKind::Point)) {
            throw SketchError("text: its box corners must be points");
        }
    }
    if (t.path != kNoId) {
        if (!s.hasEntity(t.path) || s.entity(t.path).kind() == EntityKind::Point || s.isReference(t.path)) {
            throw SketchError("text: the path must be a curve");
        }
        if (s.definingPoints(t.path).front() != t.anchor) {
            throw SketchError("text: text along a path is anchored at the path's first point");
        }
    }
    // A chain: curves with ends (not closed ones), each joined to the one before.
    auto ends = [&](Id c) -> std::vector<Id> {
        const Entity& e = s.entity(c);
        if (const auto* l = std::get_if<Line>(&e.geom)) {
            return {l->p1, l->p2};
        }
        if (const auto* a = std::get_if<Arc>(&e.geom)) {
            return {a->start, a->end};
        }
        if (const auto* ea = std::get_if<EllipseArc>(&e.geom)) {
            return {ea->start, ea->end};
        }
        if (const auto* sp = std::get_if<Spline>(&e.geom)) {
            if (sp->isPiece()) {
                return {sp->start, sp->end};
            }
            if (!sp->closed) {
                return {sp->points.front(), sp->points.back()};
            }
        }
        return {};
    };
    Id before = t.path;
    for (Id c : t.pathMore) {
        if (t.path == kNoId || !s.hasEntity(c) || s.entity(c).kind() == EntityKind::Point || s.isReference(c)) {
            throw SketchError("text: a path's curves must be curves");
        }
        const std::vector<Id> a = ends(before), b = ends(c);
        if (a.empty() || b.empty() || std::ranges::none_of(a, [&](Id p) { return std::ranges::find(b, p) != b.end(); })) {
            throw SketchError("text: a path's curves must be joined end to end, in order (and not be closed ones)");
        }
        before = c;
    }
}
}  // namespace

Id Sketch::addText(const Text& text)
{
    checkText(*this, text);
    std::vector<Id> on = textPoints(*this, text);
    on.push_back(text.path);
    on.insert(on.end(), text.pathMore.begin(), text.pathMore.end());
    requireLayer(on, activeLayer());
    Text t = text;
    t.id = nextId();
    t.sublayer = m_activeSublayer;
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
    std::vector<Id> on = textPoints(*this, text);
    on.push_back(text.path);
    on.insert(on.end(), text.pathMore.begin(), text.pathMore.end());
    requireLayer(on, layerOf(text.id));
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

// ---------------------------------------------------------------------------------------------
// Drawn dimensions
// ---------------------------------------------------------------------------------------------

namespace
{
constexpr std::pair<DrawnDimensionKind, const char*> kDrawnKinds[] = {
    {DrawnDimensionKind::Aligned, "aligned"},     {DrawnDimensionKind::Horizontal, "horizontal"},
    {DrawnDimensionKind::Vertical, "vertical"},   {DrawnDimensionKind::Radius, "radius"},
    {DrawnDimensionKind::Diameter, "diameter"},   {DrawnDimensionKind::Angle, "angle"},
    {DrawnDimensionKind::ArcLength, "arc_length"}, {DrawnDimensionKind::Ordinate, "ordinate"},
    {DrawnDimensionKind::Leader, "leader"},
};

// The dimension constraint measuring the same thing, for the kinds that have one.
std::optional<ConstraintType> constraintKind(DrawnDimensionKind kind)
{
    switch (kind) {
        case DrawnDimensionKind::Aligned: return ConstraintType::Distance;
        case DrawnDimensionKind::Horizontal: return ConstraintType::HorizontalDistance;
        case DrawnDimensionKind::Vertical: return ConstraintType::VerticalDistance;
        case DrawnDimensionKind::Radius: return ConstraintType::Radius;
        case DrawnDimensionKind::Diameter: return ConstraintType::Diameter;
        case DrawnDimensionKind::Angle: return ConstraintType::Angle;
        default: return std::nullopt;
    }
}
}  // namespace

const char* toString(DrawnDimensionKind kind)
{
    for (const auto& [k, name] : kDrawnKinds) {
        if (k == kind) {
            return name;
        }
    }
    return "?";
}

bool drawnDimensionKindFromString(const std::string& name, DrawnDimensionKind& out)
{
    for (const auto& [k, n] : kDrawnKinds) {
        if (name == n) {
            out = k;
            return true;
        }
    }
    return false;
}

void Sketch::checkDrawnDimension(DrawnDimension& d) const
{
    for (Id r : d.refs) {
        if (!hasEntity(r)) {
            throw SketchError("a dimension can only measure geometry (" + std::to_string(r) + " isn't)");
        }
    }
    if (const auto type = constraintKind(d.kind)) {
        normalize(*type, d.refs);
        validate(*type, d.refs, 1.0);  // (as that dimension would be)
    }
    else if (d.kind == DrawnDimensionKind::ArcLength) {
        if (d.refs.size() != 1 || kindOf(d.refs[0]) != EntityKind::Arc) {
            throw SketchError("an arc length measures an arc");
        }
    }
    else if (d.kind == DrawnDimensionKind::Ordinate) {
        if (d.refs.size() != 2 || kindOf(d.refs[0]) != EntityKind::Point || kindOf(d.refs[1]) != EntityKind::Point) {
            throw SketchError("an ordinate dimension measures a point from an origin point");
        }
    }
    else if (d.refs.size() > 1) {
        throw SketchError("a leader points at one thing");
    }
    if (!(d.textHeight > 0.0) || d.decimals < -1 || d.decimals > 6 || d.style.width < 0.0) {
        throw SketchError("a dimension's text height must be positive, its decimals -1 to 6");
    }
}

Id Sketch::annotationSublayer(Id layer) const
{
    for (const Sublayer& l : m_sublayers) {
        if (l.layer == layer && l.annotations) {
            return l.id;
        }
    }
    return kNoId;
}

Id Sketch::addDrawnDimension(const DrawnDimension& dimension)
{
    DrawnDimension d = dimension;
    checkDrawnDimension(d);
    requireLayer(d.refs, activeLayer());
    // Into the layer's annotations sublayer, made if it isn't there (named apart from the others).
    Id sub = annotationSublayer(activeLayer());
    if (sub == kNoId) {
        std::string name = "Annotations";
        for (int n = 2; std::ranges::any_of(m_sublayers, [&](const Sublayer& l) {
                 return l.layer == activeLayer() && l.name == name;
             });
             ++n) {
            name = "Annotations " + std::to_string(n);
        }
        sub = addSublayer(name);
        for (Sublayer& l : m_sublayers) {
            if (l.id == sub) {
                l.annotations = true;
            }
        }
    }
    d.id = nextId();
    d.sublayer = sub;
    m_drawnDimensions.emplace(d.id, std::move(d));
    return m_nextId - 1;
}

void Sketch::setDrawnDimension(const DrawnDimension& dimension)
{
    const auto it = m_drawnDimensions.find(dimension.id);
    if (it == m_drawnDimensions.end()) {
        throw SketchError("unknown dimension " + std::to_string(dimension.id));
    }
    DrawnDimension d = dimension;
    checkDrawnDimension(d);
    requireLayer(d.refs, layerOf(d.id));
    d.sublayer = it->second.sublayer;
    it->second = std::move(d);
}

void Sketch::insertDrawnDimension(const DrawnDimension& dimension)
{
    const Id id = dimension.id;
    if (id == kNoId || hasEntity(id) || hasConstraint(id) || hasFill(id) || hasText(id) || hasImage(id)
        || hasDrawnDimension(id)) {
        throw SketchError("duplicate or invalid id " + std::to_string(id));
    }
    DrawnDimension d = dimension;
    checkDrawnDimension(d);
    m_drawnDimensions.emplace(id, std::move(d));
    m_nextId = std::max(m_nextId, id + 1);
}

const DrawnDimension& Sketch::drawnDimension(Id id) const
{
    const auto it = m_drawnDimensions.find(id);
    if (it == m_drawnDimensions.end()) {
        throw SketchError("unknown dimension " + std::to_string(id));
    }
    return it->second;
}

void Sketch::setDimensionDecimals(int places)
{
    if (places < 0 || places > 6) {
        throw SketchError("decimals must be 0 to 6");
    }
    m_dimensionDecimals = places;
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
    // Drawn dimensions follow too; one that no longer makes sense (an arc length on what's now a
    // circle) goes, rather than stopping the edit.
    for (auto it = m_drawnDimensions.begin(); it != m_drawnDimensions.end();) {
        DrawnDimension& d = it->second;
        if (std::ranges::find(d.refs, from) == d.refs.end()) {
            ++it;
            continue;
        }
        std::ranges::replace(d.refs, from, to);
        try {
            checkDrawnDimension(d);
            ++it;
        }
        catch (const SketchError&) {
            it = m_drawnDimensions.erase(it);
        }
    }
}

void Sketch::clear()
{
    m_entities.clear();
    m_constraints.clear();
    m_strokes.clear();
    m_fills.clear();
    m_texts.clear();
    m_images.clear();
    m_drawnDimensions.clear();
    m_dimensionDecimals = 2;
    m_page.reset();
    m_sublayers = {{1, "Sublayer 1", true, 1}};
    m_activeSublayer = 1;
    m_layers = {{1, "Layer 1", true, 1}};
    m_origin = kNoId;
    m_axes = {kNoId, kNoId};
    m_axisEnds = {kNoId, kNoId};
    m_pageCorners = m_pageEdges = {kNoId, kNoId, kNoId, kNoId};
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
const Ellipse& Sketch::ellipse(Id id) const { return geomAs<Ellipse>(entity(id), "ellipse"); }
const EllipseArc& Sketch::ellipseArc(Id id) const { return geomAs<EllipseArc>(entity(id), "elliptical arc"); }
const Spline& Sketch::spline(Id id) const { return geomAs<Spline>(entity(id), "spline"); }

bool Sketch::isElliptic(Id id) const
{
    const EntityKind k = kindOf(id);
    return k == EntityKind::Ellipse || k == EntityKind::EllipseArc;
}

std::array<Id, 3> Sketch::ellipseAxes(Id id) const
{
    const Entity& e = entity(id);
    if (const auto* el = std::get_if<Ellipse>(&e.geom)) {
        return {el->center, el->major, el->minor};
    }
    const EllipseArc& ea = geomAs<EllipseArc>(e, "ellipse");
    return {ea.center, ea.major, ea.minor};
}

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
        case EntityKind::Ellipse: {
            const auto& el = std::get<Ellipse>(e.geom);
            return {el.center, el.major, el.minor};
        }
        case EntityKind::EllipseArc: {
            const auto& ea = std::get<EllipseArc>(e.geom);
            return {ea.center, ea.major, ea.minor, ea.start, ea.end};
        }
        case EntityKind::Spline: {
            const auto& sp = std::get<Spline>(e.geom);
            std::vector<Id> all = sp.points;
            if (sp.isPiece()) {
                all.push_back(sp.start);
                all.push_back(sp.end);
            }
            return all;
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
