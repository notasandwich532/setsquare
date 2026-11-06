#pragma once

#include "Tool.h"

#include "../CommandPanel.h"

#include <core/Gradient.h>
#include <core/Regions.h>
#include <core/Sketch.h>

#include <QPointer>

#include <array>
#include <memory>
#include <optional>

class GradientEditor;
class QComboBox;
class QDoubleSpinBox;
class QSpinBox;
class QToolButton;
class SelectionButton;

// Gradient: paints fills, strokes or text with a gradient, set up in the tool's own panel (over
// the canvas): what it paints, linear or radial, the sketch geometry that places it, its colours,
// and what happens past its ends. The geometry is picked on the canvas, so the gradient follows
// it: linear, a start point and an end point; radial, a circle, arc, ellipse or elliptical arc
// (its centre and size; an ellipse makes it elliptical) and optionally a focal point.
//
// With the geometry picked, click an area / curve / text to give it the gradient; one that
// already has a gradient is picked instead (the panel shows its gradient, and changes there,
// geometry included, change it). Shift+click gives an item the panel's gradient even if it has
// one; Ctrl+click takes an item's gradient off (its plain colour again). Strokes: double-click
// for the whole chain. Palette colours go to the selected stop.
class GradientTool : public Tool
{
public:
    enum class Target
    {
        Fill,
        Stroke,
        Text,
    };
    // The geometry inputs.
    enum Input
    {
        Start,
        End,
        Shape,
        Focal,
        kInputs,
    };

    explicit GradientTool(SketchView& view);
    ~GradientTool() override;

    QString hint() const override;
    void activate() override;
    void press(const ToolEvent& e) override;
    void move(const ToolEvent& e) override;
    void doubleClick(const ToolEvent& e) override;
    bool cancel() override;
    bool takeColor(const core::Color& color, bool second) override;
    void paintOverlay(QPainter& painter) override;

    // --- for tests and scripts ---
    CommandPanel* panel() const { return m_panel; }
    GradientEditor* stopsEditor() const { return m_stops; }
    Target target() const { return m_target; }
    void setTarget(Target target);
    // The geometry input that canvas clicks go to (-1: none, clicks paint).
    int currentInput() const { return m_input; }
    void setCurrentInput(int input);
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
    core::Id geometryAt(core::Vec2 world, int input) const;  // what the input can take there
    bool hasGradient(core::Id item) const;
    const core::Gradient* gradientOf(core::Id item) const;
    // Gives an item the gradient (making a fill where there's none yet); it becomes current.
    void apply(const Item& item, const core::Gradient& g);
    void applyStroke(const std::vector<core::Id>& curves, const core::Gradient& g);
    void removeGradient(const Item& item);
    void commit();  // the panel's gradient onto the current item, if it's changed
    void pickGeometry(int input, core::Id id);
    void nextInput();  // the first required input still empty (or none)
    std::vector<int> inputs() const;  // those of the gradient's kind
    void syncPanel();
    void syncStop();  // the selected stop's colour, place and opacity
    void onDocumentChanged();
    const core::regions::Arrangement& arrangement();

    Target m_target = Target::Fill;
    core::Id m_current = core::kNoId;
    int m_input = -1;
    Item m_hover;
    core::Id m_hoverGeometry = core::kNoId;

    std::unique_ptr<core::regions::Arrangement> m_arrangement;
    quint64 m_builtFor = ~quint64(0);
    core::Id m_builtLayer = core::kNoId;

    QPointer<CommandPanel> m_panel;
    GradientEditor* m_stops = nullptr;
    QComboBox* m_targetBox = nullptr;
    QComboBox* m_kind = nullptr;
    QComboBox* m_spread = nullptr;
    std::array<SelectionButton*, kInputs> m_inputs{};
    QToolButton* m_stopColor = nullptr;
    QDoubleSpinBox* m_stopPosition = nullptr;
    QSpinBox* m_stopOpacity = nullptr;
    bool m_syncing = false;
};
