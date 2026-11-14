#include "StyleTools.h"

#include "../Document.h"
#include "../Editor.h"
#include "../SketchView.h"

#include <core/Features.h>
#include <core/Geometry.h>

#include <core/Gradient.h>

#include <QGuiApplication>
#include <QPainter>
#include <QPainterPath>
#include <QPolygonF>

#include <algorithm>
#include <cmath>
#include <ranges>

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
    const Id dim = view().annotationAt(e.screen);
    view().setHovered(dim != kNoId ? dim : m_hover);
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
    // A drawn dimension: Colour 1 and the width (Alt: theirs picked up instead).
    if (const Id dim = view().annotationAt(screen); dim != kNoId) {
        core::DrawnDimension d = doc().sketch().drawnDimension(dim);
        if (modifiers & Qt::AltModifier) {
            SketchView::Paint paint = view().paint();
            paint.color1 = d.style.color;
            paint.width = d.style.width;
            view().setPaint(paint);
            return;
        }
        if (modifiers & Qt::ControlModifier) {
            return;  // (they always have a colour: nothing to take off)
        }
        d.style.color = view().paint().color1;
        d.style.width = std::max(0.0, view().paint().width);
        view().tryEdit(QObject::tr("Stroke"), [&](Editor& ed) { ed.setAnnotation(d); });
        return;
    }
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
    const Id layer = doc().sketch().activeLayer();  // (fills are made of the active layer's geometry)
    if (!m_arrangement || m_builtFor != doc().revision() || m_builtLayer != layer) {
        m_arrangement = std::make_unique<core::regions::Arrangement>(doc().sketch(), layer);
        m_builtFor = doc().revision();
        m_builtLayer = layer;
    }
    return *m_arrangement;
}

Id topFillAt(const core::Sketch& sketch, const core::regions::Arrangement& arrangement, core::Vec2 p)
{
    Id found = kNoId;
    for (const auto& [id, f] : sketch.fills()) {  // later ones are on top
        if (f.layer != sketch.activeLayer()) {
            continue;
        }
        const auto region = core::regions::resolve(arrangement, f);
        if (region && region->contains(p)) {
            found = id;
        }
    }
    return found;
}

Id FillTool::fillAt(core::Vec2 p)
{
    return topFillAt(doc().sketch(), arrangement(), p);
}

// =============================================================================================
// Reset fill
// =============================================================================================

QString ResetFillTool::hint() const
{
    return QObject::tr("Reset fill: click a filled area to take its fill off (its colour or gradient)");
}

Id ResetFillTool::fillAt(core::Vec2 p)
{
    const Id layer = doc().sketch().activeLayer();
    if (!m_arrangement || m_builtFor != doc().revision() || m_builtLayer != layer) {
        m_arrangement = std::make_unique<core::regions::Arrangement>(doc().sketch(), layer);
        m_builtFor = doc().revision();
        m_builtLayer = layer;
    }
    return topFillAt(doc().sketch(), *m_arrangement, p);
}

void ResetFillTool::move(const ToolEvent& e)
{
    const Id hover = fillAt(e.world);
    if (hover != m_hover) {
        m_hover = hover;
        refresh();
    }
}

void ResetFillTool::press(const ToolEvent& e)
{
    if (e.button != Qt::LeftButton) {
        return;
    }
    const Id fill = fillAt(e.world);
    if (fill == kNoId) {
        emit view().message(QObject::tr("There's no fill there"));
        return;
    }
    view().tryEdit(QObject::tr("Reset fill"), [&](Editor& ed) { ed.remove(fill); });
    m_hover = fillAt(e.world);  // (one underneath, if any)
    refresh();
}

