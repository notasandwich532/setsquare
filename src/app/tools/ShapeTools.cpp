#include "ShapeTools.h"

#include "../Document.h"
#include "../SketchView.h"

#include <core/Geometry.h>

#include <QPainter>

#include <algorithm>
#include <cmath>
#include <numbers>

using core::Id;
using core::kNoId;
using core::Vec2;
namespace build = core::build;

namespace
{
constexpr double kPi = std::numbers::pi;

double dist(Vec2 a, Vec2 b)
{
    return std::hypot(b.x - a.x, b.y - a.y);
}

// Distance from p to the infinite line through a and b.
double lineDistance(Vec2 p, Vec2 a, Vec2 b)
{
    const double len = dist(a, b);
    if (len == 0.0) {
        return dist(p, a);
    }
    return std::abs((b.x - a.x) * (p.y - a.y) - (b.y - a.y) * (p.x - a.x)) / len;
}

Id resolve(Editor& ed, const Snap& snap)
{
    return resolveSnap(ed, snap);
}

// Snapped onto an existing point: keep it on a new circle's rim.
void pinToRim(Editor& ed, const Snap& snap, Id circle)
{
    if (snap.point != kNoId && ed.sketch().hasEntity(snap.point)) {
        ed.constrain(core::ConstraintType::Coincident, {snap.point, circle});
    }
}
}  // namespace

// =============================================================================================
// StepTool
// =============================================================================================

void drawPreviewEdit(SketchView& view, QPainter& painter, const std::function<void(Editor&)>& edit)
{
    core::Sketch scratch = view.document().sketch();
    const Id before = scratch.highestId();
    try {
        Editor ed(scratch, nullptr);
        edit(ed);
    }
    catch (const core::SketchError&) {
        return;  // degenerate at this mouse position: nothing to show
    }
    view.drawPreviewSketch(painter, scratch, before);
}

Snap StepTool::snapAt(QPointF screen) const
{
    if (snaps(m_clicks.size())) {
        return view().snap(screen);
    }
    return Snap{view().toWorld(screen)};
}

QString StepTool::hint() const
{
    return hintFor(m_clicks.size());
}

void StepTool::press(const ToolEvent& e)
{
    if (e.button != Qt::LeftButton) {
        cancel();
        return;
    }
    m_cursor = snapAt(e.screen);
    tracked(m_cursor);
    m_clicks.push_back(m_cursor);
    if (static_cast<int>(m_clicks.size()) == steps()) {
        const std::vector<Snap> clicks = std::move(m_clicks);
        m_clicks.clear();
        view().tryEdit(label(), [&](Editor& ed) { build(ed, clicks); });
    }
    refresh();
}

void StepTool::move(const ToolEvent& e)
{
    m_cursor = snapAt(e.screen);
    tracked(m_cursor);
    refresh();
}

bool StepTool::cancel()
{
    if (m_clicks.empty()) {
        return false;
    }
    m_clicks.clear();
    refresh();
    return true;
}

void StepTool::paintOverlay(QPainter& painter)
{
    if (!m_clicks.empty()) {
        std::vector<Snap> clicks = m_clicks;
        clicks.push_back(m_cursor);
        if (static_cast<int>(clicks.size()) == steps()) {
            drawPreviewEdit(view(), painter, [&](Editor& ed) { build(ed, clicks); });
        }
        else {
            // Not enough clicks for the shape yet: show the rubber band from the last one.
            view().drawPreviewLine(painter, m_clicks.back().pos, m_cursor.pos);
        }
    }
    view().drawSnapMarker(painter, m_cursor);
}

// =============================================================================================
// Simple shapes
// =============================================================================================

QString PointTool::hintFor(size_t) const
{
    return QObject::tr("Point: click to place (on a curve or midpoint to constrain it there)");
}

void PointTool::build(Editor& ed, const std::vector<Snap>& clicks)
{
    if (clicks[0].point != kNoId) {
        throw core::SketchError("there's already a point there");
    }
    resolve(ed, clicks[0]);
}

