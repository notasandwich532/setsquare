#include "TransformTools.h"

#include "../Document.h"
#include "../SketchView.h"
#include "ShapeTools.h"

#include <core/Features.h>
#include <core/Geometry.h>
#include <core/Transform.h>

#include <QPainter>

#include <cmath>
#include <functional>
#include <numbers>

using core::Id;
using core::kNoId;
using core::Vec2;

namespace
{
constexpr double kPickPixels = 7.0;
constexpr double kRadToDeg = 180.0 / std::numbers::pi;

// Draws what `make` adds to a scratch copy of the sketch, in the preview style.
void drawGhost(SketchView& view, QPainter& painter, const std::function<void(core::Sketch&)>& make)
{
    core::Sketch scratch = view.document().sketch();
    const Id before = scratch.highestId();
    try {
        make(scratch);
    }
    catch (const core::SketchError&) {
        return;  // can't be done from here: nothing to show
    }
    view.drawPreviewSketch(painter, scratch, before);
}

bool isEnter(int key)
{
    return key == Qt::Key_Return || key == Qt::Key_Enter;
}
}  // namespace

// =============================================================================================
// SelectionTool
// =============================================================================================

std::vector<Id> SelectionTool::items() const
{
    const core::Sketch& s = doc().sketch();
    std::vector<Id> out;
    for (Id id : view().selection()) {
        if (s.hasEntity(id) && !s.isReference(id)) {
            out.push_back(id);
        }
    }
    return out;
}

void SelectionTool::activate()
{
    m_picking = items().empty();
    refresh();
}

void SelectionTool::restart()
{
    m_picking = true;
    refresh();
}

QString SelectionTool::hint() const
{
    if (m_picking) {
        return QObject::tr("%1: click the geometry to use (again to drop it), then press Enter").arg(title());
    }
    return stepHint();
}

void SelectionTool::press(const ToolEvent& e)
{
    if (!m_picking) {
        stepPress(e);
        return;
    }
    if (e.button != Qt::LeftButton) {
        return;
    }
    const Id hit = view().pickAt(e.screen, false);
    const core::Sketch& s = doc().sketch();
    if (hit != kNoId && s.hasEntity(hit) && !s.isReference(hit)) {
        auto selection = view().selection();
        if (!selection.erase(hit)) {
            selection.insert(hit);
        }
        view().setSelection(selection);
    }
    refresh();
}

void SelectionTool::move(const ToolEvent& e)
{
    if (m_picking) {
        view().setHovered(view().pickAt(e.screen, false));
        return;
    }
    stepMove(e);
}

bool SelectionTool::keyPress(int key, const QString& /*text*/)
{
    if (m_picking) {
        if (isEnter(key) && !items().empty()) {
            m_picking = false;
            view().setHovered(kNoId);
            refresh();
            return true;
        }
        return false;
    }
    return stepKey(key);
}

bool SelectionTool::cancel()
{
    return !m_picking && stepCancel();
}

void SelectionTool::paintOverlay(QPainter& painter)
{
    if (!m_picking) {
        paintStep(painter);
    }
}

// =============================================================================================
// Move / Rotate
// =============================================================================================

QString MoveTool::title() const
{
    return m_rotate ? QObject::tr("Rotate") : QObject::tr("Move");
}

QString MoveTool::stepHint() const
{
    const QString copy = QObject::tr(" (Copy option or Ctrl: copy instead)");
    if (m_rotate) {
        switch (m_clicks.size()) {
            case 0: return QObject::tr("Rotate: click the centre to turn about");
            case 1: return QObject::tr("Rotate: click a reference direction");
            default: return QObject::tr("Rotate: click the new direction (Shift: 15° steps)") + copy;
        }
    }
    return m_clicks.empty() ? QObject::tr("Move: click a base point")
                            : QObject::tr("Move: click where the base point goes") + copy;
}

bool MoveTool::copying() const
{
    return view().modifyOptions().copy || (m_modifiers & Qt::ControlModifier);
}

