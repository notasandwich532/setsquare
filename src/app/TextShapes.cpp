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
#include <set>

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

// --- minimum thickness, on a fine grid (see thicken; path booleans are far too slow for it) ---

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

// Fewer points on an open polyline, staying within `tolerance` of it (Douglas-Peucker).
QPolygonF simplifyOpen(const QPolygonF& line, double tolerance)
{
    const int n = static_cast<int>(line.size());
    if (n < 3) {
        return line;
    }
    std::vector<bool> keep(static_cast<size_t>(n), false);
    keep.front() = keep.back() = true;
    std::function<void(int, int)> run = [&](int a, int b) {
        const QPointF p = line[a], d = line[b] - line[a];
        const double len = std::hypot(d.x(), d.y());
        double worst = -1;
        int at = -1;
        for (int i = a + 1; i < b; ++i) {
            const QPointF r = line[i] - p;
            const double dist = len > 0 ? std::abs(d.x() * r.y() - d.y() * r.x()) / len : std::hypot(r.x(), r.y());
            if (dist > worst) {
                worst = dist;
                at = i;
            }
        }
        if (worst > tolerance && at > a) {
            keep[static_cast<size_t>(at)] = true;
            run(a, at);
            run(at, b);
        }
    };
    run(0, n - 1);
    QPolygonF out;
    for (int i = 0; i < n; ++i) {
        if (keep[static_cast<size_t>(i)]) {
            out << line[i];
        }
    }
    return out;
}

// Thins a binary shape to its 1-pixel skeleton, keeping it connected (Zhang-Suen). Only pixels
// on the current edge are looked at each pass, so it costs about the shape's area overall.
void thin(std::vector<std::uint8_t>& img, int w, int h)
{
    const int dx[8] = {0, 1, 1, 1, 0, -1, -1, -1};  // N, NE, E, SE, S, SW, W, NW (y down)
    const int dy[8] = {-1, -1, 0, 1, 1, 1, 0, -1};
    auto at = [&](int x, int y) -> int {
        return x < 0 || y < 0 || x >= w || y >= h ? 0 : img[static_cast<size_t>(y) * w + x];
    };
    auto onEdge = [&](int x, int y) {
        for (int k = 0; k < 8; k += 2) {
            if (!at(x + dx[k], y + dy[k])) {
                return true;
            }
        }
        return false;
    };
    std::vector<int> edge;
    std::vector<std::uint8_t> queued(img.size(), 0);
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            if (at(x, y) && onEdge(x, y)) {
                edge.push_back(y * w + x);
                queued[static_cast<size_t>(y) * w + x] = 1;
            }
        }
    }
    bool changed = true;
    while (changed) {
        changed = false;
        for (int step = 0; step < 2; ++step) {
            std::vector<int> doomed;
            for (int i : edge) {
                const int x = i % w, y = i / w;
                if (!img[static_cast<size_t>(i)]) {
                    continue;
                }
                int p[8];
                int b = 0;
                for (int k = 0; k < 8; ++k) {
                    p[k] = at(x + dx[k], y + dy[k]);
                    b += p[k];
                }
                int a = 0;
                for (int k = 0; k < 8; ++k) {
                    a += !p[k] && p[(k + 1) % 8];
                }
                if (b < 2 || b > 6 || a != 1) {
                    continue;
                }
                const bool ok = step == 0 ? (!(p[0] && p[2] && p[4]) && !(p[2] && p[4] && p[6]))
                                          : (!(p[0] && p[2] && p[6]) && !(p[0] && p[4] && p[6]));
                if (ok) {
                    doomed.push_back(i);
                }
            }
            if (doomed.empty()) {
                continue;
            }
            changed = true;
            for (int i : doomed) {
                img[static_cast<size_t>(i)] = 0;
            }
            // The new edge: what's left of the old one, and the neighbours of what went.
            std::vector<int> next;
            for (int i : edge) {
                queued[static_cast<size_t>(i)] = 0;
            }
            auto consider = [&](int i) {
                if (img[static_cast<size_t>(i)] && !queued[static_cast<size_t>(i)]) {
                    queued[static_cast<size_t>(i)] = 1;
                    next.push_back(i);
                }
            };
            for (int i : edge) {
                consider(i);
            }
            for (int i : doomed) {
                const int x = i % w, y = i / w;
                for (int k = 0; k < 8; ++k) {
                    const int nx = x + dx[k], ny = y + dy[k];
                    if (nx >= 0 && ny >= 0 && nx < w && ny < h) {
                        consider(ny * w + nx);
                    }
                }
            }
            edge = std::move(next);
        }
    }
}