QString Rectangle3Tool::hintFor(size_t step) const
{
    switch (step) {
        case 0: return QObject::tr("3-point rectangle: click the first corner");
        case 1: return QObject::tr("Click the second corner (sets the angle)");
        default: return QObject::tr("Click to set the width");
    }
}

void Rectangle3Tool::build(Editor& ed, const std::vector<Snap>& c)
{
    const Id a = resolve(ed, c[0]);
    const Id b = resolve(ed, c[1]);
    ed.rectangle3(a, b, c[2].pos);
}

QString RectangleCenterTool::hintFor(size_t step) const
{
    return step == 0 ? QObject::tr("Centre rectangle: click the centre") : QObject::tr("Click a corner");
}

void RectangleCenterTool::build(Editor& ed, const std::vector<Snap>& c)
{
    ed.rectangleCenter(resolve(ed, c[0]), c[1].pos);
}

QString Circle2Tool::hintFor(size_t step) const
{
    return step == 0 ? QObject::tr("2-point circle: click one end of a diameter") : QObject::tr("Click the other end");
}

void Circle2Tool::build(Editor& ed, const std::vector<Snap>& c)
{
    const Id circle = ed.circle2(c[0].pos, c[1].pos);
    pinToRim(ed, c[0], circle);
    pinToRim(ed, c[1], circle);
}

QString Circle3Tool::hintFor(size_t step) const
{
    switch (step) {
        case 0: return QObject::tr("3-point circle: click a point on the circle");
        case 1: return QObject::tr("Click a second point");
        default: return QObject::tr("Click a third point");
    }
}

void Circle3Tool::build(Editor& ed, const std::vector<Snap>& c)
{
    const Id circle = ed.circle3(c[0].pos, c[1].pos, c[2].pos);
    for (const Snap& s : c) {
        pinToRim(ed, s, circle);
    }
}

QString PolygonTool::hintFor(size_t step) const
{
    if (step == 0) {
        return QObject::tr("Polygon (%1 sides): click the centre").arg(view().polygonSides());
    }
    return m_kind == build::PolygonKind::Inscribed ? QObject::tr("Click a corner")
                                                   : QObject::tr("Click the middle of a side");
}

void PolygonTool::build(Editor& ed, const std::vector<Snap>& c)
{
    ed.polygon(resolve(ed, c[0]), c[1].pos, view().polygonSides(), m_kind);
}

QString PolygonEdgeTool::hintFor(size_t step) const
{
    switch (step) {
        case 0: return QObject::tr("Edge polygon (%1 sides): click one end of an edge").arg(view().polygonSides());
        case 1: return QObject::tr("Click the other end of the edge");
        default: return QObject::tr("Click the side the polygon goes on");
    }
}

void PolygonEdgeTool::build(Editor& ed, const std::vector<Snap>& c)
{
    const Id a = resolve(ed, c[0]);
    const Id b = resolve(ed, c[1]);
    const Vec2 pa = c[0].pos, pb = c[1].pos, side = c[2].pos;
    const bool left = (pb.x - pa.x) * (side.y - pa.y) - (pb.y - pa.y) * (side.x - pa.x) >= 0;
    ed.polygonEdge(a, b, view().polygonSides(), left);
}

// =============================================================================================
// Slots
// =============================================================================================

bool SlotTool::snaps(size_t step) const
{
    switch (m_kind) {
        case Kind::CenterToCenter: return step < 2;
        case Kind::Overall: return false;
        case Kind::CenterPoint: return step == 0;
    }
    return false;
}

QString SlotTool::hintFor(size_t step) const
{
    if (step == 2) {
        return QObject::tr("Click to set the width");
    }
    switch (m_kind) {
        case Kind::CenterToCenter:
            return step == 0 ? QObject::tr("Slot: click the first end's centre") : QObject::tr("Click the other end's centre");
        case Kind::Overall:
            return step == 0 ? QObject::tr("Overall slot: click one end") : QObject::tr("Click the other end");
        case Kind::CenterPoint:
            return step == 0 ? QObject::tr("Centre-point slot: click the centre") : QObject::tr("Click an end's centre");
    }
    return {};
}

