#include "StyleTools.h"

#include "../Document.h"
#include "../SketchView.h"

#include <core/Features.h>
#include <core/Geometry.h>

#include <QPainter>
#include <QPolygonF>

using core::Id;
using core::kNoId;

namespace
{
constexpr double kPickPixels = 7.0;
}

QString StrokeBrushTool::hint() const
{
    return QObject::tr("Stroke brush: click a curve to give it the current colour and line style, double-click for "
                       "its whole chain. Ctrl+click removes, Alt+click picks a style up");
}

Id StrokeBrushTool::curveAt(QPointF screen) const
{
    core::PickOptions options;
    options.tolerance = kPickPixels / view().zoom();
    options.points = false;
    const core::Sketch& s = doc().sketch();
    const Id hit = core::pick(s, view().toWorld(screen), options);
    if (hit == kNoId || s.isReference(hit) || s.entity(hit).construction) {
        return kNoId;
    }
    return hit;
}

void StrokeBrushTool::move(const ToolEvent& e)
{
    m_hover = curveAt(e.screen);
    view().setHovered(m_hover);
    refresh();
}

void StrokeBrushTool::press(const ToolEvent& e)
{
    if (e.button != Qt::LeftButton) {
        return;
    }
    const Id curve = curveAt(e.screen);
    if (curve == kNoId) {
        return;
    }
    const core::Sketch& s = doc().sketch();
    if (e.modifiers & Qt::AltModifier) {
        // Eyedropper: this curve's stroke becomes the paint.
        if (const core::StrokeStyle* style = s.stroke(curve)) {
            SketchView::Paint paint = view().paint();
            paint.color1 = style->color;
            paint.width = style->width;
            paint.dash = style->dash;
            view().setPaint(paint);
        }
        return;
    }
    if (e.modifiers & Qt::ControlModifier) {
        if (s.stroke(curve)) {
            view().tryEdit(QObject::tr("Remove stroke"), [&](Editor& ed) { ed.clearStroke({curve}); });
        }
        return;
    }
    const core::StrokeStyle style = view().paint().stroke();
    if (const core::StrokeStyle* current = s.stroke(curve); current && *current == style) {
        return;  // already so: no undo step for nothing
    }
    view().tryEdit(QObject::tr("Stroke"), [&](Editor& ed) { ed.setStroke({curve}, style); });
}

void StrokeBrushTool::doubleClick(const ToolEvent& e)
{
    if (e.modifiers & (Qt::AltModifier | Qt::ControlModifier)) {
        return;
    }
    const Id curve = curveAt(e.screen);
    if (curve == kNoId) {
        return;
    }
    std::vector<Id> chain;
    try {
        chain = core::feature::chainAt(doc().sketch(), curve);
    }
    catch (const core::SketchError&) {
        return;
    }
    const core::StrokeStyle style = view().paint().stroke();
    view().tryEdit(QObject::tr("Stroke"), [&](Editor& ed) { ed.setStroke(chain, style); });
}

void StrokeBrushTool::paintOverlay(QPainter& painter)
{
    const core::Sketch& s = doc().sketch();
    if (m_hover == kNoId || !s.hasEntity(m_hover)) {
        return;
    }
    // What the click will make.
    QPolygonF line;
    for (const core::Vec2& p : core::sample(s, m_hover, 96)) {
        line << view().toScreen(p);
    }
    painter.setPen(view().strokePen(view().paint().stroke()));
    painter.drawPolyline(line);
}