// Thinning leaves 2-pixel staircases on diagonals, whose corner pixels have three neighbours and
// would look like forks. Removes every pixel that has three or more neighbours all connected to
// each other (8-connected: its Yokoi connectivity number is 1); real forks and ends stay.
void tidySkeleton(std::vector<std::uint8_t>& skel, int w, int h)
{
    // Ring from east, anticlockwise on screen: E, NE, N, NW, W, SW, S, SE (y down).
    const int dx[8] = {1, 1, 0, -1, -1, -1, 0, 1};
    const int dy[8] = {0, -1, -1, -1, 0, 1, 1, 1};
    auto at = [&](int x, int y) -> int {
        return x < 0 || y < 0 || x >= w || y >= h ? 0 : skel[static_cast<size_t>(y) * w + x];
    };
    for (bool changed = true; changed;) {
        changed = false;
        for (int y = 0; y < h; ++y) {
            for (int x = 0; x < w; ++x) {
                if (!at(x, y)) {
                    continue;
                }
                int v[9];
                int count = 0;
                for (int k = 0; k < 8; ++k) {
                    v[k] = at(x + dx[k], y + dy[k]);
                    count += v[k];
                }
                v[8] = v[0];
                if (count < 3) {
                    continue;
                }
                int pieces = 0;  // Yokoi (8-connectivity): sum over E, N, W, S of !a - !a !b !c
                for (int k = 0; k < 8; k += 2) {
                    const int a = !v[k], b = !v[k + 1], c = !v[(k + 2) % 8];
                    pieces += a - a * b * c;
                }
                if (pieces == 1) {
                    skel[static_cast<size_t>(y) * w + x] = 0;
                    changed = true;
                }
            }
        }
    }
}
// A skeleton as branches: chains of pixels between ends and forks (and loops with neither).
struct Branch
{
    std::vector<int> pixels;  // in order
    int ends[2] = {-1, -1};   // the end and fork pixels it runs between (-1 for a loop)
};

