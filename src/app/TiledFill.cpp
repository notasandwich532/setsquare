#include "TiledFill.h"

#include <QPainter>

#include <algorithm>
#include <cmath>

namespace tiled
{
QPolygonF clipped(const QPolygonF& in, const QRectF& box)
{
    auto cut = [](const QPolygonF& poly, auto inside, auto cross) {
        QPolygonF out;
        out.reserve(poly.size() + 4);
        const qsizetype n = poly.size();
        for (qsizetype i = 0; i < n; ++i) {
            const QPointF& a = poly[i];
            const QPointF& b = poly[(i + 1) % n];
            const bool aIn = inside(a), bIn = inside(b);
            if (aIn) {
                out << a;
            }
            if (aIn != bIn) {
                out << cross(a, b);
            }
        }
        return out;
    };
    auto atY = [](double y) {
        return [y](const QPointF& a, const QPointF& b) { return QPointF(a.x() + (y - a.y()) / (b.y() - a.y()) * (b.x() - a.x()), y); };
    };
    auto atX = [](double x) {
        return [x](const QPointF& a, const QPointF& b) { return QPointF(x, a.y() + (x - a.x()) / (b.x() - a.x()) * (b.y() - a.y())); };
    };
    QPolygonF p = in;
    if (box.top() > p.boundingRect().top()) {
        p = cut(p, [&](const QPointF& q) { return q.y() >= box.top(); }, atY(box.top()));
    }
    if (p.size() >= 3 && box.bottom() < p.boundingRect().bottom()) {
        p = cut(p, [&](const QPointF& q) { return q.y() <= box.bottom(); }, atY(box.bottom()));
    }
    if (p.size() >= 3 && box.left() > p.boundingRect().left()) {
        p = cut(p, [&](const QPointF& q) { return q.x() >= box.left(); }, atX(box.left()));
    }
    if (p.size() >= 3 && box.right() < p.boundingRect().right()) {
        p = cut(p, [&](const QPointF& q) { return q.x() <= box.right(); }, atX(box.right()));
    }
    return p.size() >= 3 ? p : QPolygonF();
}

void fill(QPainter& painter, const QPainterPath& whole, const std::vector<QPolygonF>& polygons,
          const std::vector<QRectF>& boxes, const QBrush& brush, Qt::FillRule rule, int tile)
{
    const QTransform m = painter.deviceTransform();
    const QRectF bounds = whole.boundingRect();
    if (m.type() > QTransform::TxScale || m.m11() <= 0.0 || m.m22() <= 0.0
        || std::max(bounds.width() * m.m11(), bounds.height() * m.m22()) <= 2 * tile) {
        painter.fillPath(whole, brush);
        return;
    }
    // Device pixel (c, r) starts at ((c - dx) / m11, (r - dy) / m22) here.
    const double sx = m.m11(), sy = m.m22(), dx = m.dx(), dy = m.dy();
    const double col0 = std::floor(bounds.left() * sx + dx), row0 = std::floor(bounds.top() * sy + dy);
    const int cols = std::max(1, static_cast<int>(std::ceil((bounds.right() * sx + dx - col0) / tile)));
    const int rows = std::max(1, static_cast<int>(std::ceil((bounds.bottom() * sy + dy - row0) / tile)));
    auto tileOf = [&](double v, double scale, double shift, double origin, int count) {
        return std::clamp(static_cast<int>(std::floor((v * scale + shift - origin) / tile)), 0, count - 1);
    };
    std::vector<QPainterPath> tiles(static_cast<size_t>(cols * rows));
    for (size_t i = 0; i < polygons.size(); ++i) {
        const QRectF& box = boxes[i];
        const int c0 = tileOf(box.left(), sx, dx, col0, cols), c1 = tileOf(box.right(), sx, dx, col0, cols);
        const int r0 = tileOf(box.top(), sy, dy, row0, rows), r1 = tileOf(box.bottom(), sy, dy, row0, rows);
        for (int r = r0; r <= r1; ++r) {
            for (int c = c0; c <= c1; ++c) {
                QPainterPath& piece = tiles[static_cast<size_t>(r * cols + c)];
                if (c0 == c1 && r0 == r1) {
                    piece.addPolygon(polygons[i]);  // (all of it in this square)
                }
                else {
                    const QRectF area(QPointF((col0 + c * tile - dx) / sx, (row0 + r * tile - dy) / sy),
                                      QPointF((col0 + (c + 1) * tile - dx) / sx, (row0 + (r + 1) * tile - dy) / sy));
                    const QPolygonF part = clipped(polygons[i], area);
                    if (part.isEmpty()) {
                        continue;
                    }
                    piece.addPolygon(part);
                }
                piece.closeSubpath();
            }
        }
    }
    for (QPainterPath& piece : tiles) {
        if (!piece.isEmpty()) {
            piece.setFillRule(rule);
            painter.fillPath(piece, brush);
        }
    }
}
}  // namespace tiled
