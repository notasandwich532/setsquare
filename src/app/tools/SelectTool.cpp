#include "SelectTool.h"

#include "../Document.h"
#include "../SketchView.h"
#include "../Theme.h"
#include "StyleTools.h"

#include <core/Dimensions.h>

#include <QPainter>

#include <cmath>
#include <set>

namespace
{
constexpr double kDragThresholdPx = 4.0;
}

QString SelectTool::hint() const
{
    switch (m_state) {
        case State::Dragging: return QObject::tr("Dragging - release to finish");
        case State::BoxSelecting:
            return m_boxEnd.x() >= m_pressScreen.x()
                ? QObject::tr("Window select: entities fully inside")
                : QObject::tr("Crossing select: anything touched");
        default:
            return QObject::tr("Click to select (Shift adds). Drag geometry to move it. Drag on empty space to "
                               "box-select: left-to-right takes what's inside, right-to-left anything touched. Right-click: recent tools");
    }
}

void SelectTool::press(const ToolEvent& e)
{
    if (e.button != Qt::LeftButton) {
        return;
    }
    m_pressScreen = e.screen;
    m_pressWorld = e.world;
    m_additive = e.modifiers & (Qt::ShiftModifier | Qt::ControlModifier);

    const core::Id hit = view().pickAt(e.screen, true, true);
    if (hit != core::kNoId) {
        auto selection = view().selection();
        if (m_additive) {
            if (!selection.erase(hit)) {
                selection.insert(hit);
            }
        }
        else if (!selection.contains(hit)) {
            selection = {hit};
        }
        view().setSelection(selection);
        m_pressed = hit;
        m_state = State::PressedOnEntity;
    }
    else {
        if (!m_additive) {
            view().setSelection({});
        }
        m_boxEnd = e.screen;
        m_state = State::BoxSelecting;
    }
    refresh();
}

void SelectTool::startDrag()
{
    const core::Sketch& sketch = doc().sketch();
    const auto& selection = view().selection();

    if (sketch.hasConstraint(m_pressed)) {
        const core::Constraint& c = sketch.constraint(m_pressed);
        if (core::isDimension(c.type)) {
            m_labelStart = core::layoutDimension(sketch, c).label;
            doc().beginLabelDrag();
            m_state = State::DraggingLabel;
        }
        else {
            m_state = State::Idle;  // constraint symbols don't move
        }
        return;
    }

    if (!sketch.hasEntity(m_pressed) && !sketch.hasText(m_pressed)) {
        m_state = State::Idle;
        return;
    }
    const core::EntityKind kind =
        sketch.hasText(m_pressed) ? core::EntityKind::Point : sketch.entity(m_pressed).kind();  // (text: its anchor)
    const bool round = kind == core::EntityKind::Circle || kind == core::EntityKind::Arc;

    if (round && selection.size() <= 1) {
        // Grabbing the rim of a circle/arc resizes it rather than moving it.
        doc().beginDragEdge(m_pressed, m_pressWorld.x, m_pressWorld.y);
    }
    else {
        std::set<core::Id> points;
        const std::set<core::Id> grabbed = selection.contains(m_pressed) ? selection : std::set{m_pressed};
        for (core::Id id : grabbed) {
            if (sketch.hasEntity(id)) {
                for (core::Id p : sketch.definingPoints(id)) {
                    points.insert(p);
                }
            }
            else if (sketch.hasText(id)) {
                for (core::Id p : core::textPoints(sketch, sketch.text(id))) {
                    points.insert(p);  // a text moves by its anchor (and its box, all of it)
                }
            }
        }
        doc().beginDrag({points.begin(), points.end()});
    }
    m_state = State::Dragging;
}

void SelectTool::move(const ToolEvent& e)
{
    switch (m_state) {
        case State::Idle:
            view().setHovered(view().pickAt(e.screen, true, true));
            return;

        case State::PressedOnEntity: {
            const QPointF delta = e.screen - m_pressScreen;
            if (std::hypot(delta.x(), delta.y()) < kDragThresholdPx) {
                return;
            }
            startDrag();
            if (m_state != State::Dragging) {
                move(e);  // label drag (or nothing)
                return;
            }
            [[fallthrough]];
        }
        case State::Dragging:
            m_dragOffset = {e.world.x - m_pressWorld.x, e.world.y - m_pressWorld.y};
            m_dragPending = true;
            refresh();  // requests a frame; the solve happens in beforePaint()
            return;

        case State::DraggingLabel: {
            const core::Sketch& sketch = doc().sketch();
            if (!sketch.hasConstraint(m_pressed)) {
                return;
            }
            const core::Vec2 label{m_labelStart.x + e.world.x - m_pressWorld.x,
                                   m_labelStart.y + e.world.y - m_pressWorld.y};
            const core::Vec2 offset = core::labelOffsetFor(sketch, sketch.constraint(m_pressed), label);
            doc().moveLabel(m_pressed, offset.x, offset.y);
            return;
        }

        case State::BoxSelecting:
            m_boxEnd = e.screen;
            refresh();
            return;
    }
}

