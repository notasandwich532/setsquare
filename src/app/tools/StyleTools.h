#pragma once

#include "Tool.h"

#include <core/Sketch.h>

// Stroke brush: click a curve to give it the current stroke (colour 1, width and line style from
// the colour bar); double-click for its whole chain; drag a box to do every curve in it (left to
// right: those inside; right to left: those touched). Ctrl takes strokes off instead (click or
// box); Alt+click picks a curve's stroke up into the colour bar (an eyedropper). Construction
// geometry can't be decorated.
class StrokeBrushTool : public Tool
{
public:
    using Tool::Tool;

    QString hint() const override;
    void press(const ToolEvent& e) override;
    void move(const ToolEvent& e) override;
    void release(const ToolEvent& e) override;
    void doubleClick(const ToolEvent& e) override;
    bool cancel() override;
    void paintOverlay(QPainter& painter) override;

private:
    core::Id curveAt(QPointF screen) const;
    void clickAt(QPointF screen, Qt::KeyboardModifiers modifiers);
    void applyBox(bool clear);
    void apply(const std::vector<core::Id>& curves, bool clear);

    core::Id m_hover = core::kNoId;
    bool m_pressed = false;
    bool m_boxing = false;
    QPointF m_pressScreen;
    QPointF m_boxEnd;
    Qt::KeyboardModifiers m_pressModifiers;
};
