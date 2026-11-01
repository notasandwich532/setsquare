#pragma once

#include "TransformTools.h"

class FontPicker;
class QLineEdit;

// Text: click where it starts (anywhere, or on a point to anchor it there), type it, and pick a
// font (searchable), height, angle, bold/italic, how its outline is made (as designed, merged so
// nothing overlaps, or just the silhouette) and a minimum thickness for narrow parts. Started on
// an existing text, it edits that.
class TextTool : public CommandTool
{
public:
    TextTool(SketchView& view, core::Id editing = core::kNoId)
        : CommandTool(view)
        , m_editing(editing)
    {}

protected:
    QString title() const override { return QObject::tr("Text"); }
    void buildPanel() override;
    bool ready() const override;
    bool commit() override;
    void paintPreview(QPainter& painter) override;
    bool pickLocation(int input, const Snap& snap) override;
    void changed() override;

private:
    core::Text settings() const;  // from the panel (no anchor or shape)
    bool where(core::Vec2& at) const;

    core::Id m_editing;
    int m_position = 0;
    bool m_hasLocation = false;
    Snap m_location;
    QLineEdit* m_text = nullptr;
    FontPicker* m_font = nullptr;
    QDoubleSpinBox* m_height = nullptr;
    QDoubleSpinBox* m_angle = nullptr;
    QCheckBox* m_bold = nullptr;
    QCheckBox* m_italic = nullptr;
    QComboBox* m_outline = nullptr;
    QDoubleSpinBox* m_thickness = nullptr;

    // The letters for the current settings (made when they change, not on every repaint).
    std::vector<std::vector<core::Point>> m_shape;
    std::string m_shapeKey;
};
