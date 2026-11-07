#pragma once

#include "Tool.h"

#include "../Images.h"

#include <core/Regions.h>
#include <core/Sketch.h>

#include <QString>

#include <functional>
#include <memory>
#include <optional>

// Stroke brush: click a curve to give it the current stroke (colour 1, width and line style from
// the colour bar); double-click for its whole chain; drag a box to do every curve in it (left to
// right: those inside; right to left: those touched). Ctrl takes strokes off instead (click or
// box); Alt+click picks a curve's stroke up into the colour bar (an eyedropper). Construction
// geometry can't be decorated.
class StrokeBrushTool : public Tool
{
public:
    using Tool::Tool;

    QString hint() const override;
    void press(const ToolEvent& e) override;
    void move(const ToolEvent& e) override;
    void release(const ToolEvent& e) override;
    void doubleClick(const ToolEvent& e) override;
    bool cancel() override;
    void paintOverlay(QPainter& painter) override;

private:
    core::Id curveAt(QPointF screen) const;
    void clickAt(QPointF screen, Qt::KeyboardModifiers modifiers);
    void applyBox(bool clear);
    void apply(const std::vector<core::Id>& curves, bool clear);

    core::Id m_hover = core::kNoId;
    bool m_pressed = false;
    bool m_boxing = false;
    QPointF m_pressScreen;
    QPointF m_boxEnd;
    Qt::KeyboardModifiers m_pressModifiers;
};

// Fill (a paint bucket): click inside a closed area to fill it with Colour 2 (clicking a filled
// area recolours it). Ctrl+click removes the fill there; Alt+click picks its colour up. Hover to
// see the area first. Where nothing's enclosed, it marks the loose ends: an outline left open.
class FillTool : public Tool
{
public:
    using Tool::Tool;

    QString hint() const override;
    void press(const ToolEvent& e) override;
    void move(const ToolEvent& e) override;
    void paintOverlay(QPainter& painter) override;

private:
    const core::regions::Arrangement& arrangement();  // of the sketch as it is now
    core::Id fillAt(core::Vec2 p);                     // the topmost fill covering p

    std::unique_ptr<core::regions::Arrangement> m_arrangement;
    quint64 m_builtFor = ~quint64(0);  // document revision the arrangement is of
    core::Id m_builtLayer = core::kNoId;  // and the layer
    std::optional<core::regions::Region> m_hover;
    std::vector<core::Vec2> m_looseEnds;  // shown after a click that found no area
};

// Reset fill: click a filled area to take its fill off (colour or gradient), leaving the area
// empty again. Hover to see which fill a click will take.
class ResetFillTool : public Tool
{
public:
    using Tool::Tool;

    QString hint() const override;
    void press(const ToolEvent& e) override;
    void move(const ToolEvent& e) override;
    void paintOverlay(QPainter& painter) override;

private:
    core::Id fillAt(core::Vec2 p);  // the topmost fill of the active layer covering p

    std::unique_ptr<core::regions::Arrangement> m_arrangement;
    quint64 m_builtFor = ~quint64(0);
    core::Id m_builtLayer = core::kNoId;
    core::Id m_hover = core::kNoId;
};

// Eyedropper: click anything to take its colour, then choose whether it becomes Colour 1
// (strokes) or Colour 2 (fills). It takes the colour things really are, not as the canvas shows
// them faded while sketching: a stroke's, a text's, a fill's (a gradient's where you click), or a
// reference image's own pixel. Hover to see it first. Afterwards it goes back to the tool before.
class EyedropperTool : public Tool
{
public:
    using Tool::Tool;

    struct Sample
    {
        core::Color color;
        QString what;  // e.g. "fill", "stroke", "image"
    };
    // The colour of what's at a place on the canvas, if anything there has one.
    std::optional<Sample> sample(QPointF screen, core::Vec2 world);
    // Makes it Colour 1 (or 2), and goes back to the tool before.
    void use(const core::Color& color, bool second);

    // How the choice is asked (tests replace it): Colour 1 (false), 2 (true), or none (cancelled).
    using Chooser = std::function<std::optional<bool>(const Sample& sample, QPoint globalPos)>;
    static Chooser& chooser();

    QString hint() const override;
    void press(const ToolEvent& e) override;
    void move(const ToolEvent& e) override;
    void paintOverlay(QPainter& painter) override;

private:
    std::optional<Sample> m_hover;
    QPointF m_hoverScreen;
    images::Cache m_pictures;  // decoded reference images, to read their pixels
};

// The topmost fill of the active layer covering `p` in an arrangement of it (kNoId if none).
core::Id topFillAt(const core::Sketch& sketch, const core::regions::Arrangement& arrangement, core::Vec2 p);
