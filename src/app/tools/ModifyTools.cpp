#include "ModifyTools.h"

#include "../Document.h"
#include "../SketchView.h"
#include "../Theme.h"
#include "ShapeTools.h"

#include <core/Geometry.h>

#include <QPainter>

#include <cmath>

using core::Id;
using core::kNoId;
using core::Vec2;
namespace modify = core::modify;

namespace
{
constexpr double kPickPixels = 7.0;
constexpr double kDragPixels = 4.0;  // a press that moves further than this is a drag

void drawPolyline(SketchView& view, QPainter& painter, const std::vector<Vec2>& pts, const QPen& pen)
{
    painter.setPen(pen);
    for (size_t i = 0; i + 1 < pts.size(); ++i) {
        painter.drawLine(view.toScreen(pts[i]), view.toScreen(pts[i + 1]));
    }
}
}  // namespace

// =============================================================================================
// Trim / Extend / Break
// =============================================================================================

QString CurveOpTool::hint() const
{
    switch (m_op) {
        case Op::Trim: return QObject::tr("Trim: click the part of a curve to remove, or drag across several");
        case Op::TrimToConstruction:
            return QObject::tr("Trim to construction: click a part of a curve, or drag across several, to turn them "
                               "into construction");
        case Op::Extend: return QObject::tr("Extend: click near the end of a line or arc to extend");
        case Op::Break: return QObject::tr("Break: click a curve to split it where other curves cross it");
    }
    return {};
}

Id CurveOpTool::curveAt(QPointF screen) const
{
    core::PickOptions options;
    options.sublayer = doc().sketch().activeSublayer();  // (changes only the active sublayer)
    options.tolerance = kPickPixels / view().zoom();
    options.points = false;
    const Id id = core::pick(doc().sketch(), view().toWorld(screen), options);
    return doc().sketch().isReference(id) ? kNoId : id;
}

bool CurveOpTool::trims() const
{
    return m_op == Op::Trim || m_op == Op::TrimToConstruction;
}

void CurveOpTool::move(const ToolEvent& e)
{
    m_mouse = e.world;
    if (m_pressed && trims()) {
        const QPointF moved = e.screen - m_pressScreen;
        if (!m_dragging && std::hypot(moved.x(), moved.y()) >= kDragPixels) {
            m_dragging = true;  // a press that travels becomes a trim path
            view().setHovered(kNoId);
        }
        if (m_dragging) {
            m_path.push_back(e.world);
            refresh();
            return;
        }
    }
    m_hover = curveAt(e.screen);
    view().setHovered(m_hover);
    refresh();
}

void CurveOpTool::press(const ToolEvent& e)
{
    if (e.button != Qt::LeftButton) {
        return;
    }
    if (trims()) {
        // Wait for the release: a click trims one piece, a drag trims everything it crosses.
        m_pressed = true;
        m_dragging = false;
        m_pressScreen = e.screen;
        m_path = {e.world};
        return;
    }
    clickAt(e.screen, e.world);
}

void CurveOpTool::release(const ToolEvent& e)
{
    if (e.button != Qt::LeftButton || !m_pressed) {
        return;
    }
    m_pressed = false;
    if (m_dragging) {
        m_dragging = false;
        m_path.push_back(e.world);
        trimPath();
        m_path.clear();
    }
    else {
        clickAt(e.screen, e.world);
    }
    m_hover = curveAt(e.screen);
    refresh();
}

bool CurveOpTool::cancel()
{
    if (!m_dragging) {
        return false;
    }
    m_pressed = m_dragging = false;
    m_path.clear();
    refresh();
    return true;
}

void CurveOpTool::clickAt(QPointF screen, Vec2 at)
{
    const Id curve = curveAt(screen);
    if (curve == kNoId) {
        return;
    }
    switch (m_op) {
        case Op::Trim: view().tryEdit(QObject::tr("Trim"), [&](Editor& ed) { ed.trim(curve, at); }); break;
        case Op::TrimToConstruction:
            view().tryEdit(QObject::tr("Trim"), [&](Editor& ed) { ed.trim(curve, at, true); });
            break;
        case Op::Extend: view().tryEdit(QObject::tr("Extend"), [&](Editor& ed) { ed.extend(curve, at); }); break;
        case Op::Break: view().tryEdit(QObject::tr("Break"), [&](Editor& ed) { ed.breakCurve(curve, at); }); break;
    }
    m_hover = curveAt(screen);
    refresh();
}

