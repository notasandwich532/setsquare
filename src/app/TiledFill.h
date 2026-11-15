#pragma once

// Filling big shapes quickly. Qt's rasteriser slows down badly on a shape of thousands of edges
// over many rows and columns: a text of 70,000 corners filled whole is ten times slower than
// letter by letter, and an area of a few thousand a few times slower than in pieces.

#include <QPainterPath>
#include <QPolygonF>
#include <QRectF>

#include <vector>

class QBrush;
class QPainter;

namespace tiled
{
// The part of a closed polygon inside a box: what it covers there, its winding unchanged (the cut
// edges run along the box, and edges along it add nothing inside). Empty if nothing's left.
QPolygonF clipped(const QPolygonF& polygon, const QRectF& box);

// Fills `polygons` (with their bounding `boxes`) by `rule`, as filling `whole` (them, as one path)
// would, but a square of `tile` device pixels at a time, each with only what's in it. The squares
// meet on whole device pixels, so no pixel is filled from two: the result is what filling it
// whole makes. (Under a rotating or shearing transform, or when it's small, it's filled whole.)
void fill(QPainter& painter, const QPainterPath& whole, const std::vector<QPolygonF>& polygons,
          const std::vector<QRectF>& boxes, const QBrush& brush, Qt::FillRule rule, int tile);
}  // namespace tiled