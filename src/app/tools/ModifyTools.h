#pragma once

#include "Tool.h"

#include "../Editor.h"

#include <core/Modify.h>

#include <QObject>

#include <vector>

// Trim / Extend / Break: hover a curve to see what will happen, click to do it. Stays active.
class CurveOpTool : public Tool
{
public:
    enum class Op
    {
        Trim,
        TrimToConstruction,
        Extend,
        Break,
    };
    CurveOpTool(SketchView& view, Op op)
        : Tool(view)
        , m_op(op)
    {}

    QString hint() const override;
    void press(const ToolEvent& e) override;
    void move(const ToolEvent& e) override;
    void paintOverlay(QPainter& painter) override;

private:
    core::Id curveAt(QPointF screen) const;

    Op m_op;
    core::Id m_hover = core::kNoId;
    core::Vec2 m_mouse;
};

// Fillet / Chamfer: click two curves (or the corner point where they meet). The corner is
// made at the default size from the toolbar, then its value editor opens.
class CornerTool : public Tool
{
public:
    enum class Kind
    {
        Fillet,
        ChamferEqual,
        ChamferTwoDistances,
        ChamferDistanceAngle,
    };
    CornerTool(SketchView& view, Kind kind)
        : Tool(view)
        , m_kind(kind)
    {}

    QString hint() const override;
    void press(const ToolEvent& e) override;
    void move(const ToolEvent& e) override;
    bool cancel() override;
    void paintOverlay(QPainter& painter) override;

private:
    // Applies to a pair of curves; returns the dimension to edit (or kNoId).
    core::modify::Corner apply(Editor& ed, core::Id a, core::Vec2 nearA, core::Id b, core::Vec2 nearB) const;
    // The two curves the mouse would complete: an existing first pick plus the hovered curve,
    // or the corner at a hovered point.
    bool candidate(QPointF screen, core::Vec2 world, core::Id& a, core::Vec2& nearA, core::Id& b,
                   core::Vec2& nearB) const;
    void finish(core::Id a, core::Vec2 nearA, core::Id b, core::Vec2 nearB);

    Kind m_kind;
    core::Id m_first = core::kNoId;
    core::Vec2 m_firstAt;
    QPointF m_mouseScreen;
    core::Vec2 m_mouse;
};