std::vector<Branch> branchesOf(const std::vector<std::uint8_t>& skel, int w, int h, std::vector<int>& degree)
{
    const int dx[8] = {0, 1, 1, 1, 0, -1, -1, -1};
    const int dy[8] = {-1, -1, 0, 1, 1, 1, 0, -1};
    auto on = [&](int x, int y) { return x >= 0 && y >= 0 && x < w && y < h && skel[static_cast<size_t>(y) * w + x]; };
    auto neighbours = [&](int i) {
        std::vector<int> out;
        const int x = i % w, y = i / w;
        for (int k = 0; k < 8; ++k) {
            if (on(x + dx[k], y + dy[k])) {
                out.push_back((y + dy[k]) * w + x + dx[k]);
            }
        }
        return out;
    };
    degree.assign(skel.size(), 0);
    std::vector<int> nodes;
    for (int i = 0; i < w * h; ++i) {
        if (skel[static_cast<size_t>(i)]) {
            degree[static_cast<size_t>(i)] = static_cast<int>(neighbours(i).size());
            if (degree[static_cast<size_t>(i)] != 2) {
                nodes.push_back(i);
            }
        }
    }
    auto isNode = [&](int i) { return degree[static_cast<size_t>(i)] != 2; };
    std::vector<std::uint8_t> used(skel.size(), 0);  // pixels inside a branch already taken
    std::set<std::pair<int, int>> started;           // (node, first step) already walked
    std::vector<Branch> out;
    auto walk = [&](int from, int first) {
        Branch b;
        b.ends[0] = from;
        b.pixels = {from};
        int prev = from, cur = first;
        while (true) {
            b.pixels.push_back(cur);
            if (isNode(cur) || cur == from) {
                b.ends[1] = cur;
                break;
            }
            used[static_cast<size_t>(cur)] = 1;
            int next = -1;
            for (int n : neighbours(cur)) {
                if (n != prev && (isNode(n) || !used[static_cast<size_t>(n)])) {
                    next = n;
                    if (isNode(n)) {
                        break;  // prefer arriving at the fork
                    }
                }
            }
            if (next < 0) {
                b.ends[1] = cur;
                break;
            }
            prev = cur;
            cur = next;
        }
        return b;
    };
    for (int node : nodes) {
        for (int n : neighbours(node)) {
            if ((isNode(n) && n < node) || used[static_cast<size_t>(n)] || !started.insert({node, n}).second) {
                continue;
            }
            Branch b = walk(node, n);
            if (isNode(b.ends[1])) {
                started.insert({b.ends[1], b.pixels[b.pixels.size() - 2]});  // (not walked back)
            }
            out.push_back(std::move(b));
        }
    }
    // Loops with no ends or forks (an "o"'s ring).
    for (int i = 0; i < w * h; ++i) {
        if (skel[static_cast<size_t>(i)] && !used[static_cast<size_t>(i)] && !isNode(i)) {
            const std::vector<int> n = neighbours(i);
            used[static_cast<size_t>(i)] = 1;
            Branch b = walk(i, n.front());
            b.ends[0] = b.ends[1] = -1;
            out.push_back(std::move(b));
        }
    }
    return out;
}

