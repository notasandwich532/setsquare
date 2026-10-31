#include "SketchView.h"

#include "Theme.h"

#include <QMouseEvent>
#include <QPainter>
#include <QWheelEvent>

#include <algorithm>
#include <cmath>

namespace
{
constexpr double kMinZoom = 1e-3;
constexpr double kMaxZoom = 1e5;
constexpr double kZoomStep = 1.15;            // per wheel notch
constexpr double kMinGridPixelSpacing = 12.0;  // minor grid lines closer than this are hidden
}  // namespace

SketchView::SketchView(QWidget* parent)
    : QWidget(parent)
{
    setMouseTracking(true);
    setFocusPolicy(Qt::StrongFocus);
    setAttribute(Qt::WA_OpaquePaintEvent);
    setMinimumSize(200, 150);
}

QPointF SketchView::worldToScreen(QPointF world) const
{
    return {m_pan.x() + world.x() * m_zoom, m_pan.y() - world.y() * m_zoom};
}

QPointF SketchView::screenToWorld(QPointF screen) const
{
    return {(screen.x() - m_pan.x()) / m_zoom, (m_pan.y() - screen.y()) / m_zoom};
}

void SketchView::resetView()
{
    m_zoom = 4.0;
    m_pan = QPointF(width() / 2.0, height() / 2.0);
    update();
}

void SketchView::resizeEvent(QResizeEvent* event)
{
    QWidget::resizeEvent(event);
    if (!m_initialised) {
        resetView();
        m_initialised = true;
    }
}

void SketchView::paintEvent(QPaintEvent*)
{
    QPainter painter(this);
    painter.fillRect(rect(), theme::canvas().background);
    drawGrid(painter);
}

void SketchView::drawGrid(QPainter& painter)
{
    const auto& colors = theme::canvas();

    // Pick a minor spacing from the 1-2-5 series so lines stay at least kMinGridPixelSpacing apart.
    double spacing = 1.0;
    const double minWorld = kMinGridPixelSpacing / m_zoom;
    const double decade = std::pow(10.0, std::floor(std::log10(minWorld)));
    for (double factor : {1.0, 2.0, 5.0, 10.0}) {
        spacing = decade * factor;
        if (spacing >= minWorld) {
            break;
        }
    }
    const double majorSpacing = spacing * 5.0;

    const QPointF topLeft = screenToWorld(QPointF(0, 0));
    const QPointF bottomRight = screenToWorld(QPointF(width(), height()));

    auto drawLines = [&](double step, const QColor& color) {
        painter.setPen(QPen(color, 0));
        for (double x = std::floor(topLeft.x() / step) * step; x <= bottomRight.x(); x += step) {
            const double sx = worldToScreen(QPointF(x, 0)).x();
            painter.drawLine(QPointF(sx, 0), QPointF(sx, height()));
        }
        for (double y = std::floor(bottomRight.y() / step) * step; y <= topLeft.y(); y += step) {
            const double sy = worldToScreen(QPointF(0, y)).y();
            painter.drawLine(QPointF(0, sy), QPointF(width(), sy));
        }
    };

    drawLines(spacing, colors.gridMinor);
    drawLines(majorSpacing, colors.gridMajor);

    // Axes through the origin.
    const QPointF origin = worldToScreen(QPointF(0, 0));
    painter.setPen(QPen(colors.axisX, 1.0));
    painter.drawLine(QPointF(0, origin.y()), QPointF(width(), origin.y()));
    painter.setPen(QPen(colors.axisY, 1.0));
    painter.drawLine(QPointF(origin.x(), 0), QPointF(origin.x(), height()));
}

void SketchView::wheelEvent(QWheelEvent* event)
{
    const double notches = event->angleDelta().y() / 120.0;
    if (notches == 0.0) {
        return;
    }

    // Zoom about the cursor: keep the world point under the mouse fixed on screen.
    const QPointF mouse = event->position();
    const QPointF anchor = screenToWorld(mouse);
    m_zoom = std::clamp(m_zoom * std::pow(kZoomStep, notches), kMinZoom, kMaxZoom);
    m_pan = QPointF(mouse.x() - anchor.x() * m_zoom, mouse.y() + anchor.y() * m_zoom);

    update();
    event->accept();
}

void SketchView::mousePressEvent(QMouseEvent* event)
{
    if (event->button() == Qt::MiddleButton) {
        m_panning = true;
        m_lastMouse = event->position();
        setCursor(Qt::ClosedHandCursor);
        event->accept();
        return;
    }
    QWidget::mousePressEvent(event);
}

void SketchView::mouseMoveEvent(QMouseEvent* event)
{
    if (m_panning) {
        m_pan += event->position() - m_lastMouse;
        m_lastMouse = event->position();
        update();
    }
    emit cursorMoved(screenToWorld(event->position()));
}

void SketchView::mouseReleaseEvent(QMouseEvent* event)
{
    if (event->button() == Qt::MiddleButton && m_panning) {
        m_panning = false;
        unsetCursor();
        event->accept();
        return;
    }
    QWidget::mouseReleaseEvent(event);
}
