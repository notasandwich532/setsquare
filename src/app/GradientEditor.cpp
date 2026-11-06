#include "GradientEditor.h"

#include <QKeyEvent>
#include <QLinearGradient>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>

#include <algorithm>
#include <cmath>
#include <utility>

namespace
{
constexpr double kSide = 7.0;       // room either side of the bar for the end markers
constexpr double kBarHeight = 20.0;
constexpr double kMarker = 6.0;     // half a marker's width
constexpr double kRemoveDrop = 28.0;  // dragged this far below the markers: removed

QColor toQt(const core::Color& c)
{
    return {c.r, c.g, c.b, c.a};
}

// Grey squares behind the colours, so see-through ones show as such.
void checkerboard(QPainter& p, const QRectF& r)
{
    p.save();
    p.setClipRect(r);
    p.fillRect(r, QColor(0x9a, 0x9a, 0x9a));
    constexpr int kSquare = 5;
    for (int y = 0; y * kSquare < r.height(); ++y) {
        for (int x = (y % 2); x * kSquare < r.width(); x += 2) {
            p.fillRect(QRectF(r.left() + x * kSquare, r.top() + y * kSquare, kSquare, kSquare), QColor(0x6a, 0x6a, 0x6a));
        }
    }
    p.restore();
}
}  // namespace

GradientEditor::GradientEditor(QWidget* parent)
    : QWidget(parent)
{
    setObjectName(QStringLiteral("gradientStops"));
    setMinimumSize(200, 42);
    setFocusPolicy(Qt::ClickFocus);
    setMouseTracking(true);
    setToolTip(tr("Click the bar to add a colour; drag a marker to move it (down off the bar to remove it); "
                  "double-click a marker to change its colour"));
}

void GradientEditor::setGradient(const core::Gradient& gradient)
{
    m_gradient = gradient;
    if (m_selected >= static_cast<int>(m_gradient.stops.size())) {
        m_selected = m_gradient.stops.empty() ? -1 : 0;
        emit selectionChanged(m_selected);
    }
    else if (m_selected < 0 && !m_gradient.stops.empty()) {
        m_selected = 0;
        emit selectionChanged(m_selected);
    }
    update();
}

void GradientEditor::setSelectedStop(int index)
{
    if (index < -1 || index >= static_cast<int>(m_gradient.stops.size()) || index == m_selected) {
        return;
    }
    m_selected = index;
    update();
    emit selectionChanged(index);
}

QRectF GradientEditor::barRect() const
{
    return {kSide, 2.0, width() - 2 * kSide, kBarHeight};
}

double GradientEditor::offsetAt(double x) const
{
    const QRectF bar = barRect();
    return std::clamp((x - bar.left()) / std::max(bar.width(), 1.0), 0.0, 1.0);
}

double GradientEditor::xAt(double offset) const
{
    const QRectF bar = barRect();
    return bar.left() + offset * bar.width();
}

int GradientEditor::markerAt(QPointF pos) const
{
    const QRectF bar = barRect();
    if (pos.y() < bar.bottom() - 2) {
        return -1;
    }
    int best = -1;
    double bestD = kMarker + 2;
    // The selected one first, then the nearest.
    for (int i = 0; i < static_cast<int>(m_gradient.stops.size()); ++i) {
        const double d = std::abs(pos.x() - xAt(m_gradient.stops[size_t(i)].offset)) - (i == m_selected ? 1.0 : 0.0);
        if (d < bestD) {
            bestD = d;
            best = i;
        }
    }
    return best;
}

int GradientEditor::moveTo(int i, double offset)
{
    auto& stops = m_gradient.stops;
    const double was = stops[size_t(i)].offset;
    stops[size_t(i)].offset = std::clamp(offset, 0.0, 1.0);
    // (Moved onto another stop's place: it goes past it, the way it was moving.)
    const bool left = stops[size_t(i)].offset < was;
    while (i > 0 && (stops[size_t(i - 1)].offset > stops[size_t(i)].offset
                     || (left && stops[size_t(i - 1)].offset == stops[size_t(i)].offset))) {
        std::swap(stops[size_t(i - 1)], stops[size_t(i)]);
        --i;
    }
    while (i + 1 < static_cast<int>(stops.size())
           && (stops[size_t(i + 1)].offset < stops[size_t(i)].offset
               || (!left && stops[size_t(i)].offset > was && stops[size_t(i + 1)].offset == stops[size_t(i)].offset))) {
        std::swap(stops[size_t(i + 1)], stops[size_t(i)]);
        ++i;
    }
    return i;
}

int GradientEditor::addStop(double offset)
{
    offset = std::clamp(offset, 0.0, 1.0);
    const core::GradientStop stop{offset, m_gradient.colorAt(offset)};
    auto it = std::ranges::upper_bound(m_gradient.stops, offset, {}, &core::GradientStop::offset);
    const int index = static_cast<int>(it - m_gradient.stops.begin());
    m_gradient.stops.insert(it, stop);
    m_selected = index;
    update();
    emit selectionChanged(index);
    emit edited();
    return index;
}

void GradientEditor::moveStop(int index, double offset)
{
    if (index < 0 || index >= static_cast<int>(m_gradient.stops.size())) {
        return;
    }
    const int now = moveTo(index, offset);
    if (index == m_selected && now != index) {
        m_selected = now;
        emit selectionChanged(now);
    }
    update();
    emit edited();
}

