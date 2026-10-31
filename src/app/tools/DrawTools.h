#pragma once

#include "Tool.h"

// A position picked while drawing: either free space or an existing point to reuse, so that
// shapes drawn onto existing endpoints are connected.
struct Snap
{
    core::Vec2 pos;
    core::Id point = core::kNoId;
};

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
    bool m_active = false;
    Snap m_start;
    core::Id m_chainFirst = core::kNoId;
    Snap m_cursor;
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
