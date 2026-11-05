#pragma once

#include "ShapeTools.h"

#include <core/Sketch.h>

#include <vector>

// Ellipse: click the centre, then the end of one semi-axis (its direction and length), then
// anywhere to set the other semi-axis (its distance from the first axis). The axis ends are
// points that can be dragged, snapped to and dimensioned.
class EllipseTool : public StepTool
{
public:
    using StepTool::StepTool;

protected:
    int steps() const override { return 3; }
    QString hintFor(size_t step) const override;
    QString label() const override { return QObject::tr("Ellipse"); }
    void build(Editor& ed, const std::vector<Snap>& clicks) override;
    bool snaps(size_t step) const override { return step < 2; }
};

// Spline: click its points (each can snap to geometry, as a line's ends do). Through them (fit)
// or pulled by them (control). Enter or double-click finishes it; clicking the first point again
// closes it; Backspace takes the last point back; Esc throws it away.
class SplineTool : public Tool
{
public:
    SplineTool(SketchView& view, core::SplineKind kind)
        : Tool(view)
        , m_kind(kind)
    {}

    QString hint() const override;
    void press(const ToolEvent& e) override;
    void move(const ToolEvent& e) override;
    void doubleClick(const ToolEvent& e) override;
    bool cancel() override;
    bool keyPress(int key, const QString& text) override;
    void paintOverlay(QPainter& painter) override;

private:
    void finish(bool closed);
    void build(Editor& ed, const std::vector<Snap>& clicks, bool closed) const;
    bool closesAt(const Snap& snap) const;  // on the first point (with enough points to close)

    core::SplineKind m_kind;
    std::vector<Snap> m_clicks;
    Snap m_cursor;
};
