#pragma once

#include "Tool.h"

#include "../CommandPanel.h"

#include <core/Sketch.h>

#include <QPointer>

class QCheckBox;
class QDoubleSpinBox;
class QLabel;
class QSpinBox;
class QToolButton;

// Image: places a reference image (core::Image), in its own panel over the canvas: its opacity,
// size, angle, position and flips. Drag the image to move it; click another to edit that one.
// Calibrate, as in Fusion: click two points on the image, then type the real distance between
// them, and the image is scaled (about the first point) so they're that far apart. Enter: done
// (or, while calibrating, apply the distance); Esc: stop calibrating, else done.
class ImageTool : public Tool
{
public:
    explicit ImageTool(SketchView& view);
    ~ImageTool() override;

    QString hint() const override;
    void activate() override;
    void press(const ToolEvent& e) override;
    void move(const ToolEvent& e) override;
    void release(const ToolEvent& e) override;
    bool cancel() override;
    bool keyPress(int key, const QString& text) override;
    void paintOverlay(QPainter& painter) override;

    // --- for tests and scripts ---
    CommandPanel* panel() const { return m_panel; }
    core::Id image() const { return m_image; }
    void setImage(core::Id image);
    // Calibrating: 0 not, 1 waiting for the first point, 2 the second, 3 the distance.
    int calibrationStep() const { return m_step; }
    void startCalibration();
    // Scales the image (about `a`) so that `a` and `b` are `distance` apart.
    void calibrate(core::Vec2 a, core::Vec2 b, double distance);

private:
    void syncPanel();
    void edit(const std::function<void(core::Image&)>& change);  // one undoable change
    void onDocumentChanged();
    void applyCalibration();

    core::Id m_image = core::kNoId;
    bool m_dragging = false;
    core::Vec2 m_pressWorld;
    core::Image m_dragStart;

    int m_step = 0;
    core::Vec2 m_calA, m_calB, m_hoverWorld;

    QPointer<CommandPanel> m_panel;
    QLabel* m_file = nullptr;
    QSpinBox* m_opacity = nullptr;
    QDoubleSpinBox* m_width = nullptr;
    QDoubleSpinBox* m_height = nullptr;
    QDoubleSpinBox* m_angle = nullptr;
    QDoubleSpinBox* m_x = nullptr;
    QDoubleSpinBox* m_y = nullptr;
    QCheckBox* m_flipH = nullptr;
    QCheckBox* m_flipV = nullptr;
    QToolButton* m_calibrateButton = nullptr;
    QDoubleSpinBox* m_distance = nullptr;
    bool m_syncing = false;
};
