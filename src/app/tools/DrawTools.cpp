#include "DrawTools.h"

#include "../Document.h"
#include "../SketchView.h"

#include <QPainter>

#include <algorithm>
#include <cmath>
#include <numbers>

using core::Id;
using core::kNoId;
using core::Sketch;
using core::Vec2;

namespace
{
constexpr double kPi = std::numbers::pi;
constexpr double kMinSizePx = 2.0;  // shapes smaller than this on screen are ignored

double distance(Vec2 a, Vec2 b)
{
    return std::hypot(b.x - a.x, b.y - a.y);
}

constexpr double kAxisSnapDegrees = 3.0;  // lines this close to horizontal/vertical snap to it

Id resolvePoint(Editor& ed, const Snap& snap)
{
    return resolveSnap(ed, snap);
}

QString formatLength(double v)
{
    return QString::number(v, 'f', 2);
}
}  // namespace

Id resolveSnap(Editor& ed, const Snap& snap)
{
    const Sketch& s = ed.sketch();
    if (snap.point != kNoId && s.hasEntity(snap.point) && s.entity(snap.point).kind() == core::EntityKind::Point) {
        return snap.point;
    }
    const Id p = ed.point(snap.pos.x, snap.pos.y);
    if (snap.midpointOf != kNoId && s.hasEntity(snap.midpointOf)) {
        ed.constrain(core::ConstraintType::Midpoint, {p, snap.midpointOf});
    }
    else if (snap.onCurve != kNoId && s.hasEntity(snap.onCurve)) {
        ed.constrain(core::ConstraintType::Coincident, {p, snap.onCurve});
    }
    return p;
}

// =============================================================================================
// Line
// =============================================================================================

QString LineTool::hint() const
{
    return m_active ? QObject::tr("Click the next point. Esc, right-click or double-click ends the chain")
                    : QObject::tr("Line: click the start point");
}

void LineTool::press(const ToolEvent& e)
{
    if (e.button == Qt::RightButton) {
        m_active = false;
        refresh();
        return;
    }
    if (e.button != Qt::LeftButton) {
        return;
    }
    updateCursor(e.screen);
    const Snap snap = m_cursor;
    if (!m_active) {
        m_active = true;
        m_start = snap;
        m_chainFirst = kNoId;
        refresh();
        return;
    }
    if (distance(m_start.pos, snap.pos) * view().zoom() < kMinSizePx) {
        return;
    }

    Id a = kNoId, b = kNoId;
    const Axis axis = m_axis;
    const bool ok = view().tryEdit(QObject::tr("Line"), [&](Editor& ed) {
        a = resolvePoint(ed, m_start);
        b = resolvePoint(ed, snap);
        const Id line = ed.line(a, b);
        if (axis == Axis::Horizontal) {
            ed.constrain(core::ConstraintType::Horizontal, {line});
        }
        else if (axis == Axis::Vertical) {
            ed.constrain(core::ConstraintType::Vertical, {line});
        }
    });
    if (!ok) {
        m_active = false;
        refresh();
        return;
    }

    if (m_chainFirst == kNoId) {
        m_chainFirst = a;
    }
    if (b == m_chainFirst) {
        m_active = false;  // closed the loop
    }
    else {
        m_start = Snap{snap.pos, b};
    }
    m_axis = Axis::None;
    refresh();
}

void LineTool::updateCursor(QPointF screen)
{
    m_cursor = view().snap(screen);
    m_axis = Axis::None;
    // Nearly horizontal/vertical segments get the constraint. Free ends also snap onto the
    // axis; an existing point stays where it is (the solver aligns it). Midpoint / on-curve
    // snaps already carry a constraint, so they're left alone.
    const bool free = m_cursor.isFree();
    const bool onStart = m_cursor.point != kNoId && m_cursor.point == m_start.point;
    if (!m_active || !(free || m_cursor.point != kNoId) || onStart) {
        return;
    }
    const double dx = m_cursor.pos.x - m_start.pos.x;
    const double dy = m_cursor.pos.y - m_start.pos.y;
    if (std::hypot(dx, dy) * view().zoom() < kMinSizePx) {
        return;
    }
    const double angle = std::abs(std::atan2(dy, dx)) * 180.0 / kPi;  // 0..180
    if (angle < kAxisSnapDegrees || angle > 180.0 - kAxisSnapDegrees) {
        if (free) {
            m_cursor.pos.y = m_start.pos.y;
        }
        m_axis = Axis::Horizontal;
    }
    else if (std::abs(angle - 90.0) < kAxisSnapDegrees) {
        if (free) {
            m_cursor.pos.x = m_start.pos.x;
        }
        m_axis = Axis::Vertical;
    }
}

