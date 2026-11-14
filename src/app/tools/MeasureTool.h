#pragma once

#include "Tool.h"

#include "../CommandPanel.h"

#include <core/Regions.h>
#include <core/Sketch.h>

#include <QPointer>

#include <memory>
#include <optional>
#include <vector>

class QLabel;

// Measure: reads sizes off the drawing, changing nothing. Click a curve for its length
// (Shift+click more to add them up), a point and then another for the distance between them and
// the differences in x and y, or inside a closed area for its area (its holes taken off). The
// results are in the tool's panel (selectable, to copy) and marked on the canvas, and follow the
// drawing as it changes. Started with curves or two points selected, it measures them.
class MeasureTool : public Tool
{
public:
    explicit MeasureTool(SketchView& view);
    ~MeasureTool() override;

    enum class Mode
    {
        None,
        Curves,  // their total length
        Points,  // the distance from the first to the second (just the first: waiting for another)
        Area,
    };

    QString hint() const override;
    void activate() override;
    void press(const ToolEvent& e) override;
    void move(const ToolEvent& e) override;
    bool cancel() override;
    void beforePaint() override;
    void paintOverlay(QPainter& painter) override;

    // --- for tests and scripts ---
    CommandPanel* panel() const { return m_panel; }
    Mode mode() const { return m_mode; }
    // The panel's values, as shown ("" for rows not shown): Length, Distance, ΔX, ΔY, Area.
    QString shown(const char* row) const;

private:
    void clear();
    void measureCurve(core::Id curve, bool add);
    void measurePoint(core::Id point);
    void measureArea(core::Vec2 at);
    void forgetMissing();  // after an edit: what's no longer there isn't measured
    void updatePanel();
    const core::regions::Arrangement& hoverArrangement();

    Mode m_mode = Mode::None;
    std::vector<core::Id> m_curves;
    core::Id m_a = core::kNoId, m_b = core::kNoId;
    core::Vec2 m_areaAt;
    std::optional<core::regions::Region> m_region;  // (finely followed, for its size)
    quint64 m_regionFor = ~quint64(0);              // the document revision it's of

    core::Id m_hover = core::kNoId;  // the point or curve a click would measure
    std::optional<core::regions::Region> m_hoverRegion;
    std::unique_ptr<core::regions::Arrangement> m_arrangement;  // for hovering (coarse, quick)
    quint64 m_arrangementFor = ~quint64(0);
    core::Id m_arrangementLayer = core::kNoId;

    QPointer<CommandPanel> m_panel;
    QLabel* m_prompt = nullptr;
    QLabel* m_length = nullptr;
    QLabel* m_distance = nullptr;
    QLabel* m_dx = nullptr;
    QLabel* m_dy = nullptr;
    QLabel* m_area = nullptr;
};
