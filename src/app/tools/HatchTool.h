#pragma once

#include "Tool.h"

#include "../CommandPanel.h"

#include <core/Regions.h>
#include <core/Sketch.h>

#include <QPointer>

#include <memory>
#include <optional>
#include <vector>

class QComboBox;
class QDoubleSpinBox;

// Hatch: draws areas with lines in a pattern instead of filling them solid, set up in the tool's
// own panel: the pattern (a built-in one, or one an imported file brought), its angle and scale,
// and its lines' width. New hatches are in Colour 2; palette colours recolour the listed areas.
//
// Click areas to hatch them (they're listed in the panel; click one again to leave it out), and
// changes in the panel change them all. The first click on an area that's already hatched picks
// its hatch up into the panel instead, to edit; Shift+click hatches it with the panel's anyway;
// Ctrl+click takes its hatch off (filled solid again). Enter closes the tool.
class HatchTool : public Tool
{
public:
    explicit HatchTool(SketchView& view);
    ~HatchTool() override;

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
    const std::vector<core::Id>& items() const { return m_items; }  // the listed fills
    void setCurrent(core::Id fill);  // just that one, its hatch picked up into the panel
    // The panel's hatch (and SketchView::hatch), onto the listed areas.
    void setHatch(const core::Hatch& hatch);

private:
    struct Area
    {
        core::Id fill = core::kNoId;  // none yet: an area to fill
        std::optional<core::regions::Region> region;
        core::Vec2 at;
    };
    Area areaAt(core::Vec2 world);
    bool hatched(core::Id fill) const;
    // Hatches the areas, as one edit (in their own colour, a new one's Colour 2, unless `color`).
    // Returns their fills, as they are after.
    std::vector<core::Id> apply(const std::vector<Area>& areas, const core::Hatch& hatch,
                                std::optional<core::Color> color = std::nullopt);
    void click(const Area& area, Qt::KeyboardModifiers modifiers);
    void syncPanel();
    void onDocumentChanged();
    const core::regions::Arrangement& arrangement();

    std::vector<core::Id> m_items;
    Area m_hover;

    std::unique_ptr<core::regions::Arrangement> m_arrangement;
    quint64 m_builtFor = ~quint64(0);
    core::Id m_builtLayer = core::kNoId;

    QPointer<CommandPanel> m_panel;
    QComboBox* m_pattern = nullptr;
    QDoubleSpinBox* m_angle = nullptr;
    QDoubleSpinBox* m_scale = nullptr;
    QComboBox* m_width = nullptr;
    SelectionButton* m_list = nullptr;
    bool m_syncing = false;
};
