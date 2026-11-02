#pragma once

#include "Tool.h"

#include <core/Sketch.h>

// Project: click geometry in another layer (curves or points) to bring a linked copy of it into
// the active layer, where it can be snapped to, constrained against and dimensioned. Copies are
// fixed (they follow the original) and construction until made solid; double-click takes a curve's
// whole chain. Break Link (Sketch menu) makes a copy ordinary geometry.
class ProjectTool : public Tool
{
public:
    using Tool::Tool;

    QString hint() const override;
    void move(const ToolEvent& e) override;
    void press(const ToolEvent& e) override;
    void doubleClick(const ToolEvent& e) override;

private:
    core::Id pickAt(QPointF screen) const;  // geometry in another (shown) layer
    void project(const std::vector<core::Id>& sources);

    core::Id m_hover = core::kNoId;
};
