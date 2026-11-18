// Big shapes filled a square of pixels at a time (see TiledFill.h): it must make just what filling
// them whole does, by either fill rule, under any pan or zoom.

#include "TiledFill.h"

#include <catch2/catch_test_macros.hpp>

#include <QImage>
#include <QPainter>

#include <cmath>
#include <numbers>

namespace
{
// Stars (crossing themselves), rings, overlapping blobs and a few big shapes, over 700 x 500.
std::vector<QPolygonF> shapes()
{
    std::vector<QPolygonF> out;
    std::uint32_t seed = 12345;
    auto next = [&] {
        seed = seed * 1664525u + 1013904223u;
        return (seed >> 8) / double(1 << 24);
    };
    auto ring = [&](QPointF c, double r, int n, bool backwards, double wobble) {
        QPolygonF p;
        for (int k = 0; k < n; ++k) {
            const double a = (backwards ? -1 : 1) * 2 * std::numbers::pi * k / n;
            const double rr = r * (1 + wobble * (next() - 0.5));
            p << c + QPointF(rr * std::cos(a), rr * std::sin(a));
        }
        return p;
    };
    for (int i = 0; i < 40; ++i) {
        const QPointF c(20 + 660 * next(), 20 + 460 * next());
        const double r = 5 + 60 * next();
        out.push_back(ring(c, r, 12 + static_cast<int>(200 * next()), false, 0.3));
        if (i % 3 == 0) {
            out.push_back(ring(c, r * 0.5, 40, i % 2 == 0, 0.0));  // a hole, or (same way round) not
        }
    }
    for (int i = 0; i < 6; ++i) {  // stars, crossing themselves
        const QPointF c(80 + 540 * next(), 80 + 340 * next());
        QPolygonF star;
        for (int k = 0; k < 7; ++k) {
            const double a = 2 * std::numbers::pi * k * 3 / 7;
            star << c + QPointF(90 * std::cos(a), 90 * std::sin(a));
        }
        out.push_back(star);
    }
    out.push_back(ring(QPointF(350, 250), 240, 2000, false, 0.05));  // big, many corners
    return out;
}

int worstDifference(const QImage& a, const QImage& b)
{
    int worst = 0;
    for (int y = 0; y < a.height(); ++y) {
        const QRgb* p = reinterpret_cast<const QRgb*>(a.constScanLine(y));
        const QRgb* q = reinterpret_cast<const QRgb*>(b.constScanLine(y));
        for (int x = 0; x < a.width(); ++x) {
            worst = std::max({worst, std::abs(qRed(p[x]) - qRed(q[x])), std::abs(qGreen(p[x]) - qGreen(q[x])),
                              std::abs(qBlue(p[x]) - qBlue(q[x])), std::abs(qAlpha(p[x]) - qAlpha(q[x]))});
        }
    }
    return worst;
}
}  // namespace

TEST_CASE("Tiled fill: makes what filling the shape whole does")
{
    const std::vector<QPolygonF> polygons = shapes();
    std::vector<QRectF> boxes;
    QPainterPath whole;
    for (const QPolygonF& p : polygons) {
        whole.addPolygon(p);
        whole.closeSubpath();
        boxes.push_back(p.boundingRect());
    }
    for (Qt::FillRule rule : {Qt::WindingFill, Qt::OddEvenFill}) {
        whole.setFillRule(rule);
        for (int view = 0; view < 4; ++view) {
            for (int tile : {32, 128}) {
                for (const QColor& color : {QColor(200, 120, 40), QColor(40, 160, 220, 90)}) {
                    auto paint = [&](bool tiled) {
                        QImage image(1100, 800, QImage::Format_ARGB32_Premultiplied);
                        if (view == 3) {
                            image.setDevicePixelRatio(2.0);  // (a scaled screen's: Qt scales it)
                        }
                        image.fill(QColor(30, 32, 36));
                        QPainter p(&image);
                        p.setRenderHint(QPainter::Antialiasing);
                        if (view == 1) {
                            p.translate(13.37, 7.61);  // (a pan of part pixels)
                        }
                        if (view == 2 || view == 3) {
                            p.translate(-41.3, 22.9);
                            p.scale(1.53, 1.53);  // (zoomed, as on a scaled screen)
                        }
                        if (tiled) {
                            tiled::fill(p, whole, polygons, boxes, color, rule, tile);
                        }
                        else {
                            p.fillPath(whole, color);
                        }
                        return image;
                    };
                    INFO("rule " << int(rule) << ", view " << view << ", tile " << tile << ", alpha " << color.alpha());
                    // Inside (where a seam would show: pixels whose every neighbour is filled too)
                    // just as whole; along the edges, within the rasteriser's rounding (it works a
                    // big shape out a little differently).
                    const QImage asOne = paint(false), inTiles = paint(true);
                    QImage solid(1, 1, QImage::Format_ARGB32_Premultiplied);
                    solid.fill(QColor(30, 32, 36));
                    QPainter(&solid).fillRect(0, 0, 1, 1, color);
                    const QRgb inside = solid.pixel(0, 0);
                    int insideChanged = 0;
                    for (int y = 1; y + 1 < asOne.height(); ++y) {
                        for (int x = 1; x + 1 < asOne.width(); ++x) {
                            bool interior = true;
                            for (int k = 0; k < 9; ++k) {
                                interior = interior && asOne.pixel(x + k % 3 - 1, y + k / 3 - 1) == inside;
                            }
                            insideChanged += interior && inTiles.pixel(x, y) != inside;
                        }
                    }
                    CHECK(insideChanged == 0);
                    CHECK(worstDifference(asOne, inTiles) <= 4);
                }
            }
        }
    }
}

TEST_CASE("Tiled fill: a polygon cut to a box keeps what's inside it")
{
    const QPolygonF square{{0, 0}, {10, 0}, {10, 10}, {0, 10}};
    const QPolygonF cut = tiled::clipped(square, QRectF(5, -5, 20, 10));
    CHECK(cut.boundingRect() == QRectF(5, 0, 5, 5));
    CHECK(tiled::clipped(square, QRectF(20, 20, 5, 5)).isEmpty());
    CHECK(tiled::clipped(square, QRectF(-1, -1, 12, 12)) == square);  // (all inside: as it was)
}
