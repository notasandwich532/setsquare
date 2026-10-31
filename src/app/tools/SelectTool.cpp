#include "SelectTool.h"

#include "../Document.h"
#include "../SketchView.h"
#include "../Theme.h"

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
            return QObject::tr("Click to select, Shift+click to add, drag geometry to move it, "
                               "drag empty space to box-select");
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

    const core::Id hit = view().pickAt(e.screen);
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
    const core::EntityKind kind = sketch.entity(m_pressed).kind();
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
        }
        doc().beginDrag({points.begin(), points.end()});
    }
    m_state = State::Dragging;
}

void SelectTool::move(const ToolEvent& e)
{
    switch (m_state) {
        case State::Idle:
            view().setHovered(view().pickAt(e.screen));
            return;

        case State::PressedOnEntity: {
            const QPointF delta = e.screen - m_pressScreen;
            if (std::hypot(delta.x(), delta.y()) < kDragThresholdPx) {
                return;
            }
            startDrag();
            [[fallthrough]];
        }
        case State::Dragging:
            doc().dragTo(e.world.x - m_pressWorld.x, e.world.y - m_pressWorld.y);
            refresh();
            return;

        case State::BoxSelecting:
            m_boxEnd = e.screen;
            refresh();
            return;
    }
}

void SelectTool::release(const ToolEvent& e)
{
    if (e.button != Qt::LeftButton) {
        return;
    }
    if (m_state == State::Dragging) {
        doc().endDrag();
    }
    else if (m_state == State::BoxSelecting) {
        const QPointF delta = m_boxEnd - m_pressScreen;
        if (std::hypot(delta.x(), delta.y()) >= kDragThresholdPx) {
            const bool crossing = m_boxEnd.x() < m_pressScreen.x();
            const core::Vec2 a = view().toWorld(m_pressScreen);
            const core::Vec2 b = view().toWorld(m_boxEnd);
            auto selection = view().selection();
            for (core::Id id : core::boxSelect(doc().sketch(), a, b, crossing)) {
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
    return false;
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
