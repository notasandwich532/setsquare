#include "Handles.h"

#include "../SketchView.h"

#include <QPainter>
#include <QPainterPath>

#include <cmath>

namespace
{
constexpr double kArrowLength = 24.0;  // pixels
constexpr double kArrowWidth = 16.0;
constexpr double kKnobRadius = 9.0;
constexpr double kHitSlack = 5.0;

const QColor kFill(0xf2, 0xf4, 0xf7);
const QColor kHot(0x4d, 0xa3, 0xff);
const QColor kOutline(0x1b, 0x1d, 0x21);

// Screen direction of a world direction (y flips).
QPointF screenDir(const Handle& h)
{
    const double n = std::hypot(h.dir.x, h.dir.y);
    return n > 0 ? QPointF(h.dir.x / n, -h.dir.y / n) : QPointF(1, 0);
}

QPainterPath arrowPath(QPointF base, QPointF u)
{
    const QPointF n(-u.y(), u.x());
    const double L = kArrowLength, W = kArrowWidth;
    // A block arrow: shaft then head, like Fusion's.
    QPainterPath path;
    path.moveTo(base + n * (W * 0.2));
    path.lineTo(base + u * (L * 0.45) + n * (W * 0.2));
    path.lineTo(base + u * (L * 0.45) + n * (W * 0.5));
    path.lineTo(base + u * L);
    path.lineTo(base + u * (L * 0.45) - n * (W * 0.5));
    path.lineTo(base + u * (L * 0.45) - n * (W * 0.2));
    path.lineTo(base - n * (W * 0.2));
    path.closeSubpath();
    return path;
}
}  // namespace

void drawHandle(const SketchView& view, QPainter& painter, const Handle& h, bool hot)
{
    const QPointF p = view.toScreen(h.at);
    painter.save();
    painter.setRenderHint(QPainter::Antialiasing);
    painter.setPen(QPen(kOutline, 1.5));
    painter.setBrush(hot ? kHot : kFill);
    switch (h.kind) {
        case Handle::Kind::Arrow: painter.drawPath(arrowPath(p, screenDir(h))); break;
        case Handle::Kind::Rotate: {
            painter.drawEllipse(p, kKnobRadius, kKnobRadius);
            painter.setPen(QPen(kOutline, 1.5));
            painter.setBrush(Qt::NoBrush);
            const QRectF ring(p.x() - 5, p.y() - 5, 10, 10);
            painter.drawArc(ring, 30 * 16, 240 * 16);  // a turning arrow
            break;
        }
        case Handle::Kind::Move: painter.drawRect(QRectF(p.x() - 6, p.y() - 6, 12, 12)); break;
    }
    painter.restore();
}

bool hitHandle(const SketchView& view, const Handle& h, QPointF screen)
{
    const QPointF p = view.toScreen(h.at);
    if (h.kind == Handle::Kind::Arrow) {
        const QPointF centre = p + screenDir(h) * (kArrowLength / 2);
        const QPointF d = screen - centre;
        return std::hypot(d.x(), d.y()) <= kArrowLength / 2 + kHitSlack;
    }
    const QPointF d = screen - p;
    return std::hypot(d.x(), d.y()) <= kKnobRadius + kHitSlack;
}
