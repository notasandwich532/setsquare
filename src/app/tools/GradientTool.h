#pragma once

#include "Tool.h"

#include <core/Gradient.h>
#include <core/Regions.h>
#include <core/Sketch.h>

#include <QPointer>

#include <memory>
#include <optional>

class CommandPanel;
class GradientEditor;
class QComboBox;
class QDoubleSpinBox;
class QSpinBox;
class QToolButton;

// Gradient: paints fills, strokes or text with a gradient, set up in the tool's own panel (over
// the canvas: what it paints, linear or radial, its colours, and what happens past its ends).
// Click an area / curve / text to give it the gradient; one that already has a gradient is
// picked instead (the panel shows its gradient, and changes there change it). Drag across an
// item to set where the gradient runs (radial: from its centre out); drag the handles of the
// picked item to adjust it (radial: its centre, edge, focal point and width across).
// Shift+click gives an item the panel's gradient even if it has one; Ctrl+click takes an item's
// gradient off (its plain colour again). Strokes: double-click for the whole chain. Palette
// colours go to the selected stop.
class GradientTool : public Tool
{
public:
    enum class Target
    {
        Fill,
        Stroke,
        Text,
    };

    explicit GradientTool(SketchView& view);
    ~GradientTool() override;

    QString hint() const override;
    void activate() override;
    void press(const ToolEvent& e) override;
    void move(const ToolEvent& e) override;
    void release(const ToolEvent& e) override;
    void doubleClick(const ToolEvent& e) override;
    bool cancel() override;
    bool takeColor(const core::Color& color, bool second) override;
    void paintOverlay(QPainter& painter) override;

    // --- for tests and scripts ---
    CommandPanel* panel() const { return m_panel; }
    GradientEditor* stopsEditor() const { return m_stops; }
    Target target() const { return m_target; }
    void setTarget(Target target);
    // The item whose gradient the panel is editing (a fill, a curve or a text), if any.
    core::Id current() const { return m_current; }
    void setCurrent(core::Id item);  // picks it (its gradient into the panel)

private:
    struct Item
    {
        core::Id id = core::kNoId;  // a fill (or an area to fill: none yet), a curve, a text
        std::optional<core::regions::Region> region;  // fills: the area
        core::Vec2 at;                                 // fills: where (to make a new one)
    };
    Item itemAt(QPointF screen, core::Vec2 world);
    bool hasGradient(core::Id item) const;
    const core::Gradient* gradientOf(core::Id item) const;
    std::optional<core::gradient::Frame> frameOf(const Item& item) const;
    std::optional<core::gradient::Frame> currentFrame() const;
    // Gives an item the gradient (making a fill where there's none yet); it becomes current.
    void apply(const Item& item, const core::Gradient& g);
    void applyStroke(const std::vector<core::Id>& curves, const core::Gradient& g);
    void removeGradient(const Item& item);
    void commit();  // the panel's gradient onto the current item, if it's changed
    void syncPanel();
    void syncStop();  // the selected stop's colour, place and opacity
    void onDocumentChanged();
    const core::regions::Arrangement& arrangement();

    // Handles of the current item's gradient, in drawing coordinates: start/centre, end/edge,
    // and (radial) focal point and the end of the second axis.
    std::vector<core::Vec2> handles() const;
    int handleAt(QPointF screen) const;
    void dragHandle(int handle, core::Vec2 world);

    Target m_target = Target::Fill;
    core::Id m_current = core::kNoId;
    Item m_hover;

    enum class Drag
    {
        None,
        Pending,    // pressed on an item: a click, or the start of a drag across it
        Direction,  // dragging out where the gradient runs
        Handle,
    } m_drag = Drag::None;
    Item m_dragItem;
    int m_handle = -1;
    QPointF m_pressScreen;
    core::Vec2 m_pressWorld;
    Qt::KeyboardModifiers m_pressModifiers;
    std::optional<core::gradient::Frame> m_dragFrame;

    std::unique_ptr<core::regions::Arrangement> m_arrangement;
    quint64 m_builtFor = ~quint64(0);
    core::Id m_builtLayer = core::kNoId;

    QPointer<CommandPanel> m_panel;
    GradientEditor* m_stops = nullptr;
    QComboBox* m_targetBox = nullptr;
    QComboBox* m_kind = nullptr;
    QComboBox* m_spread = nullptr;
    QToolButton* m_stopColor = nullptr;
    QDoubleSpinBox* m_stopPosition = nullptr;
    QSpinBox* m_stopOpacity = nullptr;
    QDoubleSpinBox* m_aspect = nullptr;
    bool m_syncing = false;
};
