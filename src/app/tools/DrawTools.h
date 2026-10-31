#pragma once

#include "Tool.h"

// A position picked while drawing, with what it was inferred from. Turning a snap into a
// point (see resolveSnap) reuses the existing point, or adds the matching constraint, so
// shapes come out connected the way they were drawn.
struct Snap
{
    core::Vec2 pos;
    core::Id point = core::kNoId;       // reuse this existing point
    core::Id midpointOf = core::kNoId;  // new point, constrained to this line's midpoint
    core::Id onCurve = core::kNoId;     // new point, constrained onto this curve

    bool isFree() const { return point == core::kNoId && midpointOf == core::kNoId && onCurve == core::kNoId; }
};

// The point for a snap inside an edit: the existing point, or a new one plus its inferred
// constraint (midpoint / point-on-curve).
core::Id resolveSnap(core::Sketch& sketch, const Snap& snap);

// Polyline: each click adds a segment joined to the previous one. Esc, right-click or
// double-click ends the chain; clicking the chain's first point closes it.
class LineTool : public Tool
{
public:
    using Tool::Tool;
    QString hint() const override;
    void press(const ToolEvent& e) override;
    void move(const ToolEvent& e) override;
    void doubleClick(const ToolEvent& e) override;
    bool cancel() override;
    void paintOverlay(QPainter& painter) override;

private:
    // Cursor snap, plus horizontal/vertical inference relative to the start point.
    void updateCursor(QPointF screen);

    bool m_active = false;
    Snap m_start;
    core::Id m_chainFirst = core::kNoId;
    Snap m_cursor;
    enum class Axis
    {
        None,
        Horizontal,
        Vertical,
    } m_axis = Axis::None;
};

// Two opposite corners; adds horizontal/vertical constraints to the sides.
class RectangleTool : public Tool
{
public:
    using Tool::Tool;
    QString hint() const override;
    void press(const ToolEvent& e) override;
    void move(const ToolEvent& e) override;
    bool cancel() override;
    void paintOverlay(QPainter& painter) override;

private:
    bool m_active = false;
    Snap m_first;
    Snap m_cursor;
};

// Centre, then a point on the rim.
class CircleTool : public Tool
{
public:
    using Tool::Tool;
    QString hint() const override;
    void press(const ToolEvent& e) override;
    void move(const ToolEvent& e) override;
    bool cancel() override;
    void paintOverlay(QPainter& painter) override;

private:
    bool m_active = false;
    Snap m_center;
    Snap m_cursor;
};

// Centre-point arc: centre, start, end. The sweep direction follows the mouse.
class CenterArcTool : public Tool
{
public:
    using Tool::Tool;
    QString hint() const override;
    void press(const ToolEvent& e) override;
    void move(const ToolEvent& e) override;
    bool cancel() override;
    void paintOverlay(QPainter& painter) override;

private:
    bool preview(core::ArcShape& shape) const;

    int m_step = 0;  // clicks so far
    Snap m_center;
    Snap m_start;
    Snap m_cursor;
    double m_sweep = 0.0;      // accumulated signed sweep (radians)
    double m_lastAngle = 0.0;  // of the cursor, for accumulating the sweep
};

// Three-point arc: start, end, then a point the arc passes through.
class ThreePointArcTool : public Tool
{
public:
    using Tool::Tool;
    QString hint() const override;
    void press(const ToolEvent& e) override;
    void move(const ToolEvent& e) override;
    bool cancel() override;
    void paintOverlay(QPainter& painter) override;

private:
    bool preview(core::ArcShape& shape, bool& reversed) const;

    int m_step = 0;
    Snap m_start;
    Snap m_end;
    Snap m_cursor;
};