void LineTool::move(const ToolEvent& e)
{
    updateCursor(e.screen);
    refresh();
}

void LineTool::doubleClick(const ToolEvent&)
{
    m_active = false;
    refresh();
}

bool LineTool::cancel()
{
    if (!m_active) {
        return false;
    }
    m_active = false;
    refresh();
    return true;
}

void LineTool::paintOverlay(QPainter& painter)
{
    if (m_active) {
        view().drawPreviewLine(painter, m_start.pos, m_cursor.pos);
        QString label = formatLength(distance(m_start.pos, m_cursor.pos));
        if (m_axis == Axis::Horizontal) {
            label += QStringLiteral("  H");
        }
        else if (m_axis == Axis::Vertical) {
            label += QStringLiteral("  V");
        }
        view().drawLabel(painter, m_cursor.pos, label);
    }
    view().drawSnapMarker(painter, m_cursor);
}

// =============================================================================================
// Rectangle
// =============================================================================================

QString RectangleTool::hint() const
{
    return m_active ? QObject::tr("Click the opposite corner") : QObject::tr("Rectangle: click the first corner");
}

void RectangleTool::press(const ToolEvent& e)
{
    if (e.button != Qt::LeftButton) {
        cancel();
        return;
    }
    const Snap snap = view().snap(e.screen);
    if (!m_active) {
        m_active = true;
        m_first = snap;
        refresh();
        return;
    }
    const double w = std::abs(snap.pos.x - m_first.pos.x);
    const double h = std::abs(snap.pos.y - m_first.pos.y);
    if (std::min(w, h) * view().zoom() < kMinSizePx) {
        return;
    }

    view().tryEdit(QObject::tr("Rectangle"), [&](Editor& ed) {
        const Id corner = resolvePoint(ed, m_first);
        const Id opposite = resolvePoint(ed, snap);
        ed.rectangle(corner, opposite);
    });
    m_active = false;
    refresh();
}

void RectangleTool::move(const ToolEvent& e)
{
    m_cursor = view().snap(e.screen);
    refresh();
}

bool RectangleTool::cancel()
{
    if (!m_active) {
        return false;
    }
    m_active = false;
    refresh();
    return true;
}

void RectangleTool::paintOverlay(QPainter& painter)
{
    if (m_active) {
        const Vec2 a = m_first.pos, c = m_cursor.pos;
        const Vec2 b{c.x, a.y}, d{a.x, c.y};
        view().drawPreviewLine(painter, a, b);
        view().drawPreviewLine(painter, b, c);
        view().drawPreviewLine(painter, c, d);
        view().drawPreviewLine(painter, d, a);
        view().drawLabel(painter, c,
                         formatLength(std::abs(c.x - a.x)) + QStringLiteral(" x ") + formatLength(std::abs(c.y - a.y)));
    }
    view().drawSnapMarker(painter, m_cursor);
}

// =============================================================================================
// Circle
// =============================================================================================

QString CircleTool::hint() const
{
    return m_active ? QObject::tr("Click to set the radius") : QObject::tr("Circle: click the centre");
}

void CircleTool::press(const ToolEvent& e)
{
    if (e.button != Qt::LeftButton) {
        cancel();
        return;
    }
    const Snap snap = view().snap(e.screen);
    if (!m_active) {
        m_active = true;
        m_center = snap;
        refresh();
        return;
    }
    const double r = distance(m_center.pos, snap.pos);
    if (r * view().zoom() < kMinSizePx) {
        return;
    }
    view().tryEdit(QObject::tr("Circle"), [&](Editor& ed) {
        const Id circle = ed.circle(resolvePoint(ed, m_center), r);
        if (snap.point != kNoId && ed.sketch().hasEntity(snap.point)) {
            ed.constrain(core::ConstraintType::Coincident, {snap.point, circle});  // rim through it
        }
    });
    m_active = false;
    refresh();
}