void SelectTool::doubleClick(const ToolEvent& e)
{
    const core::Id hit = view().pickAt(e.screen, true, true);
    const core::Sketch& sketch = doc().sketch();
    if (sketch.hasText(hit)) {
        view().editText(hit);
        return;
    }
    // A gradient: into the gradient tool, editing it (a curve's stroke, else the fill there).
    if (sketch.hasEntity(hit) && sketch.stroke(hit) && sketch.stroke(hit)->gradient.active()) {
        view().editGradient(hit);
        return;
    }
    if (hit == core::kNoId) {
        const core::regions::Arrangement arrangement(sketch, sketch.activeLayer());
        const core::Id fill = topFillAt(sketch, arrangement, e.world);
        if (fill != core::kNoId && sketch.fill(fill).gradient.active()) {
            view().editGradient(fill);
        }
        else if (const core::Id image = view().imageAt(e.world); image != core::kNoId && !view().presentation()) {
            view().editImage(image);  // a reference image: placing it
        }
        return;
    }
    if (!sketch.hasConstraint(hit)) {
        return;
    }
    const core::ConstraintType type = sketch.constraint(hit).type;
    if (core::isDimension(type) || type == core::ConstraintType::Ratio) {
        view().editDimension(hit);
    }
    else if (core::isFeature(type)) {
        view().editFeature(hit);  // its symbol: back into its command
    }
}

void SelectTool::release(const ToolEvent& e)
{
    if (e.button != Qt::LeftButton) {
        return;
    }
    if (m_state == State::Dragging) {
        m_dragOffset = {e.world.x - m_pressWorld.x, e.world.y - m_pressWorld.y};
        m_dragPending = true;
        applyPendingDrag();  // land exactly where the mouse was released
        doc().endDrag();
    }
    else if (m_state == State::DraggingLabel) {
        doc().endLabelDrag();
    }
    else if (m_state == State::BoxSelecting) {
        const QPointF delta = m_boxEnd - m_pressScreen;
        if (std::hypot(delta.x(), delta.y()) >= kDragThresholdPx) {
            const bool crossing = m_boxEnd.x() < m_pressScreen.x();
            const core::Vec2 a = view().toWorld(m_pressScreen);
            const core::Vec2 b = view().toWorld(m_boxEnd);
            auto selection = view().selection();
            const core::Sketch& s = doc().sketch();
            for (core::Id id : core::boxSelect(s, a, b, crossing, s.activeSublayer())) {
                // In presentation view only what's shown: no points or construction.
                if (view().presentation()
                    && (s.entity(id).construction || s.entity(id).kind() == core::EntityKind::Point)) {
                    continue;
                }
                selection.insert(id);
            }
            view().setSelection(selection);
        }
    }
    m_state = State::Idle;
    m_pressed = core::kNoId;
    refresh();
}

bool SelectTool::cancel()
{
    if (m_state == State::Dragging) {
        m_dragPending = false;
        doc().cancelDrag();  // Esc mid-drag puts everything back
        m_state = State::Idle;
        refresh();
        return true;
    }
    if (m_state == State::BoxSelecting) {
        m_state = State::Idle;
        refresh();
        return true;
    }
    if (m_state == State::DraggingLabel) {
        doc().endLabelDrag();
        m_state = State::Idle;
        refresh();
        return true;
    }
    return false;
}

void SelectTool::applyPendingDrag()
{
    if (m_dragPending && m_state == State::Dragging) {
        m_dragPending = false;
        doc().dragTo(m_dragOffset.x, m_dragOffset.y);
    }
}

void SelectTool::beforePaint()
{
    applyPendingDrag();
}

void SelectTool::paintOverlay(QPainter& painter)
{
    if (m_state != State::BoxSelecting) {
        return;
    }
    const QRectF rect = QRectF(m_pressScreen, m_boxEnd).normalized();
    const bool crossing = m_boxEnd.x() < m_pressScreen.x();
    const QColor accent = crossing ? QColor(0x5a, 0xc8, 0x7a) : QColor(0x4d, 0xa3, 0xff);
    QColor fill = accent;
    fill.setAlpha(40);
    QPen pen(accent, 1.0, crossing ? Qt::DashLine : Qt::SolidLine);
    painter.setPen(pen);
    painter.setBrush(fill);
    painter.drawRect(rect);
    painter.setBrush(Qt::NoBrush);
}