std::vector<modify::Crossing> CurveOpTool::pathCrossings() const
{
    std::vector<modify::Crossing> hits;
    for (size_t i = 0; i + 1 < m_path.size(); ++i) {
        for (const modify::Crossing& c : modify::crossings(doc().sketch(), m_path[i], m_path[i + 1])) {
            hits.push_back(c);
        }
    }
    return hits;
}

void CurveOpTool::trimPath()
{
    const std::vector<modify::Crossing> hits = pathCrossings();
    if (hits.empty()) {
        return;
    }
    const bool construction = m_op == Op::TrimToConstruction;
    const double tolerance = kPickPixels / view().zoom();
    // All in one undo step. Each trim changes the sketch, so a later crossing only applies if
    // its spot is still on that curve (crossing the same piece twice trims it once).
    view().tryEdit(QObject::tr("Trim"), [&](Editor& ed) {
        for (const modify::Crossing& c : hits) {
            const core::Sketch& s = ed.sketch();
            if (!s.hasEntity(c.curve) || core::distanceTo(s, c.curve, c.at) > tolerance) {
                continue;
            }
            if (construction && s.entity(c.curve).construction) {
                continue;  // already done
            }
            ed.trim(c.curve, c.at, construction);
        }
    });
}

void CurveOpTool::paintOverlay(QPainter& painter)
{
    const core::Sketch& s = doc().sketch();
    if (m_dragging) {
        // The path, and every piece it will take.
        const QPen piecePen = m_op == Op::TrimToConstruction
            ? QPen(theme::canvas().construction, 3.0, Qt::DashLine, Qt::RoundCap)
            : QPen(theme::canvas().conflict, 3.0, Qt::SolidLine, Qt::RoundCap);
        for (const modify::Crossing& c : pathCrossings()) {
            drawPolyline(view(), painter, modify::trimPreview(s, c.curve, c.at), piecePen);
        }
        drawPolyline(view(), painter, m_path, QPen(theme::canvas().hover, 1.0, Qt::DotLine));
        return;
    }
    if (m_hover == kNoId || !s.hasEntity(m_hover)) {
        return;
    }
    switch (m_op) {
        case Op::Trim:
            drawPolyline(view(), painter, modify::trimPreview(s, m_hover, m_mouse),
                         QPen(theme::canvas().conflict, 3.0, Qt::SolidLine, Qt::RoundCap));
            break;
        case Op::TrimToConstruction:
            drawPolyline(view(), painter, modify::trimPreview(s, m_hover, m_mouse),
                         QPen(theme::canvas().construction, 3.0, Qt::DashLine, Qt::RoundCap));
            break;
        case Op::Extend:
            drawPolyline(view(), painter, modify::extendPreview(s, m_hover, m_mouse),
                         QPen(theme::canvas().underConstrained, 2.0, Qt::DashLine, Qt::RoundCap));
            break;
        case Op::Break:
            drawPolyline(view(), painter, modify::breakPreview(s, m_hover, m_mouse),
                         QPen(theme::canvas().selection, 3.0, Qt::SolidLine, Qt::RoundCap));
            break;
    }
}

// =============================================================================================
// Fillet / Chamfer
// =============================================================================================

QString CornerTool::hint() const
{
    const QString what = m_kind == Kind::Fillet ? QObject::tr("Fillet") : QObject::tr("Chamfer");
    return m_first == kNoId ? QObject::tr("%1: click a corner point, or the first of two curves").arg(what)
                            : QObject::tr("%1: click the second curve").arg(what);
}

