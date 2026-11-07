#pragma once

#include <core/Sketch.h>

#include <QDialog>

#include <optional>

class QCheckBox;
class QComboBox;
class QDoubleSpinBox;
class QToolButton;

// File > Page Setup: no page, or a standard size (A0..A5, Letter, Legal, Tabloid) or a custom one,
// portrait or landscape; where the sketch origin is on it; and the paper's colour (optional).
class PageSetupDialog : public QDialog
{
    Q_OBJECT

public:
    PageSetupDialog(const std::optional<core::Page>& page, QWidget* parent = nullptr);

    std::optional<core::Page> page() const;  // as set up (none: no page)

private:
    void update();  // inputs enabled and filled in to match the size chosen
    void setColor(const core::Color& color);

    QComboBox* m_size = nullptr;
    QComboBox* m_orientation = nullptr;
    QDoubleSpinBox* m_width = nullptr;
    QDoubleSpinBox* m_height = nullptr;
    QComboBox* m_origin = nullptr;
    QCheckBox* m_hasColor = nullptr;
    QToolButton* m_colorButton = nullptr;
    core::Color m_color{0xff, 0xff, 0xff, 255};
    bool m_updating = false;
};