bool GradientEditor::removeStop(int index)
{
    if (index < 0 || index >= static_cast<int>(m_gradient.stops.size()) || m_gradient.stops.size() <= 2) {
        return false;
    }
    m_gradient.stops.erase(m_gradient.stops.begin() + index);
    m_selected = std::min(index, static_cast<int>(m_gradient.stops.size()) - 1);
    update();
    emit selectionChanged(m_selected);
    emit edited();
    return true;
}

void GradientEditor::setStopColor(int index, const core::Color& color)
{
    if (index < 0 || index >= static_cast<int>(m_gradient.stops.size())) {
        return;
    }
    m_gradient.stops[size_t(index)].color = color;
    update();
    emit edited();
}

void GradientEditor::paintEvent(QPaintEvent*)
{
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);
    const QRectF bar = barRect();
    checkerboard(p, bar);
    if (!m_gradient.stops.empty()) {
        QLinearGradient lg(bar.topLeft(), bar.topRight());
        for (const core::GradientStop& s : m_gradient.stops) {
            lg.setColorAt(s.offset, toQt(s.color));
        }
        p.fillRect(bar, lg);
    }
    p.setPen(QColor(0x55, 0x5c, 0x66));
    p.setBrush(Qt::NoBrush);
    p.drawRect(bar.adjusted(-0.5, -0.5, 0.5, 0.5));

    // Markers: a small house shape pointing up at its place, filled with its colour.
    for (int i = 0; i < static_cast<int>(m_gradient.stops.size()); ++i) {
        const core::GradientStop& s = m_gradient.stops[size_t(i)];
        const double x = xAt(s.offset);
        const double top = bar.bottom() + 1;
        const double drop = (i == m_dragging && m_dragOff) ? kRemoveDrop : 0.0;
        QPainterPath marker;
        marker.moveTo(x, top + drop);
        marker.lineTo(x + kMarker, top + 6 + drop);
        marker.lineTo(x + kMarker, top + 17 + drop);
        marker.lineTo(x - kMarker, top + 17 + drop);
        marker.lineTo(x - kMarker, top + 6 + drop);
        marker.closeSubpath();
        const bool selected = i == m_selected;
        p.setPen(QPen(selected ? QColor(0x4d, 0xa3, 0xff) : QColor(0xc9, 0xd1, 0xdb), selected ? 2.0 : 1.0));
        p.setBrush(QColor(0x26, 0x29, 0x2e));
        p.drawPath(marker);
        const QRectF swatch(x - kMarker + 2.5, top + 7.5 + drop, 2 * kMarker - 5, 7);
        checkerboard(p, swatch);
        p.fillRect(swatch, toQt(s.color));
    }
}

void GradientEditor::mousePressEvent(QMouseEvent* e)
{
    const QPointF pos = e->position();
    const int marker = markerAt(pos);
    if (e->button() == Qt::RightButton) {
        if (marker >= 0) {
            removeStop(marker);
        }
        return;
    }
    if (e->button() != Qt::LeftButton) {
        return;
    }
    if (marker >= 0) {
        setSelectedStop(marker);
        m_dragging = marker;
        m_dragOff = false;
        return;
    }
    if (barRect().adjusted(-kSide, 0, kSide, 0).contains(pos)) {
        m_dragging = addStop(offsetAt(pos.x()));  // and drag it from there
        m_dragOff = false;
    }
}

void GradientEditor::mouseMoveEvent(QMouseEvent* e)
{
    if (m_dragging < 0) {
        return;
    }
    const QPointF pos = e->position();
    m_dragOff = pos.y() > barRect().bottom() + 17 + kRemoveDrop && m_gradient.stops.size() > 2;
    const int now = moveTo(m_dragging, offsetAt(pos.x()));
    if (now != m_dragging) {
        m_dragging = now;
        m_selected = now;
        emit selectionChanged(now);
    }
    update();
    emit editing();
}

void GradientEditor::mouseReleaseEvent(QMouseEvent* e)
{
    if (e->button() != Qt::LeftButton || m_dragging < 0) {
        return;
    }
    const int index = std::exchange(m_dragging, -1);
    if (std::exchange(m_dragOff, false)) {
        removeStop(index);
        return;
    }
    update();
    emit edited();
}

void GradientEditor::mouseDoubleClickEvent(QMouseEvent* e)
{
    const int marker = markerAt(e->position());
    if (marker >= 0) {
        setSelectedStop(marker);
        emit colorRequested(marker);
    }
}

void GradientEditor::keyPressEvent(QKeyEvent* e)
{
    if ((e->key() == Qt::Key_Delete || e->key() == Qt::Key_Backspace) && m_selected >= 0) {
        removeStop(m_selected);
        return;
    }
    if ((e->key() == Qt::Key_Left || e->key() == Qt::Key_Right) && m_selected >= 0) {
        const double step = (e->modifiers() & Qt::ShiftModifier) ? 0.1 : 0.01;
        moveStop(m_selected, m_gradient.stops[size_t(m_selected)].offset + (e->key() == Qt::Key_Left ? -step : step));
        return;
    }
    QWidget::keyPressEvent(e);
}