void SlotTool::build(Editor& ed, const std::vector<Snap>& c)
{
    const double width = 2 * lineDistance(c[2].pos, c[0].pos, c[1].pos);
    switch (m_kind) {
        case Kind::CenterToCenter: {
            const Id a = resolve(ed, c[0]);
            ed.slot(a, resolve(ed, c[1]), width);
            break;
        }
        case Kind::Overall: ed.slotOverall(c[0].pos, c[1].pos, width); break;
        case Kind::CenterPoint: ed.slotCenterPoint(resolve(ed, c[0]), c[1].pos, width); break;
    }
}

QString ArcSlotTool::hintFor(size_t step) const
{
    if (m_threePoint) {
        switch (step) {
            case 0: return QObject::tr("3-point arc slot: click the start of the centreline");
            case 1: return QObject::tr("Click the end of the centreline");
            case 2: return QObject::tr("Click a point the centreline passes through");
            default: return QObject::tr("Click to set the width");
        }
    }
    switch (step) {
        case 0: return QObject::tr("Centre-point arc slot: click the centre");
        case 1: return QObject::tr("Click the start of the centreline (sets its radius)");
        case 2: return QObject::tr("Click the end; the sweep follows the mouse");
        default: return QObject::tr("Click to set the width");
    }
}

void ArcSlotTool::tracked(const Snap& cursor)
{
    if (m_threePoint || m_clicks.empty()) {
        return;
    }
    const Vec2 c = m_clicks[0].pos;
    const double angle = std::atan2(cursor.pos.y - c.y, cursor.pos.x - c.x);
    if (m_clicks.size() == 1) {
        m_sweep = 0.0;  // still choosing the start
    }
    else if (m_clicks.size() == 2) {
        double delta = angle - m_lastAngle;
        if (delta > kPi) {
            delta -= 2 * kPi;
        }
        else if (delta < -kPi) {
            delta += 2 * kPi;
        }
        m_sweep = std::clamp(m_sweep + delta, -2 * kPi + 1e-3, 2 * kPi - 1e-3);
    }
    m_lastAngle = angle;
}

void ArcSlotTool::build(Editor& ed, const std::vector<Snap>& c)
{
    if (m_threePoint) {
        Vec2 centre;
        double radius = 0;
        if (!core::circumcircle(c[0].pos, c[2].pos, c[1].pos, centre, radius)) {
            throw core::SketchError("3-point arc slot: the points are in a line");
        }
        ed.arcSlot3(c[0].pos, c[1].pos, c[2].pos, 2 * std::abs(dist(c[3].pos, centre) - radius));
        return;
    }
    const Vec2 centre = c[0].pos;
    const double radius = dist(c[1].pos, centre);
    const double width = 2 * std::abs(dist(c[3].pos, centre) - radius);
    const Id centreId = resolve(ed, c[0]);
    // The model arc runs counter-clockwise; a clockwise sweep swaps the ends.
    if (m_sweep >= 0) {
        ed.arcSlot(centreId, c[1].pos, c[2].pos, width);
    }
    else {
        ed.arcSlot(centreId, c[2].pos, c[1].pos, width);
    }
}

// =============================================================================================
// Tangent circles
// =============================================================================================

QString CircleTangentTool::hint() const
{
    if (static_cast<int>(m_picks.size()) < m_count) {
        return QObject::tr("Tangent circle: pick %1 lines, circles or arcs (%2 so far)")
            .arg(m_count)
            .arg(m_picks.size());
    }
    return m_count == 2 ? QObject::tr("Click to place; the mouse sets the radius")
                        : QObject::tr("Click on the side where the circle should go");
}

double CircleTangentTool::radiusAt(Vec2 at) const
{
    double sum = 0;
    for (Id c : m_picks) {
        sum += core::distanceTo(doc().sketch(), c, at);
    }
    return std::max(sum / static_cast<double>(m_picks.size()), 1e-3);
}