void CircleTool::move(const ToolEvent& e)
{
    m_cursor = view().snap(e.screen);
    refresh();
}

bool CircleTool::cancel()
{
    if (!m_active) {
        return false;
    }
    m_active = false;
    refresh();
    return true;
}

void CircleTool::paintOverlay(QPainter& painter)
{
    if (m_active) {
        const double r = distance(m_center.pos, m_cursor.pos);
        view().drawPreviewCircle(painter, m_center.pos, r);
        view().drawLabel(painter, m_cursor.pos, QStringLiteral("R ") + formatLength(r));
    }
    view().drawSnapMarker(painter, m_cursor);
}

// =============================================================================================
// Centre-point arc
// =============================================================================================

QString CenterArcTool::hint() const
{
    switch (m_step) {
        case 0: return QObject::tr("Arc: click the centre");
        case 1: return QObject::tr("Click the start point (sets the radius)");
        default: return QObject::tr("Click the end point; sweep follows the mouse");
    }
}

bool CenterArcTool::preview(core::ArcShape& shape) const
{
    shape.center = m_center.pos;
    shape.radius = distance(m_center.pos, m_start.pos);
    const double a0 = std::atan2(m_start.pos.y - m_center.pos.y, m_start.pos.x - m_center.pos.x);
    if (m_sweep >= 0.0) {
        shape.startAngle = a0;
        shape.sweep = m_sweep;
    }
    else {
        shape.startAngle = a0 + m_sweep;
        shape.sweep = -m_sweep;
    }
    return shape.radius > 0.0 && shape.sweep > 1e-6;
}

void CenterArcTool::press(const ToolEvent& e)
{
    if (e.button != Qt::LeftButton) {
        cancel();
        return;
    }
    const Snap snap = view().snap(e.screen);
    switch (m_step) {
        case 0:
            m_center = snap;
            m_step = 1;
            break;
        case 1:
            if (distance(m_center.pos, snap.pos) * view().zoom() < kMinSizePx) {
                return;
            }
            m_start = snap;
            m_sweep = 0.0;
            m_lastAngle = std::atan2(snap.pos.y - m_center.pos.y, snap.pos.x - m_center.pos.x);
            m_step = 2;
            break;
        default: {
            core::ArcShape shape;
            if (!preview(shape)) {
                return;
            }
            // The end lands on the circle (or on the snapped point, which the solver pulls on).
            const double endAngle = std::atan2(snap.pos.y - m_center.pos.y, snap.pos.x - m_center.pos.x);
            const Snap end{{m_center.pos.x + shape.radius * std::cos(endAngle),
                            m_center.pos.y + shape.radius * std::sin(endAngle)},
                           snap.point};
            const bool ccw = m_sweep >= 0.0;
            view().tryEdit(QObject::tr("Arc"), [&](Editor& ed) {
                const Id c = resolvePoint(ed, m_center);
                const Id a = resolvePoint(ed, m_start);
                const Id b = resolvePoint(ed, end);
                ccw ? ed.arc(c, a, b) : ed.arc(c, b, a);
            });
            m_step = 0;
            break;
        }
    }
    refresh();
}

void CenterArcTool::move(const ToolEvent& e)
{
    m_cursor = view().snap(e.screen);
    if (m_step == 2) {
        const double angle = std::atan2(m_cursor.pos.y - m_center.pos.y, m_cursor.pos.x - m_center.pos.x);
        double delta = angle - m_lastAngle;
        if (delta > kPi) {
            delta -= 2 * kPi;
        }
        else if (delta < -kPi) {
            delta += 2 * kPi;
        }
        m_sweep = std::clamp(m_sweep + delta, -2 * kPi + 1e-3, 2 * kPi - 1e-3);
        m_lastAngle = angle;
    }
    refresh();
}

bool CenterArcTool::cancel()
{
    if (m_step == 0) {
        return false;
    }
    m_step = 0;
    refresh();
    return true;
}