modify::Corner CornerTool::apply(Editor& ed, Id a, Vec2 nearA, Id b, Vec2 nearB) const
{
    const SketchView::ModifyOptions& o = view().modifyOptions();
    switch (m_kind) {
        case Kind::Fillet: return ed.fillet(a, nearA, b, nearB, o.filletRadius);
        case Kind::ChamferEqual:
            return ed.chamfer(a, nearA, b, nearB, modify::ChamferKind::EqualDistance, o.chamferDistance);
        case Kind::ChamferTwoDistances:
            return ed.chamfer(a, nearA, b, nearB, modify::ChamferKind::TwoDistances, o.chamferDistance,
                              o.chamferDistance2);
        case Kind::ChamferDistanceAngle:
            return ed.chamfer(a, nearA, b, nearB, modify::ChamferKind::DistanceAngle, o.chamferDistance,
                              o.chamferAngle);
    }
    return {};
}

bool CornerTool::candidate(QPointF screen, Vec2 world, Id& a, Vec2& nearA, Id& b, Vec2& nearB) const
{
    const core::Sketch& s = doc().sketch();
    if (m_first == kNoId) {
        // A corner point: both curves at once.
        core::PickOptions points;
        points.sublayer = doc().sketch().activeSublayer();  // (changes only the active sublayer)
        points.tolerance = kPickPixels / view().zoom();
        points.curves = false;
        const Id p = core::pick(s, world, points);
        return p != kNoId && modify::cornerAt(s, p, a, nearA, b, nearB);
    }
    core::PickOptions curves;
    curves.sublayer = doc().sketch().activeSublayer();  // (changes only the active sublayer)
    curves.tolerance = kPickPixels / view().zoom();
    curves.points = false;
    const Id hit = core::pick(s, view().toWorld(screen), curves);
    if (hit == kNoId || hit == m_first || s.isReference(hit) || !s.hasEntity(m_first)) {
        return false;
    }
    a = m_first;
    nearA = m_firstAt;
    b = hit;
    nearB = world;
    return true;
}

void CornerTool::finish(Id a, Vec2 nearA, Id b, Vec2 nearB)
{
    m_first = kNoId;
    view().setSelection({});
    Id dimension = kNoId;
    const bool ok = view().tryEdit(m_kind == Kind::Fillet ? QObject::tr("Fillet") : QObject::tr("Chamfer"),
                                   [&](Editor& ed) {
                                       const modify::Corner c = apply(ed, a, nearA, b, nearB);
                                       if (!c.dimensions.empty()) {
                                           dimension = c.dimensions.front();
                                       }
                                   });
    refresh();
    if (ok && dimension != kNoId) {
        view().editDimension(dimension);  // type the size straight away, as in Fusion
    }
}

void CornerTool::press(const ToolEvent& e)
{
    if (e.button != Qt::LeftButton) {
        cancel();
        return;
    }
    Id a, b;
    Vec2 nearA, nearB;
    if (candidate(e.screen, e.world, a, nearA, b, nearB)) {
        finish(a, nearA, b, nearB);
        return;
    }
    if (m_first == kNoId) {
        core::PickOptions curves;
        curves.sublayer = doc().sketch().activeSublayer();  // (changes only the active sublayer)
        curves.tolerance = kPickPixels / view().zoom();
        curves.points = false;
        const Id hit = core::pick(doc().sketch(), e.world, curves);
        if (hit != kNoId && !doc().sketch().isReference(hit)) {
            m_first = hit;
            m_firstAt = e.world;
            view().setSelection({hit});
        }
    }
    refresh();
}

void CornerTool::move(const ToolEvent& e)
{
    m_mouseScreen = e.screen;
    m_mouse = e.world;
    refresh();
}

bool CornerTool::cancel()
{
    if (m_first == kNoId) {
        return false;
    }
    m_first = kNoId;
    view().setSelection({});
    refresh();
    return true;
}

void CornerTool::paintOverlay(QPainter& painter)
{
    Id a, b;
    Vec2 nearA, nearB;
    if (candidate(m_mouseScreen, m_mouse, a, nearA, b, nearB)) {
        drawPreviewEdit(view(), painter, [&](Editor& ed) { apply(ed, a, nearA, b, nearB); });
    }
}
