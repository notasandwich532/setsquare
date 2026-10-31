#pragma once

#include "DrawTools.h"

#include <core/Builders.h>

#include <QObject>

#include <functional>
#include <vector>

// Tools that place a shape with a fixed number of clicks. The preview builds the shape into a
// copy of the sketch with the real builder (so preview and result can't disagree) and draws
// whatever was added.
class StepTool : public Tool
{
public:
    using Tool::Tool;

    QString hint() const override;
    void press(const ToolEvent& e) override;
    void move(const ToolEvent& e) override;
    bool cancel() override;
    void paintOverlay(QPainter& painter) override;

protected:
    virtual int steps() const = 0;
    virtual QString hintFor(size_t step) const = 0;
    virtual QString label() const = 0;
    // clicks.size() == steps(); the last may be the live cursor when previewing.
    virtual void build(Editor& ed, const std::vector<Snap>& clicks) = 0;
    // Steps that only measure (a width, a side) don't snap to geometry.
    virtual bool snaps(size_t /*step*/) const { return true; }
    virtual void tracked(const Snap& /*cursor*/) {}  // e.g. accumulate an arc's sweep

    std::vector<Snap> m_clicks;
    Snap m_cursor;

private:
    Snap snapAt(QPointF screen) const;
};

// Draws a preview of `edit` applied to a scratch copy of the document's sketch.
void drawPreviewEdit(SketchView& view, QPainter& painter, const std::function<void(Editor&)>& edit);

class PointTool : public StepTool
{
public:
    using StepTool::StepTool;

protected:
    int steps() const override { return 1; }
    QString hintFor(size_t) const override;
    QString label() const override { return QObject::tr("Point"); }
    void build(Editor& ed, const std::vector<Snap>& clicks) override;
};

class Rectangle3Tool : public StepTool
{
public:
    using StepTool::StepTool;

protected:
    int steps() const override { return 3; }
    QString hintFor(size_t step) const override;
    QString label() const override { return QObject::tr("Rectangle"); }
    void build(Editor& ed, const std::vector<Snap>& clicks) override;
    bool snaps(size_t step) const override { return step < 2; }
};

class RectangleCenterTool : public StepTool
{
public:
    using StepTool::StepTool;

protected:
    int steps() const override { return 2; }
    QString hintFor(size_t step) const override;
    QString label() const override { return QObject::tr("Rectangle"); }
    void build(Editor& ed, const std::vector<Snap>& clicks) override;
    bool snaps(size_t step) const override { return step == 0; }
};

class Circle2Tool : public StepTool
{
public:
    using StepTool::StepTool;

protected:
    int steps() const override { return 2; }
    QString hintFor(size_t step) const override;
    QString label() const override { return QObject::tr("Circle"); }
    void build(Editor& ed, const std::vector<Snap>& clicks) override;
};

class Circle3Tool : public StepTool
{
public:
    using StepTool::StepTool;

protected:
    int steps() const override { return 3; }
    QString hintFor(size_t step) const override;
    QString label() const override { return QObject::tr("Circle"); }
    void build(Editor& ed, const std::vector<Snap>& clicks) override;
};

class PolygonTool : public StepTool
{
public:
    PolygonTool(SketchView& view, core::build::PolygonKind kind)
        : StepTool(view)
        , m_kind(kind)
    {}

protected:
    int steps() const override { return 2; }
    QString hintFor(size_t step) const override;
    QString label() const override { return QObject::tr("Polygon"); }
    void build(Editor& ed, const std::vector<Snap>& clicks) override;
    bool snaps(size_t step) const override { return step == 0; }

private:
    core::build::PolygonKind m_kind;
};

class PolygonEdgeTool : public StepTool
{
public:
    using StepTool::StepTool;

protected:
    int steps() const override { return 3; }
    QString hintFor(size_t step) const override;
    QString label() const override { return QObject::tr("Polygon"); }
    void build(Editor& ed, const std::vector<Snap>& clicks) override;
    bool snaps(size_t step) const override { return step < 2; }
};

class SlotTool : public StepTool
{
public:
    enum class Kind
    {
        CenterToCenter,
        Overall,
        CenterPoint,
    };
    SlotTool(SketchView& view, Kind kind)
        : StepTool(view)
        , m_kind(kind)
    {}

protected:
    int steps() const override { return 3; }
    QString hintFor(size_t step) const override;
    QString label() const override { return QObject::tr("Slot"); }
    void build(Editor& ed, const std::vector<Snap>& clicks) override;
    bool snaps(size_t step) const override;

private:
    Kind m_kind;
};

class ArcSlotTool : public StepTool
{
public:
    ArcSlotTool(SketchView& view, bool threePoint)
        : StepTool(view)
        , m_threePoint(threePoint)
    {}

protected:
    int steps() const override { return 4; }
    QString hintFor(size_t step) const override;
    QString label() const override { return QObject::tr("Slot"); }
    void build(Editor& ed, const std::vector<Snap>& clicks) override;
    bool snaps(size_t step) const override { return step == 0 && !m_threePoint; }
    void tracked(const Snap& cursor) override;

private:
    bool m_threePoint;
    double m_sweep = 0.0;  // centre-point variant: signed sweep from the start, following the mouse
    double m_lastAngle = 0.0;
};

// Pick 2 or 3 curves, then click where the tangent circle should go.
class CircleTangentTool : public Tool
{
public:
    CircleTangentTool(SketchView& view, int count)
        : Tool(view)
        , m_count(count)
    {}

    QString hint() const override;
    void press(const ToolEvent& e) override;
    void move(const ToolEvent& e) override;
    bool cancel() override;
    void paintOverlay(QPainter& painter) override;

private:
    double radiusAt(core::Vec2 at) const;
    void apply(Editor& ed, core::Vec2 at) const;

    int m_count;
    std::vector<core::Id> m_picks;
    core::Vec2 m_mouse;
};

// Click the end of a line or arc, then where the tangent arc should end.
class ArcTangentTool : public Tool
{
public:
    using Tool::Tool;

    QString hint() const override;
    void press(const ToolEvent& e) override;
    void move(const ToolEvent& e) override;
    bool cancel() override;
    void paintOverlay(QPainter& painter) override;

private:
    core::Id m_curve = core::kNoId;
    core::Id m_start = core::kNoId;
    Snap m_cursor;
};
