#pragma once

#include <core/Style.h>

#include <QWidget>

// A gradient's colours as a bar, with a marker under it for each stop. Click the bar to add a
// stop (in the colour already there); drag a marker along to move it, or down off the bar to
// remove it (as does right-click, or Delete with it selected); double-click a marker to pick its
// colour. A gradient keeps at least two stops.
class GradientEditor : public QWidget
{
    Q_OBJECT

public:
    explicit GradientEditor(QWidget* parent = nullptr);

    void setGradient(const core::Gradient& gradient);  // shows it (keeps the selected stop if it can)
    const core::Gradient& gradient() const { return m_gradient; }

    int selectedStop() const { return m_selected; }
    void setSelectedStop(int index);

    // As the mouse would (for tests and scripts). Each emits edited().
    int addStop(double offset);  // in the colour there; returns its index (selected)
    void moveStop(int index, double offset);
    bool removeStop(int index);  // false if it's one of the last two
    void setStopColor(int index, const core::Color& color);

    QSize sizeHint() const override { return {220, 42}; }

signals:
    void editing();             // changing (a marker being dragged): show it, don't keep it yet
    void edited();              // a change to keep
    void selectionChanged(int index);
    void colorRequested(int index);  // double-click on a marker

protected:
    void paintEvent(QPaintEvent*) override;
    void mousePressEvent(QMouseEvent* e) override;
    void mouseMoveEvent(QMouseEvent* e) override;
    void mouseReleaseEvent(QMouseEvent* e) override;
    void mouseDoubleClickEvent(QMouseEvent* e) override;
    void keyPressEvent(QKeyEvent* e) override;

private:
    QRectF barRect() const;
    double offsetAt(double x) const;
    double xAt(double offset) const;
    int markerAt(QPointF pos) const;
    int moveTo(int index, double offset);  // keeps the stops in order; returns its new index

    core::Gradient m_gradient;
    int m_selected = -1;
    int m_dragging = -1;
    bool m_dragOff = false;  // dragged down off the bar: removed on release
};
