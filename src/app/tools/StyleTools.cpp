#include "StyleTools.h"

#include "../Document.h"
#include "../SketchView.h"

#include <core/Features.h>
#include <core/Geometry.h>

#include <QPainter>
#include <QPolygonF>

#include <cmath>

using core::Id;
using core::kNoId;

namespace
{
constexpr double kPickPixels = 7.0;
constexpr double kDragPixels = 4.0;  // a press that moves further than this draws a box
}  // namespace

QString StrokeBrushTool::hint() const
{
    if (m_boxing) {
        return m_boxEnd.x() >= m_pressScreen.x() ? QObject::tr("Stroke brush: curves fully inside the box")
                                                 : QObject::tr("Stroke brush: every curve the box touches");
    }
    return QObject::tr("Stroke brush: click a curve (double-click: its chain, drag: a box of them) to give it the "
                       "current colour and line style. Ctrl removes strokes; Alt+click picks one up");
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

void StrokeBrushTool::apply(const std::vector<Id>& curves, bool clear)
{
    const core::Sketch& s = doc().sketch();
    const core::StrokeStyle style = view().paint().stroke();
    std::vector<Id> todo;
    for (Id c : curves) {
        const core::StrokeStyle* current = s.stroke(c);
        if (clear ? current != nullptr : !(current && *current == style)) {
            todo.push_back(c);  // (no undo step for what's already so)
        }
    }
    if (todo.empty()) {
        return;
    }
    if (clear) {
        view().tryEdit(QObject::tr("Remove stroke"), [&](Editor& ed) { ed.clearStroke(todo); });
    }
    else {
        view().tryEdit(QObject::tr("Stroke"), [&](Editor& ed) { ed.setStroke(todo, style); });
    }
}

void StrokeBrushTool::press(const ToolEvent& e)
{
    if (e.button != Qt::LeftButton) {
        return;
    }
    // Wait for the release: a click paints one curve, a drag a box of them.
    m_pressed = true;
    m_boxing = false;
    m_pressScreen = m_boxEnd = e.screen;
    m_pressModifiers = e.modifiers;
}

void StrokeBrushTool::move(const ToolEvent& e)
{
    if (m_pressed) {
        const QPointF d = e.screen - m_pressScreen;
        if (!m_boxing && std::hypot(d.x(), d.y()) >= kDragPixels) {
            m_boxing = true;
            view().setHovered(kNoId);
        }
        if (m_boxing) {
            m_boxEnd = e.screen;
            refresh();
            return;
        }
    }
    m_hover = curveAt(e.screen);
    view().setHovered(m_hover);
    refresh();
}

void StrokeBrushTool::release(const ToolEvent& e)
{
    if (e.button != Qt::LeftButton || !m_pressed) {
        return;
    }
    m_pressed = false;
    if (m_boxing) {
        m_boxEnd = e.screen;
        m_boxing = false;
        applyBox(m_pressModifiers & Qt::ControlModifier);
    }
    else {
        clickAt(e.screen, m_pressModifiers);
    }
    refresh();
}

bool StrokeBrushTool::cancel()
{
    if (!m_boxing) {
        return false;
    }
    m_pressed = m_boxing = false;
    refresh();
    return true;
}

void StrokeBrushTool::applyBox(bool clear)
{
    const core::Sketch& s = doc().sketch();
    const bool crossing = m_boxEnd.x() < m_pressScreen.x();
    std::vector<Id> curves;
    for (Id id : core::boxSelect(s, view().toWorld(m_pressScreen), view().toWorld(m_boxEnd), crossing)) {
        const core::Entity& e = s.entity(id);
        if (e.kind() != core::EntityKind::Point && !e.construction && !s.isReference(id)) {
            curves.push_back(id);
        }
    }
    apply(curves, clear);
}

void StrokeBrushTool::clickAt(QPointF screen, Qt::KeyboardModifiers modifiers)
{
    const Id curve = curveAt(screen);
    if (curve == kNoId) {
        return;
    }
    if (modifiers & Qt::AltModifier) {
        // Eyedropper: this curve's stroke becomes the paint.
        if (const core::StrokeStyle* style = doc().sketch().stroke(curve)) {
            SketchView::Paint paint = view().paint();
            paint.color1 = style->color;
            paint.width = style->width;
            paint.dash = style->dash;
            view().setPaint(paint);
        }
        return;
    }
    apply({curve}, modifiers & Qt::ControlModifier);
}

void StrokeBrushTool::doubleClick(const ToolEvent& e)
{
    if (e.modifiers & Qt::AltModifier) {
        return;
    }
    const Id curve = curveAt(e.screen);
    if (curve == kNoId) {
        return;
    }
    try {
        apply(core::feature::chainAt(doc().sketch(), curve), e.modifiers & Qt::ControlModifier);
    }
    catch (const core::SketchError&) {
    }
}

// =============================================================================================
// Fill
// =============================================================================================

QString FillTool::hint() const
{
    if (!m_looseEnds.empty()) {
        return QObject::tr("Fill: that area isn't closed; the red rings are loose ends where the outline is open");
    }
    return QObject::tr("Fill: click inside a closed area to fill it with Colour 2 (right-click a palette colour to "
                       "choose it). Ctrl+click removes a fill, Alt+click picks its colour up");
}

const core::regions::Arrangement& FillTool::arrangement()
{
    // Rebuilt after edits only (cheap to keep while hovering).
    if (!m_arrangement || m_builtFor != doc().revision()) {
        m_arrangement = std::make_unique<core::regions::Arrangement>(doc().sketch());
        m_builtFor = doc().revision();
    }
    return *m_arrangement;
}

Id FillTool::fillAt(core::Vec2 p)
{
    Id found = kNoId;
    for (const auto& [id, f] : doc().sketch().fills()) {  // later ones are on top
        const auto region = core::regions::resolve(arrangement(), f);
        if (region && region->contains(p)) {
            found = id;
        }
    }
    return found;
}

void FillTool::move(const ToolEvent& e)
{
    m_hover = arrangement().regionAt(e.world);
    if (m_hover) {
        m_looseEnds.clear();
    }
    refresh();
}

void FillTool::press(const ToolEvent& e)
{
    if (e.button != Qt::LeftButton) {
        return;
    }
    if (e.modifiers & (Qt::ControlModifier | Qt::AltModifier)) {
        const Id fill = fillAt(e.world);
        if (fill == kNoId) {
            return;
        }
        if (e.modifiers & Qt::AltModifier) {
            SketchView::Paint paint = view().paint();
            paint.color2 = doc().sketch().fill(fill).color;  // eyedropper
            view().setPaint(paint);
        }
        else {
            view().tryEdit(QObject::tr("Remove fill"), [&](Editor& ed) { ed.remove(fill); });
        }
        refresh();
        return;
    }
    m_looseEnds.clear();
    if (!arrangement().regionAt(e.world)) {
        m_looseEnds = arrangement().looseEnds();
    }
    const core::Color color = view().paint().color2;
    view().tryEdit(QObject::tr("Fill"), [&](Editor& ed) { ed.fill(e.world, color); });
    refresh();
}

void FillTool::paintOverlay(QPainter& painter)
{
    if (m_hover) {
        // The area a click would fill, in Colour 2, outlined.
        const core::Color& c = view().paint().color2;
        QColor fill(c.r, c.g, c.b, c.a);
        fill.setAlphaF(fill.alphaF() * 0.6);
        const QPainterPath path = view().regionPath(*m_hover);
        painter.fillPath(path, fill);
        painter.setPen(QPen(QColor(0xff, 0xff, 0xff, 200), 1.5, Qt::DashLine));
        painter.setBrush(Qt::NoBrush);
        painter.drawPath(path);
    }
    if (!m_looseEnds.empty()) {
        painter.setPen(QPen(QColor(0xff, 0x45, 0x45), 2.0));
        painter.setBrush(Qt::NoBrush);
        for (const core::Vec2& p : m_looseEnds) {
            painter.drawEllipse(view().toScreen(p), 9.0, 9.0);
        }
    }
}

void StrokeBrushTool::paintOverlay(QPainter& painter)
{
    if (m_boxing) {
        const QRectF rect = QRectF(m_pressScreen, m_boxEnd).normalized();
        const bool crossing = m_boxEnd.x() < m_pressScreen.x();
        const QColor accent = crossing ? QColor(0x5a, 0xc8, 0x7a) : QColor(0x4d, 0xa3, 0xff);
        QColor fill = accent;
        fill.setAlpha(40);
        painter.setPen(QPen(accent, 1.0, crossing ? Qt::DashLine : Qt::SolidLine));
        painter.setBrush(fill);
        painter.drawRect(rect);
        painter.setBrush(Qt::NoBrush);
        return;
    }
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
