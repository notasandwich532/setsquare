#pragma once

#include <core/Geometry.h>

#include <QPointF>
#include <QString>
#include <Qt>

class Document;
class QPainter;
class SketchView;

enum class ToolKind
{
    Select,
    Line,
    Rectangle,
    Circle,
    ArcCenter,
    Arc3Point,

    Dimension,
    Coincident,
    HorizontalVertical,
    Parallel,
    Perpendicular,
    Tangent,
    Equal,
    Midpoint,
    Concentric,
    Symmetric,
    Fix,
};

struct ToolEvent
{
    QPointF screen;
    core::Vec2 world;
    Qt::MouseButton button = Qt::NoButton;  // the button that changed (press/release)
    Qt::MouseButtons buttons;               // buttons held
    Qt::KeyboardModifiers modifiers;
};

// An interaction mode of the sketch view. The view forwards input here and calls
// paintOverlay() after drawing the sketch, so tools can show previews.
class Tool
{
public:
    explicit Tool(SketchView& view)
        : m_view(view)
    {}
    virtual ~Tool() = default;

    virtual QString hint() const = 0;

    // Called once the tool is current (e.g. to act on the existing selection).
    virtual void activate() {}

    virtual void press(const ToolEvent&) {}
    virtual void move(const ToolEvent&) {}
    virtual void release(const ToolEvent&) {}
    virtual void doubleClick(const ToolEvent&) {}

    // Esc. Return true when the tool consumed it (e.g. abandoned a shape in progress);
    // otherwise the view falls back to the select tool.
    virtual bool cancel() { return false; }

    // Called once per frame before painting. Tools defer expensive work (like the drag solve)
    // to here so it runs at most once per displayed frame, with the newest input.
    virtual void beforePaint() {}

    virtual void paintOverlay(QPainter&) {}

protected:
    SketchView& view() const { return m_view; }
    Document& doc() const;
    void refresh() const;  // repaint and re-publish the hint

private:
    SketchView& m_view;
};
