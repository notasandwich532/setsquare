#include "TextShapes.h"

#include <QFont>
#include <QFontDatabase>
#include <QFontMetricsF>
#include <QPainterPath>
#include <QImage>
#include <QPainter>
#include <QPolygonF>
#include <QTransform>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <functional>
#include <map>

namespace textshapes
{

namespace
{
// Glyphs are drawn this big (pixels) and flattened at that size, so curves stay smooth however
// small the text is made.
constexpr double kPixelSize = 200.0;

// Only the outer contours: a contour inside another is a hole (or something drawn inside).
QPainterPath silhouette(const QPainterPath& merged)
{
    const QList<QPolygonF> contours = merged.toSubpathPolygons();
    QPainterPath out;
    out.setFillRule(Qt::WindingFill);
    for (int i = 0; i < contours.size(); ++i) {
        const QPolygonF& c = contours[i];
        if (c.size() < 3) {
            continue;
        }
        const bool inside = std::ranges::any_of(contours, [&](const QPolygonF& other) {
            return &other != &c && other.size() >= 3 && other.containsPoint(c.first(), Qt::OddEvenFill)
                && other.boundingRect().contains(c.boundingRect());
        });
        if (!inside) {
            out.addPolygon(c);
            out.closeSubpath();
        }
    }
    return out.simplified();
}

// --- minimum thickness, on a fine grid ---
//
// The narrow parts of a shape are what a disc as wide as the minimum can't reach rolling round
// inside it (the shape less its morphological opening). Each is widened by the full minimum, so
// it ends up at least that thick, and the rest is left alone. Done with exact distance fields on
// a grid (path booleans are far too slow and fragile for this), then traced back into smooth
// outlines.

constexpr double kFar = 1e20;

// Squared distance to the nearest set pixel, for every pixel (Felzenszwalb & Huttenlocher).
std::vector<float> distanceSquared(const std::vector<std::uint8_t>& set, int w, int h)
{
    std::vector<double> d(set.size());
    for (size_t i = 0; i < set.size(); ++i) {
        d[i] = set[i] ? 0.0 : kFar;
    }
    const size_t n = static_cast<size_t>(std::max(w, h));
    std::vector<double> f(n), out(n), z(n + 1);
    std::vector<size_t> v(n);
    // One row or column: out[q] = min over p of (q - p)^2 + f[p].
    auto pass = [&](size_t count) {
        size_t k = 0;
        v[0] = 0;
        z[0] = -kFar;
        z[1] = kFar;
        auto meet = [&](size_t q, size_t p) {
            const double dq = double(q), dp = double(p);
            return ((f[q] + dq * dq) - (f[p] + dp * dp)) / (2.0 * (dq - dp));
        };
        for (size_t q = 1; q < count; ++q) {
            double s = meet(q, v[k]);
            while (s <= z[k]) {
                --k;
                s = meet(q, v[k]);
            }
            ++k;
            v[k] = q;
            z[k] = s;
            z[k + 1] = kFar;
        }
        k = 0;
        for (size_t q = 0; q < count; ++q) {
            while (z[k + 1] < double(q)) {
                ++k;
            }
            const double dq = double(q) - double(v[k]);
            out[q] = dq * dq + f[v[k]];
        }
    };
    const size_t W = static_cast<size_t>(w), H = static_cast<size_t>(h);
    for (size_t x = 0; x < W; ++x) {
        for (size_t y = 0; y < H; ++y) {
            f[y] = d[y * W + x];
        }
        pass(H);
        for (size_t y = 0; y < H; ++y) {
            d[y * W + x] = out[y];
        }
    }
    std::vector<float> result(set.size());
    for (size_t y = 0; y < H; ++y) {
        for (size_t x = 0; x < W; ++x) {
            f[x] = d[y * W + x];
        }
        pass(W);
        for (size_t x = 0; x < W; ++x) {
            result[y * W + x] = static_cast<float>(std::min(out[x], 1e30));
        }
    }
    return result;
}
// The loops where `field` crosses zero (negative inside), at sub-pixel positions, each with the
// inside on the same side (so holes run the other way round from outlines).
std::vector<QPolygonF> traceZero(const std::vector<float>& field, int w, int h)
{
    auto at = [&](int x, int y) { return field[static_cast<size_t>(y) * w + x]; };
    // Edges between grid points: horizontal (x, y)-(x+1, y) and vertical (x, y)-(x, y+1).
    auto edgeId = [&](int x, int y, bool vertical) { return (static_cast<qint64>(y) * w + x) * 2 + (vertical ? 1 : 0); };
    auto crossing = [&](int x0, int y0, int x1, int y1) {
        const float a = at(x0, y0), b = at(x1, y1);
        const double t = a / double(a - b);
        return QPointF(x0 + t * (x1 - x0), y0 + t * (y1 - y0));
    };
    struct Segment
    {
        qint64 from, to;
        QPointF a, b;
    };
    std::map<qint64, Segment> byStart;
    for (int y = 0; y + 1 < h; ++y) {
        for (int x = 0; x + 1 < w; ++x) {
            // Corners clockwise on screen (y down) from the top left, and the edges between them.
            const int cx[4] = {x, x + 1, x + 1, x};
            const int cy[4] = {y, y, y + 1, y + 1};
            bool in[4];
            for (int i = 0; i < 4; ++i) {
                in[i] = at(cx[i], cy[i]) < 0;
            }
            if (in[0] == in[1] && in[1] == in[2] && in[2] == in[3]) {
                continue;
            }
            const qint64 edges[4] = {edgeId(x, y, false), edgeId(x + 1, y, true), edgeId(x, y + 1, false),
                                     edgeId(x, y, true)};
            struct Cross
            {
                int edge;
                bool entering;  // outside -> inside, walking round
            };
            std::vector<Cross> crosses;
            for (int i = 0; i < 4; ++i) {
                if (in[i] != in[(i + 1) % 4]) {
                    crosses.push_back({i, !in[i]});
                }
            }
            auto point = [&](int e) {
                return crossing(cx[e], cy[e], cx[(e + 1) % 4], cy[(e + 1) % 4]);
            };
            auto add = [&](int fromEdge, int toEdge) {
                byStart[edges[fromEdge]] = {edges[fromEdge], edges[toEdge], point(fromEdge), point(toEdge)};
            };
            // Each segment runs from where the walk enters the inside to where it leaves (a
            // corner cut off), or with a joined-up middle (a saddle whose centre is inside),
            // from where it enters to where it last left.
            const bool joined = crosses.size() == 4 && (at(x, y) + at(x + 1, y) + at(x + 1, y + 1) + at(x, y + 1)) < 0;
            const int n = static_cast<int>(crosses.size());
            for (int i = 0; i < n; ++i) {
                if (!crosses[static_cast<size_t>(i)].entering) {
                    continue;
                }
                const int j = joined ? (i + n - 1) % n : (i + 1) % n;
                add(crosses[static_cast<size_t>(i)].edge, crosses[static_cast<size_t>(j)].edge);
            }
        }
    }
    // Chain the segments into loops (each ends where the next starts).
    std::vector<QPolygonF> loops;
    while (!byStart.empty()) {
        auto it = byStart.begin();
        const qint64 first = it->first;
        QPolygonF loop;
        qint64 next = first;
        while (true) {
            auto s = byStart.find(next);
            if (s == byStart.end()) {
                break;
            }
            loop << s->second.a;
            next = s->second.to;
            byStart.erase(s);
            if (next == first) {
                break;
            }
        }
        if (loop.size() >= 3) {
            loops.push_back(loop);
        }
    }
    return loops;
}

// The largest value within `radius` pixels (a square window), for every pixel; constant time per
// pixel whatever the radius (van Herk / Gil-Werman).
std::vector<float> maxFilter(const std::vector<float>& in, int w, int h, int radius)
{
    std::vector<float> out = in;
    const int window = 2 * radius + 1;
    auto run = [&](int count, auto get, auto set) {
        std::vector<float> line(static_cast<size_t>(count)), forward(line.size()), backward(line.size());
        for (int i = 0; i < count; ++i) {
            line[static_cast<size_t>(i)] = get(i);
        }
        for (int i = 0; i < count; ++i) {
            const size_t k = static_cast<size_t>(i);
            forward[k] = i % window == 0 ? line[k] : std::max(forward[k - 1], line[k]);
        }
        for (int i = count - 1; i >= 0; --i) {
            const size_t k = static_cast<size_t>(i);
            backward[k] = (i == count - 1 || (i + 1) % window == 0) ? line[k] : std::max(backward[k + 1], line[k]);
        }
        for (int i = 0; i < count; ++i) {
            const int lo = std::max(0, i - radius), hi = std::min(count - 1, i + radius);
            set(i, std::max(backward[static_cast<size_t>(lo)], forward[static_cast<size_t>(hi)]));
        }
    };
    const size_t W = static_cast<size_t>(w);
    for (int y = 0; y < h; ++y) {
        const size_t row = static_cast<size_t>(y) * W;
        run(w, [&](int x) { return out[row + static_cast<size_t>(x)]; },
            [&](int x, float v) { out[row + static_cast<size_t>(x)] = v; });
    }
    std::vector<float> column = out;
    for (int x = 0; x < w; ++x) {
        run(h, [&](int y) { return column[static_cast<size_t>(y) * W + static_cast<size_t>(x)]; },
            [&](int y, float v) { out[static_cast<size_t>(y) * W + static_cast<size_t>(x)] = v; });
    }
    return out;
}
// Fewer points, staying within `tolerance` of the original (Douglas-Peucker, closed loop).
QPolygonF simplify(const QPolygonF& loop, double tolerance)
{
    const int n = static_cast<int>(loop.size());
    if (n < 8) {
        return loop;
    }
    std::vector<bool> keep(static_cast<size_t>(n), false);
    std::function<void(int, int)> run = [&](int a, int b) {  // indices, b may wrap (b > a)
        const QPointF p = loop[a % n], q = loop[b % n];
        const QPointF d = q - p;
        const double len = std::hypot(d.x(), d.y());
        double worst = -1;
        int at = -1;
        for (int i = a + 1; i < b; ++i) {
            const QPointF r = loop[i % n] - p;
            const double dist = len > 0 ? std::abs(d.x() * r.y() - d.y() * r.x()) / len : std::hypot(r.x(), r.y());
            if (dist > worst) {
                worst = dist;
                at = i;
            }
        }
        if (worst > tolerance && at > 0) {
            keep[static_cast<size_t>(at % n)] = true;
            run(a, at);
            run(at, b);
        }
    };
    keep[0] = true;
    keep[static_cast<size_t>(n / 2)] = true;
    run(0, n / 2);
    run(n / 2, n);
    QPolygonF out;
    for (int i = 0; i < n; ++i) {
        if (keep[static_cast<size_t>(i)]) {
            out << loop[i];
        }
    }
    return out;
}

QPainterPath thicken(const QPainterPath& shape, double width)
{
    const QRectF box = shape.boundingRect();
    if (box.isEmpty() || !(width > 0)) {
        return shape;
    }
    // Fine enough to resolve the minimum well (and the letters), within a pixel budget.
    double k = std::max(4.0, 12.0 / width);  // grid pixels per path unit
    const double margin = width + 2;
    const double area = (box.width() + 2 * margin) * (box.height() + 2 * margin);
    k = std::min(k, std::sqrt(6.0e6 / std::max(area, 1.0)));
    const int w = static_cast<int>(std::ceil((box.width() + 2 * margin) * k)) + 2;
    const int h = static_cast<int>(std::ceil((box.height() + 2 * margin) * k)) + 2;
    const QPointF origin = box.topLeft() - QPointF(margin, margin);

    // The shape, sampled at pixel centres (anti-aliased, then cut at half).
    QImage image(w, h, QImage::Format_Grayscale8);
    image.fill(0);
    {
        QPainter painter(&image);
        painter.setRenderHint(QPainter::Antialiasing);
        painter.scale(k, k);
        painter.translate(-origin);
        painter.fillPath(shape, Qt::white);
    }
    std::vector<std::uint8_t> inside(static_cast<size_t>(w) * h), outside(inside.size());
    for (int y = 0; y < h; ++y) {
        const uchar* row = image.constScanLine(y);
        for (int x = 0; x < w; ++x) {
            inside[static_cast<size_t>(y) * w + x] = row[x] >= 128;
            outside[static_cast<size_t>(y) * w + x] = row[x] < 128;
        }
    }
    const float r = float(width * k / 2);
    auto distances = [&](const std::vector<std::uint8_t>& set) {
        std::vector<float> d = distanceSquared(set, w, h);
        for (float& v : d) {
            v = std::sqrt(v);
        }
        return d;
    };
    const std::vector<float> toOutside = distances(outside);  // inside: how deep
    const std::vector<float> toInside = distances(inside);
    // Opening: shrink by r (what's deeper than r), then grow that back by r.
    std::vector<std::uint8_t> core(inside.size()), opened(inside.size()), narrow(inside.size());
    for (size_t i = 0; i < inside.size(); ++i) {
        core[i] = inside[i] && toOutside[i] > r;
    }
    const std::vector<float> toCore = distances(core);
    for (size_t i = 0; i < inside.size(); ++i) {
        opened[i] = inside[i] && toCore[i] <= r;
    }
    // What's left is narrow; but a thick part's sharp-ish corners are left too, right next to the
    // opening. Only narrow parts reaching beyond r / 2 from the opening count (a thin stroke
    // does; a 90 degree corner's tip is 0.41 r away).
    const std::vector<float> toOpened = distances(opened);
    for (size_t i = 0; i < inside.size(); ++i) {
        narrow[i] = inside[i] && !opened[i] && toOpened[i] > r / 2;
    }
    // The narrow parts' centre lines: the ridge of depth (a pixel at least as deep as its
    // neighbours either side, and deeper than one, in some direction). A disc as wide as the minimum along each makes
    // it exactly that thick, and reaches over the r / 2 left out next to the thick parts.
    std::vector<std::uint8_t> ridge(inside.size(), 0);
    bool any = false;
    auto depth = [&](int x, int y) {
        return x < 0 || y < 0 || x >= w || y >= h ? 0.0f : toOutside[static_cast<size_t>(y) * w + x];
    };
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            const size_t i = static_cast<size_t>(y) * w + x;
            if (!narrow[i]) {
                continue;
            }
            const float d = toOutside[i];
            const int dirs[4][2] = {{1, 0}, {0, 1}, {1, 1}, {1, -1}};
            for (const auto& dir : dirs) {
                const float a = depth(x + dir[0], y + dir[1]), b = depth(x - dir[0], y - dir[1]);
                if (d >= a && d >= b && (d > a || d > b)) {  // (not along the stroke, where it's flat)
                    ridge[i] = 1;
                    any = true;
                    break;
                }
            }
        }
    }
    if (!any) {
        return shape;
    }
    // Where a stroke ends, its centre line forks into the corners, getting shallower; discs there
    // would give it dog-bone ends. Keep only ridge nearly as deep as the deepest ridge within a
    // disc's width (a hairline thin all along keeps all of its centre line).
    {
        std::vector<float> ridgeDepth(inside.size(), 0.0f);
        for (size_t i = 0; i < inside.size(); ++i) {
            ridgeDepth[i] = ridge[i] ? toOutside[i] : 0.0f;
        }
        const std::vector<float> deepestNear = maxFilter(ridgeDepth, w, h, static_cast<int>(std::ceil(r)));
        for (size_t i = 0; i < inside.size(); ++i) {
            ridge[i] = ridge[i] && toOutside[i] >= 0.75f * deepestNear[i];
        }
    }
    const std::vector<float> toRidge = distances(ridge);
    // The result as a field (negative inside): the shape, and those discs.
    std::vector<float> field(inside.size());
    for (size_t i = 0; i < inside.size(); ++i) {
        const float shapeField = inside[i] ? 0.5f - toOutside[i] : toInside[i] - 0.5f;
        field[i] = std::min(shapeField, toRidge[i] - r);
    }
    QPainterPath out;
    out.setFillRule(Qt::WindingFill);
    const QTransform back = QTransform::fromTranslate(origin.x(), origin.y()).scale(1 / k, 1 / k);
    for (const QPolygonF& loop : traceZero(field, w, h)) {
        QPolygonF simple = simplify(loop, 0.3);
        out.addPolygon(back.map(simple));
        out.closeSubpath();
    }
    return out;
}
}  // namespace

