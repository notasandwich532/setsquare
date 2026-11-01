#pragma once

#include "DrawTools.h"
#include "Handles.h"
#include "Tool.h"

#include "../Editor.h"

#include <core/Sketch.h>

#include <QObject>
#include <QPointer>

#include <functional>
#include <vector>

class CommandPanel;
class QCheckBox;
class QComboBox;
class QDoubleSpinBox;
class QSpinBox;
class SelectionButton;

// A Fusion-style command: a panel of inputs over the canvas, a live preview, and handles to drag.
// Selection inputs (objects, a centre point, a mirror line, ...) take clicks on the canvas: the
// current one is highlighted, and picking fills it (a single-item input then moves on to the
// next empty one). OK or Enter makes the change and returns to Select; Cancel or Esc drops it.
class CommandTool : public Tool
{
public:
    explicit CommandTool(SketchView& view);
    ~CommandTool() override;

    QString hint() const override;
    void activate() override;
    void press(const ToolEvent& e) override;
    void move(const ToolEvent& e) override;
    void release(const ToolEvent& e) override;
    bool keyPress(int key, const QString& text) override;
    bool cancel() override;
    void paintOverlay(QPainter& painter) override;

    // Makes the change now (as OK does). Returns false, with a message, if it can't.
    bool accept();

protected:
    virtual QString title() const = 0;
    virtual void buildPanel() = 0;     // add inputs to panel(); pick inputs via addPick()
    virtual bool ready() const = 0;    // everything required is filled in
    virtual bool commit() = 0;         // make the change; false if it failed
    virtual void paintPreview(QPainter&) {}
    virtual std::vector<Handle> handles() const { return {}; }
    virtual void beginHandleDrag(int /*id*/) {}
    virtual void dragHandle(int /*id*/, core::Vec2 /*world*/, core::Vec2 /*pressedAt*/, Qt::KeyboardModifiers) {}
    virtual QString dragLabel(int /*id*/) const { return {}; }  // shown by a handle being dragged
    virtual void changed() {}          // inputs or picks changed
    // A click on nothing the current pick input accepts; return true to use it (e.g. a location).
    virtual bool pickLocation(int /*input*/, const Snap&) { return false; }
    virtual void abandon() {}          // the command is being dropped without committing

    CommandPanel* panel() const { return m_panel; }
    SelectionButton* pickButton(int input) const { return m_picks[static_cast<size_t>(input)].button; }
    core::Vec2 mouse() const { return m_mouse; }  // the latest cursor position (world)

    // A selection input. `accepts` says which items it takes; `multiple` toggles items in and
    // out, otherwise a pick replaces the item. Returns the input's index.
    int addPick(const QString& label, const char* name, const QString& prompt, std::function<bool(core::Id)> accepts,
                bool multiple);
    const std::vector<core::Id>& picked(int input) const { return m_picks[static_cast<size_t>(input)].ids; }
    void setPicked(int input, std::vector<core::Id> ids);
    void setCurrentPick(int input);
    void setPickEnabled(int input, bool enabled);

    // Geometry the selection inputs may take.
    bool isGeometry(core::Id id) const;
    bool isLine(core::Id id) const;
    bool isPoint(core::Id id) const;
    bool isCurve(core::Id id) const;

    // Centre of the bounding box of these items' points.
    core::Vec2 centreOf(const std::vector<core::Id>& items) const;
    double sizeOf(const std::vector<core::Id>& items) const;  // half the bounding box diagonal

    bool committed() const { return m_committed; }

private:
    struct Pick
    {
        SelectionButton* button = nullptr;
        QString prompt;
        std::function<bool(core::Id)> accepts;
        bool multiple = false;
        bool enabled = true;
        std::vector<core::Id> ids;
    };
    void picksChanged();
    void leave();  // back to Select, once the current event is done
    int handleAt(QPointF screen) const;

    QPointer<CommandPanel> m_panel;
    std::vector<Pick> m_picks;
    int m_current = -1;
    int m_hotHandle = -1;
    int m_dragHandle = -1;
    core::Vec2 m_pressedAt;
    core::Vec2 m_mouse;
    bool m_committed = false;
    bool m_leaving = false;
};

// Move/Copy: X and Y distances and an angle about the objects' centre, set in the panel or by
// dragging the arrows, the rotation knob or the centre square. Moving drags the real geometry,
// so what's attached follows; "Create copy" copies instead.
class MoveTool : public CommandTool
{
public:
    using CommandTool::CommandTool;
    ~MoveTool() override;

protected:
    QString title() const override { return QObject::tr("Move/Copy"); }
    void buildPanel() override;
    bool ready() const override;
    bool commit() override;
    void paintPreview(QPainter& painter) override;
    std::vector<Handle> handles() const override;
    void beginHandleDrag(int id) override;
    void dragHandle(int id, core::Vec2 world, core::Vec2 pressedAt, Qt::KeyboardModifiers modifiers) override;
    QString dragLabel(int id) const override;
    void changed() override;
    void abandon() override;

private:
    struct Motion
    {
        double dx = 0.0, dy = 0.0, angle = 0.0;
    };
    Motion motion() const;
    void stopLiveMove();

    int m_objects = 0;
    QDoubleSpinBox* m_dx = nullptr;
    QDoubleSpinBox* m_dy = nullptr;
    QDoubleSpinBox* m_angle = nullptr;
    QCheckBox* m_copy = nullptr;
    core::Vec2 m_pivot;             // the objects' centre when they were picked
    std::vector<core::Id> m_moving;  // objects of the live move, if one is open
    Motion m_dragStart;
};

