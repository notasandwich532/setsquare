#include "CurveTools.h"

#include "../Document.h"
#include "../SketchView.h"

#include <QPainter>

#include <cmath>

using core::Id;
using core::kNoId;
using core::Vec2;

namespace
{
constexpr double kClosePixels = 8.0;  // a click this near the first point closes the spline
}  // namespace

// =============================================================================================
// Ellipse
// =============================================================================================

QString EllipseTool::hintFor(size_t step) const
{
    switch (step) {
        case 0: return QObject::tr("Ellipse: click the centre");
        case 1: return QObject::tr("Click the end of one axis (its direction and half-length)");
        default: return QObject::tr("Click to set the other axis");
    }
}

void EllipseTool::build(Editor& ed, const std::vector<Snap>& c)
{
    const Id centre = resolveSnap(ed, c[0]);
    const Id major = resolveSnap(ed, c[1]);
    const core::Point& pc = ed.sketch().point(centre);
    const core::Point& pa = ed.sketch().point(major);
    const Vec2 u{pa.x - pc.x, pa.y - pc.y};
    const double len = std::hypot(u.x, u.y);
    if (len <= 0.0) {
        throw core::SketchError("ellipse: the axis has no length");
    }
    // The other axis: at right angles, as far out as the third click is from the first axis.
    const Vec2 n{-u.y / len, u.x / len};
    const double b = (c[2].pos.x - pc.x) * n.x + (c[2].pos.y - pc.y) * n.y;
    if (std::abs(b) <= 1e-9 * len) {
        throw core::SketchError("ellipse: the other axis has no length");
    }
    const Id minor = ed.point(pc.x + n.x * b, pc.y + n.y * b);
    ed.ellipse(centre, major, minor);
}

// =============================================================================================
// Spline
// =============================================================================================

QString SplineTool::hint() const
{
    const QString what = m_kind == core::SplineKind::Fit ? QObject::tr("Spline (through points)")
                                                         : QObject::tr("Spline (control points)");
    if (m_clicks.empty()) {
        return what + QObject::tr(": click its first point");
    }
    if (closesAt(m_cursor)) {
        return what + QObject::tr(": click to close it");
    }
    return what + QObject::tr(": click the next point; Enter or double-click to finish, the first point to close it, "
                              "Backspace to take one back");
}

bool SplineTool::closesAt(const Snap& snap) const
{
    if (m_clicks.size() < 3) {
        return false;
    }
    const Snap& first = m_clicks.front();
    if (first.point != kNoId && snap.point == first.point) {
        return true;
    }
    const QPointF a = view().toScreen(first.pos), b = view().toScreen(snap.pos);
    return std::hypot(a.x() - b.x(), a.y() - b.y()) <= kClosePixels;
}

void SplineTool::press(const ToolEvent& e)
{
    if (e.button != Qt::LeftButton) {
        if (m_clicks.size() >= 2) {
            finish(false);  // right-click: done
        }
        else {
            cancel();
        }
        return;
    }
    m_cursor = view().snap(e.screen);
    if (closesAt(m_cursor)) {
        finish(true);
        return;
    }
    // (the same spot twice in a row adds nothing: e.g. the first click of a double-click)
    if (!m_clicks.empty()) {
        const QPointF a = view().toScreen(m_clicks.back().pos), b = e.screen;
        if (std::hypot(a.x() - b.x(), a.y() - b.y()) < 2.0) {
            return;
        }
    }
    m_clicks.push_back(m_cursor);
    refresh();
}

void SplineTool::move(const ToolEvent& e)
{
    m_cursor = view().snap(e.screen);
    refresh();
}

void SplineTool::doubleClick(const ToolEvent&)
{
    if (m_clicks.size() >= 2) {
        finish(false);
    }
}

bool SplineTool::cancel()
{
    if (m_clicks.empty()) {
        return false;
    }
    m_clicks.clear();
    refresh();
    return true;
}

bool SplineTool::keyPress(int key, const QString&)
{
    if (key == Qt::Key_Return || key == Qt::Key_Enter) {
        if (m_clicks.size() >= 2) {
            finish(false);
        }
        return true;
    }
    if (key == Qt::Key_Backspace && !m_clicks.empty()) {
        m_clicks.pop_back();
        refresh();
        return true;
    }
    return false;
}

void SplineTool::build(Editor& ed, const std::vector<Snap>& clicks, bool closed) const
{
    std::vector<Id> points;
    for (const Snap& c : clicks) {
        const Id p = resolveSnap(ed, c);
        if (std::ranges::find(points, p) == points.end()) {  // (an existing point used twice)
            points.push_back(p);
        }
    }
    ed.spline(points, m_kind, closed && points.size() >= 3);
}

void SplineTool::finish(bool closed)
{
    const std::vector<Snap> clicks = std::move(m_clicks);
    m_clicks.clear();
    if (clicks.size() >= 2) {
        view().tryEdit(m_kind == core::SplineKind::Fit ? QObject::tr("Spline") : QObject::tr("Control point spline"),
                       [&](Editor& ed) { build(ed, clicks, closed); });
    }
    refresh();
}

void SplineTool::paintOverlay(QPainter& painter)
{
    if (!m_clicks.empty()) {
        const bool closing = closesAt(m_cursor);
        std::vector<Snap> clicks = m_clicks;
        if (!closing) {
            clicks.push_back(m_cursor);
        }
        if (clicks.size() >= 2) {
            drawPreviewEdit(view(), painter, [&](Editor& ed) { build(ed, clicks, closing); });
        }
    }
    view().drawSnapMarker(painter, m_cursor);
}