std::vector<std::vector<core::Point>> make(const core::Text& t)
{
    if (t.text.empty()) {
        return {};
    }
    QFont font(QString::fromStdString(t.font));
    font.setPixelSize(static_cast<int>(kPixelSize));
    font.setBold(t.bold);
    font.setItalic(t.italic);
    font.setKerning(true);

    QPainterPath path;
    path.setFillRule(Qt::WindingFill);
    path.addText(0, 0, font, QString::fromStdString(t.text));  // baseline at y = 0, y down
    path.setFillRule(Qt::WindingFill);

    double capHeight = QFontMetricsF(font).capHeight();
    if (!(capHeight > 0)) {
        capHeight = QFontMetricsF(font).ascent() * 0.7;
    }
    const double mmPerPixel = t.height / capHeight;

    switch (t.outline) {
        case core::TextOutline::AsDesigned: break;
        case core::TextOutline::Merged: path = path.simplified(); break;
        case core::TextOutline::Silhouette: path = silhouette(path.simplified()); break;
    }
    if (t.minThickness > 0.0) {
        path = thicken(path.simplified(), t.minThickness / mmPerPixel);
    }

    std::vector<std::vector<core::Point>> out;
    for (const QPolygonF& polygon : path.toSubpathPolygons()) {
        std::vector<core::Point> contour;
        for (const QPointF& p : polygon) {
            const core::Point q{p.x() * mmPerPixel, -p.y() * mmPerPixel};  // to mm, y up
            if (contour.empty() || std::hypot(q.x - contour.back().x, q.y - contour.back().y) > 1e-9) {
                contour.push_back(q);
            }
        }
        if (contour.size() > 2 && std::hypot(contour.front().x - contour.back().x, contour.front().y - contour.back().y) < 1e-9) {
            contour.pop_back();  // closed already: don't repeat the start
        }
        if (contour.size() >= 3) {
            out.push_back(std::move(contour));
        }
    }
    return out;
}

void update(core::Text& text)
{
    text.shape = make(text);
}

QStringList families(const QString& search)
{
    QStringList out;
    for (const QString& family : QFontDatabase::families()) {
        if (QFontDatabase::isPrivateFamily(family)) {
            continue;
        }
        if (search.isEmpty() || family.contains(search, Qt::CaseInsensitive)) {
            out << family;
        }
    }
    out.sort(Qt::CaseInsensitive);
    out.removeDuplicates();
    return out;
}

QString defaultFamily()
{
    for (const char* preferred : {"Arial", "Segoe UI", "DejaVu Sans", "Liberation Sans", "Helvetica"}) {
        if (QFontDatabase::hasFamily(QString::fromLatin1(preferred))) {
            return QString::fromLatin1(preferred);
        }
    }
    return QFontDatabase::systemFont(QFontDatabase::GeneralFont).family();
}

}  // namespace textshapes
