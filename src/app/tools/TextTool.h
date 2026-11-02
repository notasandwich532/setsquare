#pragma once

#include "TransformTools.h"

class FontPicker;
class QPlainTextEdit;

// Text: in a box (click two corners, as for a rectangle: a construction rectangle the text is
// aligned in, which can be dimensioned, and which it follows), or along a curve (a line, arc or
// circle), following it as it moves. (Text placed at a point, from scripts or older drawings, can
// be edited here too.) Type it (Enter starts a new line; Ctrl+Enter or OK places it), pick a font
// (searchable), height, bold/italic, character spacing, where it goes in the box (or on the
// curve), mirroring, how its outline is made (as designed, merged so nothing overlaps, or just the
// silhouette) and a minimum thickness for narrow parts. Started on an existing text, it edits that.
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
    enum class Kind
    {
        Box,
        Path,
        Point,  // (editing an older point-anchored text)
    };
    Kind kind() const;
    bool alongPath() const { return kind() == Kind::Path; }
    // The box's corners as picked so far (the second following the cursor until it's placed).
    bool corners(core::Vec2& a, core::Vec2& b) const;
    core::Text settings() const;  // from the panel (anchor and path too, once they're known)
    bool where(core::Vec2& at) const;

    core::Id m_editing;
    int m_position = 0;
    int m_corner = 0;
    int m_opposite = 0;
    int m_path = 0;
    bool m_hasLocation = false;
    Snap m_location;
    bool m_hasCorner[2] = {false, false};
    Snap m_cornerAt[2];
    QComboBox* m_type = nullptr;
    QPlainTextEdit* m_text = nullptr;
    FontPicker* m_font = nullptr;
    QDoubleSpinBox* m_height = nullptr;
    QDoubleSpinBox* m_angle = nullptr;
    QCheckBox* m_bold = nullptr;
    QCheckBox* m_italic = nullptr;
    QDoubleSpinBox* m_spacing = nullptr;
    QComboBox* m_align = nullptr;
    QComboBox* m_valign = nullptr;
    QCheckBox* m_flipH = nullptr;
    QCheckBox* m_flipV = nullptr;
    QComboBox* m_outline = nullptr;
    QDoubleSpinBox* m_thickness = nullptr;

    // The letters for the current settings (made when they change, not on every repaint).
    std::vector<std::vector<core::Point>> m_shape;
    std::string m_shapeKey;
};
