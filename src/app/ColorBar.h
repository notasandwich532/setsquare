#pragma once

#include <core/Style.h>

#include <QWidget>

#include <vector>

class QComboBox;
class SketchView;
class Swatch;

// The colour and line-style strip, after MS Paint: Colour 1 (strokes) and Colour 2 (fills) side
// by side, a palette (left-click sets Colour 1, or the selected colour; right-click Colour 2), a
// row of custom colours, "Edit colours", and the line width and style. Clicking Colour 1 or 2
// selects it for the next colour (palette, Edit colours, eyedropper); any other click puts it back
// to neither. Double-click either to edit it. Changes apply to the selected curves straight away;
// selecting a decorated curve shows its style here. "Reset" takes strokes off.
class ColorBar : public QWidget
{
    Q_OBJECT

public:
    explicit ColorBar(SketchView& view, QWidget* parent = nullptr);

    enum Part
    {
        ColorPart = 1,
        WidthPart = 2,
        DashPart = 4,
    };

    // As a click on the palette would (for tests and scripts): sets Colour 1 or 2.
    void pickColor(const core::Color& color, bool second = false);
    void setWidth(double width);   // 0 = hairline, StrokeStyle::kNone = no line
    void setDash(core::Dash dash);
    void editColor(bool second);   // opens the colour editor
    void resetSelection();         // takes the strokes off the selected curves

protected:
    bool eventFilter(QObject* watched, QEvent* event) override;

private:
    bool secondFor(Qt::MouseButton button) const;  // a palette click: to Colour 2?
    void syncFromView();             // show the view's paint
    void syncFromSelection();        // a selected curve's stroke becomes the paint
    void applyToSelection(int parts);  // the paint's `parts` onto the selected curves
    void addCustom(const core::Color& color);

    SketchView& m_view;
    class ColorBox* m_box1 = nullptr;
    class ColorBox* m_box2 = nullptr;
    class QToolButton* m_edit = nullptr;
    std::vector<Swatch*> m_custom;
    std::vector<core::Color> m_customColors;
    QComboBox* m_width = nullptr;
    QComboBox* m_dash = nullptr;
    bool m_syncing = false;
};
