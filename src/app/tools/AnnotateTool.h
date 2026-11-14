#pragma once

#include "Tool.h"

#include "../CommandPanel.h"

#include <core/Sketch.h>

#include <QPointer>

#include <optional>
#include <vector>

class QComboBox;
class QDoubleSpinBox;
class QLineEdit;
class QSpinBox;

// Annotate: dimensions and notes drawn on the drawing (see core::DrawnDimension). They measure the
// geometry, never drive it, and are shown and exported with the drawing, in Colour 1 (the stroke
// brush recolours them).
//
// Pick what to measure as with the Dimension tool (a line, a circle, two points, two lines...),
// then click where the label goes. The panel's kind says what's made: Auto (as the Dimension tool
// would; placing it decides horizontal, vertical or aligned), or one in particular: aligned,
// horizontal, vertical, radius, diameter, angle, arc length (an arc), ordinate x or y (a point,
// from the origin; Alt+click a point to measure from it instead), or a leader (click where it
// points: a point, a curve or anywhere; then where its note goes).
//
// The panel's text (<> for the value), decimals, text height and line width go on new ones, and
// on the one being edited: the last made, or one clicked (or double-clicked from Select). Its
// "drawing's decimals" are what dimensions show that don't set their own.
class AnnotateTool : public Tool
{
public:
    explicit AnnotateTool(SketchView& view);
    ~AnnotateTool() override;

    // What the panel's kind list offers, in order.
    enum class Kind
    {
        Auto,
        Aligned,
        Horizontal,
        Vertical,
        Radius,
        Diameter,
        Angle,
        ArcLength,
        OrdinateX,
        OrdinateY,
        Leader,
    };

    QString hint() const override;
    void activate() override;
    void press(const ToolEvent& e) override;
    void move(const ToolEvent& e) override;
    bool cancel() override;
    bool keyPress(int key, const QString& text) override;
    bool takeColor(const core::Color& color, bool second) override;
    void paintOverlay(QPainter& painter) override;

    // --- for tests and scripts ---
    CommandPanel* panel() const { return m_panel; }
    void setKind(Kind kind);
    core::Id current() const { return m_current; }
    void setCurrent(core::Id dimension);  // edited by the panel (kNoId: none)

private:
    // The dimension these picks would make with its label at `at` (none: nullopt; `incomplete`
    // set when more picks could still make one).
    std::optional<core::DrawnDimension> candidate(const std::vector<core::Id>& picks, core::Vec2 at,
                                                  bool* incomplete = nullptr) const;
    core::DrawnDimension fromPanel(core::DrawnDimension d) const;  // the panel's settings onto it
    Kind kind() const;
    void reset();
    void syncPanel();
    void applyPanel();

    std::vector<core::Id> m_picks;
    std::optional<core::Vec2> m_freeTip;  // a leader's, pointing at nothing in particular
    double m_along = 0.0;                 // a leader's, on a curve
    core::Id m_origin = core::kNoId;      // ordinates' (none: the sketch's)
    core::Vec2 m_mouse;
    core::Id m_current = core::kNoId;

    QPointer<CommandPanel> m_panel;
    QComboBox* m_kind = nullptr;
    QLineEdit* m_text = nullptr;
    QComboBox* m_decimals = nullptr;
    QDoubleSpinBox* m_height = nullptr;
    QDoubleSpinBox* m_width = nullptr;
    QSpinBox* m_drawingDecimals = nullptr;
    bool m_syncing = false;
};