void CenterArcTool::paintOverlay(QPainter& painter)
{
    if (m_step == 1) {
        view().drawPreviewLine(painter, m_center.pos, m_cursor.pos);
        view().drawLabel(painter, m_cursor.pos, QStringLiteral("R ") + formatLength(distance(m_center.pos, m_cursor.pos)));
    }
    else if (m_step == 2) {
        core::ArcShape shape;
        if (preview(shape)) {
            view().drawPreviewArc(painter, shape);
            view().drawLabel(painter, m_cursor.pos, QString::number(std::abs(m_sweep) * 180.0 / kPi, 'f', 1) + QChar(0x00B0));
        }
    }
    view().drawSnapMarker(painter, m_cursor);
}

// =============================================================================================
// Three-point arc
// =============================================================================================

QString ThreePointArcTool::hint() const
{
    switch (m_step) {
        case 0: return QObject::tr("3-point arc: click the start point");
        case 1: return QObject::tr("Click the end point");
        default: return QObject::tr("Click a point the arc passes through");
    }
}

bool ThreePointArcTool::preview(core::ArcShape& shape, bool& reversed) const
{
    Vec2 c;
    double r = 0.0;
    const Vec2 s = m_start.pos, e = m_end.pos, m = m_cursor.pos;
    if (!core::circumcircle(s, m, e, c, r)) {
        return false;
    }
    // Model arcs run counter-clockwise from start to end. If walking start -> through -> end is
    // clockwise, the arc has to be stored the other way round (end -> start).
    const double cross = (m.x - s.x) * (e.y - s.y) - (m.y - s.y) * (e.x - s.x);
    reversed = cross < 0.0;
    const Vec2 from = reversed ? e : s;
    const Vec2 to = reversed ? s : e;
    shape.center = c;
    shape.radius = r;
    shape.startAngle = std::atan2(from.y - c.y, from.x - c.x);
    double end = std::atan2(to.y - c.y, to.x - c.x);
    double sweep = end - shape.startAngle;
    while (sweep <= 0.0) {
        sweep += 2 * kPi;
    }
    shape.sweep = sweep;
    return true;
}

void ThreePointArcTool::press(const ToolEvent& e)
{
    if (e.button != Qt::LeftButton) {
        cancel();
        return;
    }
    const Snap snap = view().snap(e.screen);
    switch (m_step) {
        case 0:
            m_start = snap;
            m_step = 1;
            break;
        case 1:
            if (distance(m_start.pos, snap.pos) * view().zoom() < kMinSizePx) {
                return;
            }
            m_end = snap;
            m_step = 2;
            break;
        default: {
            m_cursor = snap;
            core::ArcShape shape;
            bool reversed = false;
            if (!preview(shape, reversed)) {
                return;
            }
            view().tryEdit(QObject::tr("Arc"), [&](Editor& ed) {
                const Id c = ed.point(shape.center.x, shape.center.y);
                const Id a = resolvePoint(ed, m_start);
                const Id b = resolvePoint(ed, m_end);
                reversed ? ed.arc(c, b, a) : ed.arc(c, a, b);
            });
            m_step = 0;
            break;
        }
    }
    refresh();
}

void ThreePointArcTool::move(const ToolEvent& e)
{
    m_cursor = view().snap(e.screen);
    refresh();
}

bool ThreePointArcTool::cancel()
{
    if (m_step == 0) {
        return false;
    }
    m_step = 0;
    refresh();
    return true;
}

void ThreePointArcTool::paintOverlay(QPainter& painter)
{
    if (m_step == 1) {
        view().drawPreviewLine(painter, m_start.pos, m_cursor.pos);
    }
    else if (m_step == 2) {
        core::ArcShape shape;
        bool reversed = false;
        if (preview(shape, reversed)) {
            view().drawPreviewArc(painter, shape);
            view().drawLabel(painter, m_cursor.pos, QStringLiteral("R ") + formatLength(shape.radius));
        }
        else {
            view().drawPreviewLine(painter, m_start.pos, m_end.pos);
        }
    }
    view().drawSnapMarker(painter, m_cursor);
}
