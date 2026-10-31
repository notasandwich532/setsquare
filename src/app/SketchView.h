#pragma once

#include <QPointF>
#include <QWidget>

class QPainter;

// The sketch canvas. World coordinates are in millimetres with +Y up; the view keeps a
// world->screen transform made of a pan offset and a uniform zoom factor.
class SketchView : public QWidget
{
    Q_OBJECT

public:
    explicit SketchView(QWidget* parent = nullptr);

    QPointF worldToScreen(QPointF world) const;
    QPointF screenToWorld(QPointF screen) const;

    // Pixels per world unit.
    double zoom() const { return m_zoom; }

    // Centres the view on the origin at the default zoom.
    void resetView();

signals:
    void cursorMoved(QPointF world);

protected:
    void paintEvent(QPaintEvent* event) override;
    void wheelEvent(QWheelEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;
    void resizeEvent(QResizeEvent* event) override;

private:
    void drawGrid(QPainter& painter);

    QPointF m_pan;          // screen position of the world origin
    double m_zoom = 4.0;    // pixels per world unit
    bool m_panning = false;
    QPointF m_lastMouse;
    bool m_initialised = false;
};
