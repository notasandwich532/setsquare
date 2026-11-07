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
// With the geometry picked, the panel's last input takes what it paints: click areas / curves /
// texts to give them the gradient (they're listed in it; click one again to leave it out), and
// changes in the panel, geometry included, change them all. The first click on an item that
// already has a gradient picks it up instead, to edit. Shift+click gives an item the panel's
// gradient even if it has one; Ctrl+click takes an item's gradient off (its plain colour again).
// Strokes: double-click for the whole chain. Palette colours go to the selected stop. Enter
// closes the tool.
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
        Items,  // what it paints: areas, curves or texts
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
    bool keyPress(int key, const QString& text) override;
    bool takeColor(const core::Color& color, bool second) override;
    void paintOverlay(QPainter& painter) override;

    // --- for tests and scripts ---
    CommandPanel* panel() const { return m_panel; }
    GradientEditor* stopsEditor() const { return m_stops; }
    Target target() const { return m_target; }
    void setTarget(Target target);
    // The input that canvas clicks go to (-1: none).
    int currentInput() const { return m_input; }
    void setCurrentInput(int input);
    // What the panel's gradient is on (fills, curves or texts), its changes going to them all.
    const std::vector<core::Id>& items() const { return m_items; }
    core::Id current() const { return m_items.empty() ? core::kNoId : m_items.front(); }
    void setCurrent(core::Id item);  // just that item, its gradient picked up into the panel

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
    // Gives the items the gradient, as one edit (a new fill where an area has none yet; a curve
    // in a gradient outline, the whole outline). Returns the items as they are after (new fills).
    std::vector<core::Id> apply(const std::vector<Item>& items, const core::Gradient& g);
    void removeGradient(const Item& item);
    void commit();  // the panel's gradient onto the items, where it's changed
    void clickItem(const Item& item, Qt::KeyboardModifiers modifiers);
    void pickGeometry(int input, core::Id id);
    void nextInput();  // the first required input still empty (or none)
    std::vector<int> inputs() const;  // those of the gradient's kind
    void syncPanel();
    void syncStop();  // the selected stop's colour, place and opacity
    void onDocumentChanged();
    const core::regions::Arrangement& arrangement();

    Target m_target = Target::Fill;
    std::vector<core::Id> m_items;
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