MoveTool::Motion MoveTool::motionTo(Vec2 cursor, Qt::KeyboardModifiers modifiers) const
{
    Motion m;
    if (!m_rotate) {
        if (!m_clicks.empty()) {
            m.dx = cursor.x - m_clicks[0].pos.x;
            m.dy = cursor.y - m_clicks[0].pos.y;
        }
        return m;
    }
    if (m_clicks.size() < 2) {
        return m;
    }
    const Vec2 c = m_clicks[0].pos, r = m_clicks[1].pos;
    double angle = (std::atan2(cursor.y - c.y, cursor.x - c.x) - std::atan2(r.y - c.y, r.x - c.x)) * kRadToDeg;
    while (angle > 180.0) {
        angle -= 360.0;
    }
    while (angle <= -180.0) {
        angle += 360.0;
    }
    if (modifiers & Qt::ShiftModifier) {
        angle = std::round(angle / 15.0) * 15.0;
    }
    m.angle = angle;
    m.about = c;
    return m;
}

void MoveTool::stepPress(const ToolEvent& e)
{
    if (e.button != Qt::LeftButton) {
        return;
    }
    m_modifiers = e.modifiers;
    m_cursor = m_dragging ? e.world : view().snap(e.screen).pos;
    const size_t needed = m_rotate ? 2 : 1;
    if (m_clicks.size() < needed) {
        m_clicks.push_back(view().snap(e.screen));
        if (m_clicks.size() == needed && !copying()) {
            // Move the real geometry from here on, so what's attached follows as it would.
            std::vector<Id> points;
            for (Id id : core::xform::expand(doc().sketch(), items())) {
                if (doc().sketch().entity(id).kind() == core::EntityKind::Point) {
                    points.push_back(id);
                }
            }
            doc().beginDrag(points);
            m_dragging = true;
        }
        refresh();
        return;
    }
    finish(e.modifiers);
}

void MoveTool::finish(Qt::KeyboardModifiers modifiers)
{
    const Motion m = motionTo(m_cursor, modifiers);
    const bool copy = copying();
    if (m_dragging) {
        m_dragging = m_pending = false;
        if (copy) {
            doc().cancelDrag();
        }
        else {
            doc().dragMove(m.dx, m.dy, m.angle, m.about);
            doc().endDrag();
        }
    }
    if (copy) {
        const std::vector<Id> selected = items();
        view().tryEdit(QObject::tr("Copy"), [&](Editor& ed) { ed.copy(selected, m.dx, m.dy, m.angle, m.about); });
    }
    m_clicks.clear();
    refresh();
}

void MoveTool::stepMove(const ToolEvent& e)
{
    m_modifiers = e.modifiers;
    m_cursor = m_dragging ? e.world : view().snap(e.screen).pos;
    if (m_dragging) {
        m_pending = true;
    }
    refresh();
}

void MoveTool::beforePaint()
{
    if (m_pending && m_dragging) {
        m_pending = false;
        const Motion m = motionTo(m_cursor, m_modifiers);
        doc().dragMove(m.dx, m.dy, m.angle, m.about);
    }
}

bool MoveTool::stepCancel()
{
    if (m_clicks.empty()) {
        return false;
    }
    if (m_dragging) {
        m_dragging = m_pending = false;
        doc().cancelDrag();  // everything back where it was
    }
    m_clicks.clear();
    refresh();
    return true;
}

void MoveTool::paintStep(QPainter& painter)
{
    if (m_clicks.empty()) {
        view().drawSnapMarker(painter, Snap{m_cursor});
        return;
    }
    const Vec2 first = m_clicks[0].pos;
    view().drawPreviewLine(painter, first, m_cursor);
    const size_t needed = m_rotate ? 2 : 1;
    if (m_clicks.size() < needed) {
        return;
    }
    const Motion m = motionTo(m_cursor, m_modifiers);
    if (m_rotate) {
        view().drawPreviewLine(painter, first, m_clicks[1].pos);
        view().drawLabel(painter, m_cursor, QStringLiteral("%1°").arg(m.angle, 0, 'f', 1));
    }
    if (!m_dragging) {  // copying: show where the copy will land
        const std::vector<Id> selected = items();
        drawGhost(view(), painter, [&](core::Sketch& s) {
            core::xform::copy(s, selected, Editor::moveMap(m.dx, m.dy, m.angle, m.about));
        });
    }
}

