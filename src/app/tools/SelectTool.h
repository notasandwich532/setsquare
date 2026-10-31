#pragma once

#include "Tool.h"

// Click/shift-click selection, window (left-to-right) and crossing (right-to-left) box
// selection, and dragging geometry with a live solve.
class SelectTool : public Tool
{
public:
    using Tool::Tool;

    QString hint() const override;
    void press(const ToolEvent& e) override;
    void move(const ToolEvent& e) override;
    void release(const ToolEvent& e) override;
    bool cancel() override;
    void paintOverlay(QPainter& painter) override;

private:
    enum class State
    {
        Idle,
        PressedOnEntity,
        Dragging,
        BoxSelecting,
    };

    void startDrag();

    State m_state = State::Idle;
    core::Id m_pressed = core::kNoId;
    QPointF m_pressScreen;
    core::Vec2 m_pressWorld;
    QPointF m_boxEnd;
    bool m_additive = false;
};
