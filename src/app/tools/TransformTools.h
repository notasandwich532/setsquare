#pragma once

#include "DrawTools.h"
#include "Tool.h"

#include "../Editor.h"

#include <core/Sketch.h>

#include <QObject>

#include <vector>

// Tools that act on a group of selected items: move/rotate, scale, mirror and patterns. If nothing
// is selected when one starts, clicks pick the items first and Enter goes on; with a selection,
// it goes straight to its own steps. Each previews the result before the final click.
class SelectionTool : public Tool
{
public:
    using Tool::Tool;

    QString hint() const override;
    void activate() override;
    void press(const ToolEvent& e) override;
    void move(const ToolEvent& e) override;
    bool keyPress(int key, const QString& text) override;
    bool cancel() override;
    void paintOverlay(QPainter& painter) override;

protected:
    virtual QString title() const = 0;
    virtual QString stepHint() const = 0;
    virtual void stepPress(const ToolEvent& e) = 0;
    virtual void stepMove(const ToolEvent&) {}
    virtual bool stepCancel() { return false; }  // true if it backed out of a step
    virtual bool stepKey(int /*key*/) { return false; }
    virtual void paintStep(QPainter&) {}

    bool picking() const { return m_picking; }
    // The selected sketch items (geometry only).
    std::vector<core::Id> items() const;
    // Back to picking items, e.g. after the selection was used up.
    void restart();

private:
    bool m_picking = false;
};

// Move (or Copy): click a base point, then where it goes. Rotate: click the centre, a reference
// direction, then the new direction (Shift snaps to 15°). Moving drags the geometry live, so
// whatever it's connected to follows; with the Copy option (or Ctrl on the last click) the
// selection is copied instead.
class MoveTool : public SelectionTool
{
public:
    MoveTool(SketchView& view, bool rotate)
        : SelectionTool(view)
        , m_rotate(rotate)
    {}

    void beforePaint() override;

protected:
    QString title() const override;
    QString stepHint() const override;
    void stepPress(const ToolEvent& e) override;
    void stepMove(const ToolEvent& e) override;
    bool stepCancel() override;
    void paintStep(QPainter& painter) override;

private:
    struct Motion
    {
        double dx = 0.0, dy = 0.0, angle = 0.0;
        core::Vec2 about;
    };
    Motion motionTo(core::Vec2 cursor, Qt::KeyboardModifiers modifiers) const;
    bool copying() const;
    void finish(Qt::KeyboardModifiers modifiers);

    bool m_rotate;
    std::vector<Snap> m_clicks;  // base (move) or centre + reference (rotate)
    core::Vec2 m_cursor;
    Qt::KeyboardModifiers m_modifiers;
    bool m_dragging = false;  // a live move is in progress (not copying)
    bool m_pending = false;   // the live move needs updating before the next frame
};

// Scale: hover where the centre goes (snapping to points), click to scale the selection by the
// factor set in the toolbar.
class ScaleTool : public SelectionTool
{
public:
    using SelectionTool::SelectionTool;

protected:
    QString title() const override { return QObject::tr("Scale"); }
    QString stepHint() const override;
    void stepPress(const ToolEvent& e) override;
    void stepMove(const ToolEvent& e) override;
    void paintStep(QPainter& painter) override;

private:
    Snap m_cursor;
};

// Mirror: click the line to mirror across (the axes work too).
class MirrorTool : public SelectionTool
{
public:
    using SelectionTool::SelectionTool;

protected:
    QString title() const override { return QObject::tr("Mirror"); }
    QString stepHint() const override;
    void stepPress(const ToolEvent& e) override;
    void stepMove(const ToolEvent& e) override;
    void paintStep(QPainter& painter) override;

private:
    core::Id lineAt(QPointF screen) const;
    core::Id m_line = core::kNoId;
};

// Rectangular pattern: previews with the toolbar's counts, spacings and direction; click (or
// Enter) to make it. Circular pattern: click the centre point.
class PatternTool : public SelectionTool
{
public:
    PatternTool(SketchView& view, bool circular)
        : SelectionTool(view)
        , m_circular(circular)
    {}

protected:
    QString title() const override;
    QString stepHint() const override;
    void stepPress(const ToolEvent& e) override;
    void stepMove(const ToolEvent& e) override;
    bool stepKey(int key) override;
    void paintStep(QPainter& painter) override;

private:
    core::Id apply(Editor& ed, core::Id center) const;
    void finish(core::Id center);
    core::Id centerAt(QPointF screen) const;

    bool m_circular;
    core::Id m_center = core::kNoId;  // hovered centre (circular)
};

// Offset: hover a curve to preview its chain offset to the side the cursor is on, click to make
// it (at the toolbar's distance), then type the distance.
class OffsetTool : public Tool
{
public:
    using Tool::Tool;

    QString hint() const override;
    void press(const ToolEvent& e) override;
    void move(const ToolEvent& e) override;
    void paintOverlay(QPainter& painter) override;

private:
    std::vector<core::Id> chainAt(QPointF screen) const;
    std::vector<core::Id> m_chain;
    core::Vec2 m_mouse;
};

// Paste: the clipboard's contents follow the cursor; click to place them.
class PasteTool : public Tool
{
public:
    PasteTool(SketchView& view, core::Sketch fragment)
        : Tool(view)
        , m_fragment(std::move(fragment))
    {}

    QString hint() const override;
    void press(const ToolEvent& e) override;
    void move(const ToolEvent& e) override;
    void paintOverlay(QPainter& painter) override;

private:
    core::Sketch m_fragment;
    Snap m_cursor;
};