// =============================================================================================
// Scale
// =============================================================================================

QString ScaleTool::stepHint() const
{
    return QObject::tr("Scale: click the point to scale about (factor %1, set in the toolbar)")
        .arg(view().modifyOptions().scaleFactor);
}

void ScaleTool::stepMove(const ToolEvent& e)
{
    m_cursor = view().snap(e.screen);
    refresh();
}

void ScaleTool::stepPress(const ToolEvent& e)
{
    if (e.button != Qt::LeftButton) {
        return;
    }
    const Vec2 about = view().snap(e.screen).pos;
    const double factor = view().modifyOptions().scaleFactor;
    const std::vector<Id> selected = items();
    view().tryEdit(QObject::tr("Scale"), [&](Editor& ed) { ed.scale(selected, factor, about); });
    refresh();
}

void ScaleTool::paintStep(QPainter& painter)
{
    const std::vector<Id> selected = items();
    const double factor = view().modifyOptions().scaleFactor;
    drawGhost(view(), painter, [&](core::Sketch& s) {
        core::xform::copy(s, selected, core::Affine::scaling(m_cursor.pos, factor), false);
    });
    view().drawSnapMarker(painter, m_cursor);
}

// =============================================================================================
// Mirror
// =============================================================================================

QString MirrorTool::stepHint() const
{
    return QObject::tr("Mirror: click the line to mirror across");
}

Id MirrorTool::lineAt(QPointF screen) const
{
    core::PickOptions options;
    options.tolerance = kPickPixels / view().zoom();
    options.points = false;
    const core::Sketch& s = doc().sketch();
    const Id hit = core::pick(s, view().toWorld(screen), options);
    if (hit == kNoId || s.entity(hit).kind() != core::EntityKind::Line) {
        return kNoId;
    }
    return hit;
}

void MirrorTool::stepMove(const ToolEvent& e)
{
    m_line = lineAt(e.screen);
    view().setHovered(m_line);
    refresh();
}

void MirrorTool::stepPress(const ToolEvent& e)
{
    if (e.button != Qt::LeftButton) {
        return;
    }
    const Id line = lineAt(e.screen);
    if (line == kNoId) {
        emit view().message(QObject::tr("Click a line (or an axis) to mirror across"));
        return;
    }
    const std::vector<Id> selected = items();
    if (view().tryEdit(QObject::tr("Mirror"), [&](Editor& ed) { ed.mirror(selected, line); })) {
        m_line = kNoId;
        view().setSelection({});
        restart();
    }
}

void MirrorTool::paintStep(QPainter& painter)
{
    if (m_line == kNoId || !doc().sketch().hasEntity(m_line)) {
        return;
    }
    const std::vector<Id> selected = items();
    drawGhost(view(), painter, [&](core::Sketch& s) { core::xform::mirror(s, selected, m_line); });
}

// =============================================================================================
// Patterns
// =============================================================================================

QString PatternTool::title() const
{
    return m_circular ? QObject::tr("Circular pattern") : QObject::tr("Rectangular pattern");
}

QString PatternTool::stepHint() const
{
    if (m_circular) {
        return QObject::tr("Circular pattern: click the centre point (count and angle are in the toolbar)");
    }
    return QObject::tr("Rectangular pattern: set counts, spacing and direction in the toolbar, then click or "
                       "press Enter");
}

Id PatternTool::centerAt(QPointF screen) const
{
    core::PickOptions options;
    options.tolerance = kPickPixels / view().zoom();
    options.curves = false;
    return core::pick(doc().sketch(), view().toWorld(screen), options);
}

Id PatternTool::apply(Editor& ed, Id center) const
{
    const SketchView::ModifyOptions& o = view().modifyOptions();
    const std::vector<Id> selected = items();
    if (m_circular) {
        return ed.circularPattern(selected, center, {o.circularCount, o.circularAngle});
    }
    return ed.rectangularPattern(selected, {o.patternCount, o.patternSpacing, o.patternAngle, o.patternCount2,
                                            o.patternSpacing2});
}