void CircleTangentTool::apply(Editor& ed, Vec2 at) const
{
    ed.circleTangent(m_picks, at, m_count == 2 ? radiusAt(at) : 0.0);
}

void CircleTangentTool::press(const ToolEvent& e)
{
    if (e.button != Qt::LeftButton) {
        cancel();
        return;
    }
    m_mouse = e.world;
    if (static_cast<int>(m_picks.size()) < m_count) {
        core::PickOptions options;
        options.tolerance = 7.0 / view().zoom();
        options.points = false;
        const Id hit = core::pick(doc().sketch(), e.world, options);
        if (hit != kNoId && std::ranges::find(m_picks, hit) == m_picks.end()) {
            m_picks.push_back(hit);
            view().setSelection({m_picks.begin(), m_picks.end()});
        }
        refresh();
        return;
    }
    view().tryEdit(QObject::tr("Circle"), [&](Editor& ed) { apply(ed, e.world); });
    m_picks.clear();
    view().setSelection({});
    refresh();
}

void CircleTangentTool::move(const ToolEvent& e)
{
    m_mouse = e.world;
    refresh();
}

bool CircleTangentTool::cancel()
{
    if (m_picks.empty()) {
        return false;
    }
    m_picks.clear();
    view().setSelection({});
    refresh();
    return true;
}

void CircleTangentTool::paintOverlay(QPainter& painter)
{
    if (static_cast<int>(m_picks.size()) == m_count) {
        drawPreviewEdit(view(), painter, [&](Editor& ed) { apply(ed, m_mouse); });
    }
}

// =============================================================================================
// Tangent arc
// =============================================================================================

QString ArcTangentTool::hint() const
{
    return m_curve == kNoId ? QObject::tr("Tangent arc: click the end of a line or arc")
                            : QObject::tr("Click where the arc ends");
}

void ArcTangentTool::press(const ToolEvent& e)
{
    if (e.button != Qt::LeftButton) {
        cancel();
        return;
    }
    const core::Sketch& s = doc().sketch();
    if (m_curve == kNoId) {
        const Snap snap = view().snap(e.screen);
        if (snap.point == kNoId) {
            emit view().message(QObject::tr("Click on the end of a line or arc"));
            return;
        }
        // The line or arc this point is an end of (the most recent one, if several).
        for (auto it = s.entities().rbegin(); it != s.entities().rend(); ++it) {
            const core::Entity& ent = it->second;
            const bool isEnd = (ent.kind() == core::EntityKind::Line
                                && (s.line(ent.id).p1 == snap.point || s.line(ent.id).p2 == snap.point))
                || (ent.kind() == core::EntityKind::Arc
                    && (s.arc(ent.id).start == snap.point || s.arc(ent.id).end == snap.point));
            if (isEnd && !ent.construction) {
                m_curve = ent.id;
                m_start = snap.point;
                break;
            }
        }
        if (m_curve == kNoId) {
            emit view().message(QObject::tr("That point isn't the end of a line or arc"));
        }
        refresh();
        return;
    }
    const Snap end = view().snap(e.screen);
    const Id curve = m_curve, start = m_start;
    m_curve = m_start = kNoId;
    view().tryEdit(QObject::tr("Arc"), [&](Editor& ed) { ed.arcTangent(curve, start, resolve(ed, end)); });
    refresh();
}

void ArcTangentTool::move(const ToolEvent& e)
{
    m_cursor = view().snap(e.screen);
    refresh();
}

bool ArcTangentTool::cancel()
{
    if (m_curve == kNoId) {
        return false;
    }
    m_curve = m_start = kNoId;
    refresh();
    return true;
}

void ArcTangentTool::paintOverlay(QPainter& painter)
{
    if (m_curve != kNoId && doc().sketch().hasEntity(m_curve)) {
        const Snap end = m_cursor;
        drawPreviewEdit(view(), painter, [&](Editor& ed) { ed.arcTangent(m_curve, m_start, resolve(ed, end)); });
    }
    view().drawSnapMarker(painter, m_cursor);
}