// Scale: objects, the point to scale about (a point, or anywhere clicked), and the factor (or
// drag the arrow).
class ScaleTool : public CommandTool
{
public:
    using CommandTool::CommandTool;

protected:
    QString title() const override { return QObject::tr("Scale"); }
    void buildPanel() override;
    bool ready() const override;
    bool commit() override;
    void paintPreview(QPainter& painter) override;
    std::vector<Handle> handles() const override;
    void beginHandleDrag(int id) override;
    void dragHandle(int id, core::Vec2 world, core::Vec2 pressedAt, Qt::KeyboardModifiers modifiers) override;
    QString dragLabel(int id) const override;
    bool pickLocation(int input, const Snap& snap) override;
    void changed() override;

private:
    core::Vec2 about() const;
    double reach() const;  // arrow length at factor 1

    int m_objects = 0;
    int m_point = 0;
    bool m_hasLocation = false;
    core::Vec2 m_location;
    QDoubleSpinBox* m_factor = nullptr;
    double m_dragStart = 1.0;
};

// Mirror: objects and a mirror line (or axis).
class MirrorTool : public CommandTool
{
public:
    using CommandTool::CommandTool;

protected:
    QString title() const override { return QObject::tr("Mirror"); }
    void buildPanel() override;
    bool ready() const override;
    bool commit() override;
    void paintPreview(QPainter& painter) override;

private:
    int m_objects = 0;
    int m_line = 0;
};

// Rectangular pattern: objects, an optional direction line (else the X/Y axes), and per
// direction a quantity, a distance (the whole extent, or the spacing) and one-way/symmetric.
// Drag the arrows to set the distances; a negative distance goes the other way. Started on an
// existing pattern, it edits that pattern.
class RectangularPatternTool : public CommandTool
{
public:
    RectangularPatternTool(SketchView& view, core::Id editing = core::kNoId)
        : CommandTool(view)
        , m_editing(editing)
    {}

protected:
    QString title() const override { return QObject::tr("Rectangular Pattern"); }
    void buildPanel() override;
    bool ready() const override;
    bool commit() override;
    void paintPreview(QPainter& painter) override;
    std::vector<Handle> handles() const override;
    void beginHandleDrag(int id) override;
    void dragHandle(int id, core::Vec2 world, core::Vec2 pressedAt, Qt::KeyboardModifiers modifiers) override;
    QString dragLabel(int id) const override;
    void changed() override;

private:
    core::feature::LinearPatternParams params() const;
    double baseAngle() const;  // degrees: the first direction before any flip
    core::Vec2 axis(int direction) const;
    double extent(int direction) const;  // signed, along axis(direction)

    core::Id m_editing;
    int m_objects = 0;
    int m_direction = 0;
    QComboBox* m_distribution = nullptr;
    QSpinBox* m_count[2] = {};
    QDoubleSpinBox* m_distance[2] = {};
    QComboBox* m_symmetric[2] = {};
    double m_angle = 0.0;  // first direction (degrees) when no direction line is picked
    double m_dragStart = 0.0;  // extent when a handle drag began
};

// Circular pattern: objects, centre point, full turn or a given angle, and a quantity. Drag the
// knob to set the angle. Started on an existing pattern, it edits that pattern.
class CircularPatternTool : public CommandTool
{
public:
    CircularPatternTool(SketchView& view, core::Id editing = core::kNoId)
        : CommandTool(view)
        , m_editing(editing)
    {}

protected:
    QString title() const override { return QObject::tr("Circular Pattern"); }
    void buildPanel() override;
    bool ready() const override;
    bool commit() override;
    void paintPreview(QPainter& painter) override;
    std::vector<Handle> handles() const override;
    void dragHandle(int id, core::Vec2 world, core::Vec2 pressedAt, Qt::KeyboardModifiers modifiers) override;
    QString dragLabel(int id) const override;
    void changed() override;

private:
    core::feature::CircularPatternParams params() const;

    core::Id m_editing;
    int m_objects = 0;
    int m_center = 0;
    QComboBox* m_type = nullptr;
    QSpinBox* m_count = nullptr;
    QDoubleSpinBox* m_angle = nullptr;
};

// Offset: click a curve (its whole chain, unless "Chain selection" is off), then set the
// distance in the panel or with the arrow; a negative distance goes to the other side. "Both
// sides" offsets to each side at once, and "Cap ends" closes an open chain's ends with half
// circles (a slot round the chain).
class OffsetTool : public CommandTool
{
public:
    using CommandTool::CommandTool;

protected:
    QString title() const override { return QObject::tr("Offset"); }
    void buildPanel() override;
    bool ready() const override;
    bool commit() override;
    void paintPreview(QPainter& painter) override;
    std::vector<Handle> handles() const override;
    void beginHandleDrag(int id) override;
    void dragHandle(int id, core::Vec2 world, core::Vec2 pressedAt, Qt::KeyboardModifiers modifiers) override;
    QString dragLabel(int id) const override;
    void changed() override;

private:
    // Where the arrow starts (on the curve) and which way is positive.
    bool frame(core::Vec2& foot, core::Vec2& normal) const;
    core::Vec2 toward() const;

    bool bothSides() const;
    bool caps() const;

    int m_curves = 0;
    QDoubleSpinBox* m_distance = nullptr;
    QComboBox* m_sides = nullptr;  // one side / both sides
    QCheckBox* m_caps = nullptr;
    QCheckBox* m_chain = nullptr;
    core::Vec2 m_clickedAt;  // where the first curve was picked: its side is positive
    double m_dragStart = 0.0;
    std::vector<core::Id> m_lastPick;
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