void ResetFillTool::paintOverlay(QPainter& painter)
{
    if (m_hover == kNoId || !doc().sketch().hasFill(m_hover) || !m_arrangement) {
        return;
    }
    const auto region = core::regions::resolve(*m_arrangement, doc().sketch().fill(m_hover));
    if (!region) {
        return;
    }
    // The fill a click takes off: crossed out in red.
    const QPainterPath path = view().regionPath(*region);
    painter.fillPath(path, QBrush(QColor(0xff, 0x45, 0x45, 90), Qt::BDiagPattern));
    painter.setPen(QPen(QColor(0xff, 0x45, 0x45), 1.5, Qt::DashLine));
    painter.setBrush(Qt::NoBrush);
    painter.drawPath(path);
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

// =============================================================================================
// Eyedropper
// =============================================================================================

QString EyedropperTool::hint() const
{
    return QObject::tr("Eyedropper: click anything coloured (a stroke, fill, text or reference image) to take its "
                       "colour as Colour %1; right-click for Colour 2")
        .arg(view().selectedColor() == 2 ? 2 : 1);
}

std::optional<EyedropperTool::Sample> EyedropperTool::sample(QPointF screen, core::Vec2 world)
{
    const core::Sketch& s = doc().sketch();
    // What's drawn on top first: strokes, text, fills, then images (under everything).
    core::PickOptions options;
    options.tolerance = kPickPixels / view().zoom();
    options.points = false;
    const Id hit = core::pick(s, world, options);
    const core::StrokeStyle* style = hit != kNoId ? s.stroke(hit) : nullptr;
    if (style && !style->none() && !s.entity(hit).construction && s.isShown(hit)) {
        if (style->gradient.active()) {
            if (const auto placed = core::gradient::place(style->gradient, s)) {
                return Sample{core::gradient::colorAt(*placed, world), QObject::tr("stroke")};
            }
            return Sample{style->gradient.average(), QObject::tr("stroke")};
        }
        return Sample{style->color, QObject::tr("stroke")};
    }
    if (const Id text = view().textAt(screen); text != kNoId && s.text(text).hasColor) {
        const core::Text& t = s.text(text);
        if (t.gradient.active()) {
            if (const auto placed = core::gradient::place(t.gradient, s)) {
                return Sample{core::gradient::colorAt(*placed, world), QObject::tr("text")};
            }
        }
        return Sample{t.color, QObject::tr("text")};
    }
    // Fills: the top layer's first, the topmost in it.
    const core::regions::Arrangements arrangements(s);
    for (const core::Layer& layer : s.layers()) {
        if (!layer.visible) {
            continue;
        }
        const core::Fill* found = nullptr;
        for (const auto& [id, f] : s.fills()) {
            if (f.layer != layer.id) {
                continue;
            }
            const auto region = core::regions::resolve(arrangements, f);
            if (region && region->contains(world)) {
                found = &f;
            }
        }
        if (found) {
            if (found->gradient.active()) {
                if (const auto placed = core::gradient::place(found->gradient, s)) {
                    return Sample{core::gradient::colorAt(*placed, world), QObject::tr("fill")};
                }
                return Sample{found->gradient.average(), QObject::tr("fill")};
            }
            return Sample{found->color, QObject::tr("fill")};
        }
    }
    // Reference images: their own pixel (not faded, nor under the grid).
    if (!view().presentation()) {
        std::optional<Sample> picked;
        for (const core::Layer& layer : s.layers() | std::views::reverse) {  // (bottom first: top wins)
            if (!layer.visible) {
                continue;
            }
            for (const auto& [id, im] : s.images()) {
                if (im.layer != layer.id || !im.visible || !im.contains({world.x, world.y})) {
                    continue;
                }
                const QImage& picture = m_pictures.get(im, im.pixelWidth);
                if (picture.isNull()) {
                    continue;
                }
                core::Point f = im.fractions({world.x, world.y});
                if (im.flipH) {
                    f.x = 1.0 - f.x;
                }
                if (im.flipV) {
                    f.y = 1.0 - f.y;
                }
                const int px = std::clamp(static_cast<int>(f.x * picture.width()), 0, picture.width() - 1);
                const int py = std::clamp(static_cast<int>((1.0 - f.y) * picture.height()), 0, picture.height() - 1);
                const QColor c = picture.pixelColor(px, py);
                picked = Sample{{static_cast<std::uint8_t>(c.red()), static_cast<std::uint8_t>(c.green()),
                                 static_cast<std::uint8_t>(c.blue()), static_cast<std::uint8_t>(c.alpha())},
                                QObject::tr("image")};
            }
        }
        return picked;
    }
    return std::nullopt;
}

void EyedropperTool::use(const core::Color& color, bool second)
{
    SketchView::Paint paint = view().paint();
    (second ? paint.color2 : paint.color1) = color;
    view().setPaint(paint);
    emit view().colorSampled(color);
    emit view().message(QObject::tr("Colour %1 is now %2").arg(second ? 2 : 1).arg(QString::fromStdString(color.hex())));
    view().setTool(view().previousTool());  // (this tool is gone after this)
}

void EyedropperTool::move(const ToolEvent& e)
{
    m_hover = sample(e.screen, e.world);
    m_hoverScreen = e.screen;
    refresh();
}

void EyedropperTool::press(const ToolEvent& e)
{
    if (e.button != Qt::LeftButton && e.button != Qt::RightButton) {
        return;
    }
    const std::optional<Sample> picked = sample(e.screen, e.world);
    if (!picked) {
        emit view().message(QObject::tr("Nothing coloured there"));
        return;
    }
    // Right: Colour 2; left: the selected one (Colour 1 unless Colour 2 is).
    const bool second = e.button == Qt::RightButton || view().selectedColor() == 2;
    use(picked->color, second);  // (this tool is gone after this)
}

void EyedropperTool::paintOverlay(QPainter& painter)
{
    if (!m_hover) {
        return;
    }
    // A swatch by the cursor, with its code.
    const QPointF at = m_hoverScreen + QPointF(16, 16);
    const core::Color& c = m_hover->color;
    painter.setPen(QPen(QColor(0xff, 0xff, 0xff), 1.5));
    painter.setBrush(QColor(c.r, c.g, c.b));
    painter.drawEllipse(QRectF(at, QSizeF(22, 22)));
    const QString label = QString::fromStdString(c.hex());
    const QRectF text(at + QPointF(28, 2), QSizeF(painter.fontMetrics().horizontalAdvance(label) + 10, 18));
    painter.setPen(Qt::NoPen);
    painter.setBrush(QColor(0x2b, 0x2f, 0x36, 230));
    painter.drawRoundedRect(text, 3, 3);
    painter.setPen(QColor(0xdf, 0xe5, 0xec));
    painter.drawText(text, Qt::AlignCenter, label);
}