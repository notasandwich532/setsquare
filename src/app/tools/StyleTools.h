#pragma once

#include "Tool.h"

#include <core/Sketch.h>

// Stroke brush: click a curve to give it the current stroke (colour 1, width and line style from
// the colour bar); double-click for its whole chain. Ctrl+click takes a stroke off; Alt+click
// picks a curve's stroke up into the colour bar (an eyedropper). Construction geometry can't be
// decorated.
class StrokeBrushTool : public Tool
{
public:
    using Tool::Tool;

    QString hint() const override;
    void press(const ToolEvent& e) override;
    void move(const ToolEvent& e) override;
    void doubleClick(const ToolEvent& e) override;
    void paintOverlay(QPainter& painter) override;

private:
    core::Id curveAt(QPointF screen) const;
    core::Id m_hover = core::kNoId;
};