void PatternTool::finish(Id center)
{
    if (view().tryEdit(title(), [&](Editor& ed) { apply(ed, center); })) {
        m_center = kNoId;
        view().setSelection({});
        restart();
    }
}

void PatternTool::stepMove(const ToolEvent& e)
{
    if (m_circular) {
        m_center = centerAt(e.screen);
        view().setHovered(m_center);
    }
    refresh();
}

void PatternTool::stepPress(const ToolEvent& e)
{
    if (e.button != Qt::LeftButton) {
        return;
    }
    if (!m_circular) {
        finish(kNoId);
        return;
    }
    const Id center = centerAt(e.screen);
    if (center == kNoId) {
        emit view().message(QObject::tr("Click a point (or the origin) to pattern around"));
        return;
    }
    finish(center);
}

bool PatternTool::stepKey(int key)
{
    if (!m_circular && isEnter(key)) {
        finish(kNoId);
        return true;
    }
    return false;
}

void PatternTool::paintStep(QPainter& painter)
{
    if (m_circular && (m_center == kNoId || !doc().sketch().hasEntity(m_center))) {
        return;
    }
    drawPreviewEdit(view(), painter, [&](Editor& ed) { apply(ed, m_center); });
}

// =============================================================================================
// Offset
// =============================================================================================

QString OffsetTool::hint() const
{
    return QObject::tr("Offset: click a curve to offset its chain to the side the cursor is on, then type the "
                       "distance");
}

std::vector<Id> OffsetTool::chainAt(QPointF screen) const
{
    core::PickOptions options;
    options.tolerance = kPickPixels / view().zoom();
    options.points = false;
    const core::Sketch& s = doc().sketch();
    const Id hit = core::pick(s, view().toWorld(screen), options);
    if (hit == kNoId || s.isReference(hit)) {
        return {};
    }
    try {
        return core::feature::chainAt(s, hit);
    }
    catch (const core::SketchError&) {
        return {};
    }
}

void OffsetTool::move(const ToolEvent& e)
{
    m_mouse = e.world;
    m_chain = chainAt(e.screen);
    view().setHovered(m_chain.empty() ? kNoId : m_chain.front());
    refresh();
}

void OffsetTool::press(const ToolEvent& e)
{
    if (e.button != Qt::LeftButton) {
        return;
    }
    const std::vector<Id> chain = chainAt(e.screen);
    if (chain.empty()) {
        return;
    }
    Id made = kNoId;
    const double distance = view().modifyOptions().offsetDistance;
    if (view().tryEdit(QObject::tr("Offset"), [&](Editor& ed) { made = ed.offset(chain, distance, e.world); })) {
        view().editValue(made, e.screen);  // type the distance straight away
    }
    m_chain.clear();
    refresh();
}

void OffsetTool::paintOverlay(QPainter& painter)
{
    if (m_chain.empty()) {
        return;
    }
    const double distance = view().modifyOptions().offsetDistance;
    drawPreviewEdit(view(), painter, [&](Editor& ed) { ed.offset(m_chain, distance, m_mouse); });
}

// =============================================================================================
// Paste
// =============================================================================================

QString PasteTool::hint() const
{
    return QObject::tr("Paste: click to place (Esc when done)");
}

void PasteTool::move(const ToolEvent& e)
{
    m_cursor = view().snap(e.screen);
    refresh();
}

void PasteTool::press(const ToolEvent& e)
{
    if (e.button != Qt::LeftButton) {
        return;
    }
    const Vec2 at = view().snap(e.screen).pos;
    std::map<Id, Id> made;
    if (view().tryEdit(QObject::tr("Paste"), [&](Editor& ed) { made = ed.paste(m_fragment, at); })) {
        std::set<Id> pasted;
        for (const auto& [from, to] : made) {
            if (m_fragment.hasEntity(from)) {
                pasted.insert(to);
            }
        }
        view().setSelection(std::move(pasted));
    }
    refresh();
}

void PasteTool::paintOverlay(QPainter& painter)
{
    drawGhost(view(), painter, [&](core::Sketch& s) { core::xform::paste(s, m_fragment, m_cursor.pos); });
    view().drawSnapMarker(painter, m_cursor);
}