// Thickens the parts narrower than `width` to it, leaving the rest as it is: a stroke `width`
// wide runs along the centre line (skeleton) wherever the shape is thinner than that. Spurs of
// the skeleton running into the corners of thick strokes are pruned first, or their strokes would
// show as lumps; free ends are carried on to the tip, and branches meeting at a fork are joined
// with a round. The shape and strokes are drawn together on a fine grid and traced back into
// smooth outlines.
QPainterPath thicken(const QPainterPath& shape, double width)
{
    const QRectF box = shape.boundingRect();
    if (box.isEmpty() || !(width > 0)) {
        return shape;
    }
    // Fine enough for smooth letters (capital height ~350 px) and a well-resolved minimum, within a
    // pixel budget.
    double k = std::max(350.0 / (kPixelSize * 0.7), 8.0 / width);  // grid pixels per path unit
    const double margin = width + 2;
    const double area = (box.width() + 2 * margin) * (box.height() + 2 * margin);
    k = std::min(k, std::sqrt(6.0e6 / std::max(area, 1.0)));
    const int w = static_cast<int>(std::ceil((box.width() + 2 * margin) * k)) + 2;
    const int h = static_cast<int>(std::ceil((box.height() + 2 * margin) * k)) + 2;
    const QPointF origin = box.topLeft() - QPointF(margin, margin);
    const QTransform toGrid = QTransform::fromScale(k, k).translate(-origin.x(), -origin.y());

    QImage image(w, h, QImage::Format_Grayscale8);
    image.fill(0);
    QPainter painter(&image);
    painter.setRenderHint(QPainter::Antialiasing);
    painter.setPen(Qt::NoPen);
    painter.setBrush(Qt::white);
    painter.setTransform(toGrid);
    painter.drawPath(shape);

    std::vector<std::uint8_t> inside(static_cast<size_t>(w) * h), outside(inside.size());
    for (int y = 0; y < h; ++y) {
        const uchar* row = image.constScanLine(y);
        for (int x = 0; x < w; ++x) {
            inside[static_cast<size_t>(y) * w + x] = row[x] >= 128;
            outside[static_cast<size_t>(y) * w + x] = row[x] < 128;
        }
    }
    // How deep each pixel is (half the local thickness, on the centre line).
    std::vector<float> depth = distanceSquared(outside, w, h);
    for (float& d : depth) {
        d = std::sqrt(d);
    }
    const float r = float(width * k / 2);

    std::vector<std::uint8_t> skel = inside;
    thin(skel, w, h);
    tidySkeleton(skel, w, h);
    std::vector<int> degree;
    std::vector<Branch> branches = branchesOf(skel, w, h, degree);
    auto depthAt = [&](int i) { return depth[static_cast<size_t>(i)]; };

    // Prune spurs: branches from a fork to a dead end that run into a corner rather than along a
    // stroke. Going into a corner, the depth falls away steadily to nothing (about half the fork's
    // depth on average), and the spur is short for that depth even at an acute corner; along a
    // stroke it stays about as deep. Again until none go, as pruning can leave new ones.
    for (int pass = 0; pass < 4; ++pass) {
        // Which branches are spurs, judged all together: at a stroke's end the centre line can
        // fork into both corners of the cut, and both spurs go (pruning one at a time, the other
        // would no longer look like a spur and leave a stub).
        auto isSpur = [&](const Branch& b, int& forkOut) {
            for (int side = 0; side < 2; ++side) {
                const int fork = b.ends[side], end = b.ends[1 - side];
                if (fork < 0 || end < 0 || degree[static_cast<size_t>(fork)] < 3
                    || degree[static_cast<size_t>(end)] != 1) {
                    continue;
                }
                const double forkDepth = depthAt(fork);
                double mean = 0;
                for (int p : b.pixels) {
                    mean += depthAt(p);
                }
                mean /= double(b.pixels.size());
                const double length = double(b.pixels.size());
                if (length < 2.0 * forkDepth + 2.0 || (mean < 0.6 * forkDepth && length < 4.0 * forkDepth + 2.0)) {
                    forkOut = fork;
                    return true;
                }
            }
            return false;
        };
        std::map<int, int> keepers;  // fork -> branches there that aren't spurs
        std::vector<int> spurFork(branches.size(), -1);
        for (size_t i = 0; i < branches.size(); ++i) {
            int fork = -1;
            if (isSpur(branches[i], fork)) {
                spurFork[i] = fork;
            }
            for (int e : branches[i].ends) {
                if (e >= 0 && spurFork[i] != e) {
                    ++keepers[e];
                }
            }
        }
        std::vector<Branch> kept;
        for (size_t i = 0; i < branches.size(); ++i) {
            // A spur goes if a real stroke leaves its fork (not all of an "x" in one go).
            if (spurFork[i] >= 0 && keepers[spurFork[i]] >= 1) {
                continue;
            }
            kept.push_back(std::move(branches[i]));
        }
        const bool pruned = kept.size() != branches.size();
        branches = std::move(kept);
        if (!pruned) {
            break;
        }
        // Forks left with fewer branches are ends or bends now.
        std::map<int, int> count;
        for (const Branch& br : branches) {
            for (int e : br.ends) {
                if (e >= 0) {
                    ++count[e];
                }
            }
        }
        for (const auto& [node, c] : count) {
            if (degree[static_cast<size_t>(node)] >= 3) {
                degree[static_cast<size_t>(node)] = c;
            }
        }
    }    // Forks still joining branches after pruning (where a round joint goes).
    std::map<int, int> joins;
    for (const Branch& b : branches) {
        for (int e : b.ends) {
            if (e >= 0) {
                ++joins[e];
            }
        }
    }

    // The narrow stretches of each branch, as polylines on the grid.
    QPainterPath strokes;
    std::vector<QPointF> rounds;
    auto pointOf = [&](int i) { return QPointF(i % w + 0.5, i / w + 0.5); };
    for (const Branch& b : branches) {
        const size_t n = b.pixels.size();
        size_t i = 0;
        while (i < n) {
            if (depthAt(b.pixels[i]) >= r) {
                ++i;
                continue;
            }
            size_t j = i;
            while (j + 1 < n && depthAt(b.pixels[j + 1]) < r) {
                ++j;
            }
            QPolygonF run;
            for (size_t q = i; q <= j; ++q) {
                run << pointOf(b.pixels[q]);
            }
            // Smooth out the pixel staircase (a wide stroke shows every step), ends kept.
            {
                const int m = std::max(2, static_cast<int>(std::lround(r / 3)));
                QPolygonF smooth = run;
                for (int q = 1; q + 1 < run.size(); ++q) {
                    const int lo = std::max(0, q - m), hi = std::min(static_cast<int>(run.size()) - 1, q + m);
                    const int reach = std::min(q - lo, hi - q);  // symmetric, so ends don't drift
                    QPointF sum(0, 0);
                    for (int s = q - reach; s <= q + reach; ++s) {
                        sum += run[s];
                    }
                    smooth[q] = sum / double(2 * reach + 1);
                }
                run = smooth;
            }
            run = simplifyOpen(run, 0.5);
            // Where a run ends at the branch's end: a free end goes on to the tip (as far as it's
            // deep there), and a fork gets a round joint.
            auto extend = [&](bool atStart) {
                const size_t at = atStart ? i : j;
                const int end = atStart ? b.ends[0] : b.ends[1];
                if ((atStart && i != 0) || (!atStart && j != n - 1) || end < 0) {
                    return;
                }
                if (joins[end] >= 2) {
                    rounds.push_back(pointOf(b.pixels[at]));
                    return;
                }
                if (run.size() < 2) {
                    return;
                }
                const QPointF tip = atStart ? run.first() : run.last();
                const QPointF from = atStart ? run[std::min<qsizetype>(3, run.size() - 1)]
                                             : run[std::max<qsizetype>(0, run.size() - 4)];
                QPointF d = tip - from;
                const double len = std::hypot(d.x(), d.y());
                if (len <= 0) {
                    return;
                }
                const QPointF further = tip + d / len * double(depthAt(b.pixels[at]) + 1.0);
                if (atStart) {
                    run.prepend(further);
                }
                else {
                    run.append(further);
                }
            };
            extend(true);
            extend(false);
            if (run.size() >= 2) {
                strokes.addPolygon(run);
            }
            else {
                rounds.push_back(run.first());
            }
            i = j + 1;
        }
    }
    if (strokes.isEmpty() && rounds.empty()) {
        return shape;
    }

    // Draw the strokes over the shape, then trace the whole.
    painter.resetTransform();
    painter.setBrush(Qt::NoBrush);
    painter.setPen(QPen(Qt::white, 2 * double(r), Qt::SolidLine, Qt::FlatCap, Qt::RoundJoin));
    painter.drawPath(strokes);
    painter.setPen(Qt::NoPen);
    painter.setBrush(Qt::white);
    for (const QPointF& c : rounds) {
        painter.drawEllipse(c, double(r), double(r));
    }
    painter.end();

    std::vector<float> field(inside.size());
    for (int y = 0; y < h; ++y) {
        const uchar* row = image.constScanLine(y);
        for (int x = 0; x < w; ++x) {
            field[static_cast<size_t>(y) * w + x] = 127.5f - float(row[x]);  // negative inside
        }
    }
    QPainterPath out;
    out.setFillRule(Qt::WindingFill);
    const QTransform back = toGrid.inverted();
    for (const QPolygonF& loop : traceZero(field, w, h)) {
        out.addPolygon(back.map(simplify(loop, 0.25)));
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
