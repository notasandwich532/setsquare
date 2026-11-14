#include "TextShapes.h"

#include <core/Geometry.h>
#include <core/Solver.h>

#include <QDir>
#include <QFileInfo>
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
#include <array>
#include <functional>
#include <unordered_map>
#include <map>
#include <numbers>
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

// Thinning leaves 2-pixel staircases and little detours on diagonals, whose pixels would look like
// forks (or make tiny loops). Removes every pixel whose two or more neighbours are all connected
// to each other without it (8-connected: its Yokoi connectivity number is 1), so it's redundant;
// real forks, lines and their ends stay.
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
                if (count < 2) {
                    continue;  // (a line's end stays)
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

// --- putting a traced outline back together from the curves it was traced from ---

QPointF cubicAt(const QPointF* p, double t)
{
    const double u = 1 - t;
    return p[0] * (u * u * u) + p[1] * (3 * u * u * t) + p[2] * (3 * u * t * t) + p[3] * (t * t * t);
}

// The part of a cubic between t0 and t1 (t1 < t0: running backwards).
std::array<QPointF, 4> cubicPart(const QPointF* p, double t0, double t1)
{
    auto split = [](std::array<QPointF, 4> c, double t, bool keepEnd) {
        const QPointF a = c[0] + (c[1] - c[0]) * t, b = c[1] + (c[2] - c[1]) * t, d = c[2] + (c[3] - c[2]) * t;
        const QPointF ab = a + (b - a) * t, bd = b + (d - b) * t;
        const QPointF m = ab + (bd - ab) * t;
        return keepEnd ? std::array<QPointF, 4>{m, bd, d, c[3]} : std::array<QPointF, 4>{c[0], a, ab, m};
    };
    const bool backwards = t1 < t0;
    if (backwards) {
        std::swap(t0, t1);
    }
    std::array<QPointF, 4> c{p[0], p[1], p[2], p[3]};
    c = split(c, t1, false);  // [0, t1]
    if (t1 > 1e-12) {
        c = split(c, t0 / t1, true);  // [t0, t1]
    }
    if (backwards) {
        std::reverse(c.begin(), c.end());
    }
    return c;
}

// Outlines' lines and curves, found again by where they pass: for each subpath (all closed),
// arc length along it, and a spatial index of points along them.
class CurveSet
{
public:
    explicit CurveSet(double spacing)
        : m_spacing(spacing)
        , m_cell(spacing * 4)
    {}

    void add(const QPainterPath& path)
    {
        QPointF start, at;
        bool open = false;
        auto closeSub = [&] {
            if (open && QLineF(at, start).length() > 1e-9) {
                addSeg({at, (at * 2 + start) / 3, (at + start * 2) / 3, start}, false);
            }
            open = false;
        };
        for (int i = 0; i < path.elementCount(); ++i) {
            const QPainterPath::Element e = path.elementAt(i);
            if (e.isMoveTo()) {
                closeSub();
                m_subs.push_back({static_cast<int>(m_segs.size()), 0, 0.0});
                start = at = QPointF(e.x, e.y);
                open = true;
            }
            else if (e.isLineTo()) {
                const QPointF to(e.x, e.y);
                if (QLineF(at, to).length() > 1e-9) {
                    addSeg({at, (at * 2 + to) / 3, (at + to * 2) / 3, to}, false);
                }
                at = to;
            }
            else if (e.isCurveTo() && i + 2 < path.elementCount()) {
                const QPointF c1(e.x, e.y), c2(path.elementAt(i + 1).x, path.elementAt(i + 1).y),
                    to(path.elementAt(i + 2).x, path.elementAt(i + 2).y);
                addSeg({at, c1, c2, to}, true);
                at = to;
                i += 2;
            }
        }
        closeSub();
    }

    struct Hit
    {
        int sub = -1;
        double s = 0.0;  // arc length along it
        double distance = 1e300;
    };
    // The nearest point within `within` (on subpath `onlySub`, if that's 0 or more).
    Hit nearest(QPointF p, double within, int onlySub = -1) const
    {
        Hit best;
        const double reach = within + m_spacing;
        const int x0 = cellOf(p.x() - reach), x1 = cellOf(p.x() + reach);
        const int y0 = cellOf(p.y() - reach), y1 = cellOf(p.y() + reach);
        int bestSample = -1;
        double bestD = 1e300;
        for (int cx = x0; cx <= x1; ++cx) {
            for (int cy = y0; cy <= y1; ++cy) {
                const auto it = m_grid.find(key(cx, cy));
                if (it == m_grid.end()) {
                    continue;
                }
                for (int k : it->second) {
                    if (onlySub >= 0 && m_segs[static_cast<size_t>(m_samples[static_cast<size_t>(k)].seg)].sub != onlySub) {
                        continue;
                    }
                    const double d = QLineF(p, m_samples[static_cast<size_t>(k)].p).length();
                    if (d < bestD) {
                        bestD = d;
                        bestSample = k;
                    }
                }
            }
        }
        if (bestSample < 0) {
            return best;
        }
        // Refined along its segment, about the sample.
        const Sample& sm = m_samples[static_cast<size_t>(bestSample)];
        const Seg& g = m_segs[static_cast<size_t>(sm.seg)];
        double lo = std::max(0.0, sm.t - sm.dt), hi = std::min(1.0, sm.t + sm.dt);
        for (int i = 0; i < 30; ++i) {
            const double a = lo + (hi - lo) / 3, b = hi - (hi - lo) / 3;
            if (QLineF(p, cubicAt(g.p.data(), a)).length() < QLineF(p, cubicAt(g.p.data(), b)).length()) {
                hi = b;
            }
            else {
                lo = a;
            }
        }
        const double t = (lo + hi) / 2;
        best.distance = QLineF(p, cubicAt(g.p.data(), t)).length();
        if (best.distance > within) {
            return {};
        }
        best.sub = g.sub;
        best.s = sOf(g, t);
        return best;
    }

    double length(int sub) const { return m_subs[static_cast<size_t>(sub)].length; }
    QPointF at(int sub, double s) const
    {
        const auto [seg, t] = locate(sub, s);
        return cubicAt(m_segs[static_cast<size_t>(seg)].p.data(), t);
    }

    // Appends to `out` (which is at the piece's start) the subpath from s0 to s1, forwards or
    // backwards (all the way round if they're the same and `whole`).
    void appendPiece(QPainterPath& out, int sub, double s0, double s1, bool forward, bool whole = false) const
    {
        const double L = length(sub);
        auto norm = [&](double s) { return std::fmod(std::fmod(s, L) + L, L); };
        s0 = norm(s0);
        s1 = norm(s1);
        // As forward pieces of [a, b], wrapping past the start.
        double a = forward ? s0 : s1, b = forward ? s1 : s0;
        std::vector<std::pair<double, double>> spans;
        if (b > a || (b == a && !whole)) {
            spans.push_back({a, b});
        }
        else {
            spans.push_back({a, L});
            spans.push_back({0.0, b});
        }
        struct Piece
        {
            int seg;
            double t0, t1;
        };
        std::vector<Piece> pieces;
        const SubInfo& info = m_subs[static_cast<size_t>(sub)];
        for (const auto& [from, to] : spans) {
            for (int i = info.first; i < info.first + info.count; ++i) {
                const Seg& g = m_segs[static_cast<size_t>(i)];
                const double lo = std::max(from, g.s0), hi = std::min(to, g.s1);
                if (hi - lo > 1e-9) {
                    pieces.push_back({i, tOf(g, lo), tOf(g, hi)});
                }
            }
        }
        if (!forward) {
            std::reverse(pieces.begin(), pieces.end());
            for (Piece& pc : pieces) {
                std::swap(pc.t0, pc.t1);
            }
        }
        for (const Piece& pc : pieces) {
            const Seg& g = m_segs[static_cast<size_t>(pc.seg)];
            const std::array<QPointF, 4> c = cubicPart(g.p.data(), pc.t0, pc.t1);
            if (g.cubic) {
                out.cubicTo(c[1], c[2], c[3]);
            }
            else {
                out.lineTo(c[3]);
            }
        }
    }

private:
    struct Seg
    {
        std::array<QPointF, 4> p;
        bool cubic = false;
        int sub = 0;
        double s0 = 0.0, s1 = 0.0;
        std::array<double, 17> st{};  // arc length (from s0) at t = 0, 1/16, ... 1
    };
    struct SubInfo
    {
        int first = 0, count = 0;
        double length = 0.0;
    };
    struct Sample
    {
        QPointF p;
        int seg = 0;
        double t = 0.0, dt = 0.0;
    };

    void addSeg(std::array<QPointF, 4> p, bool cubic)
    {
        SubInfo& info = m_subs.back();
        Seg g{p, cubic, static_cast<int>(m_subs.size()) - 1, info.length, info.length, {}};
        QPointF prev = p[0];
        for (int i = 1; i <= 16; ++i) {
            const QPointF q = cubicAt(p.data(), i / 16.0);
            g.st[static_cast<size_t>(i)] = g.st[static_cast<size_t>(i) - 1] + QLineF(prev, q).length();
            prev = q;
        }
        g.s1 = g.s0 + g.st[16];
        info.length = g.s1;
        ++info.count;
        const int index = static_cast<int>(m_segs.size());
        m_segs.push_back(g);
        // Points along it, about `spacing` apart, indexed by where they are.
        const int n = std::max(2, static_cast<int>(std::ceil(g.st[16] / m_spacing)));
        for (int i = 0; i <= n; ++i) {
            const double t = double(i) / n;
            m_samples.push_back({cubicAt(p.data(), t), index, t, 1.0 / n});
            const QPointF& q = m_samples.back().p;
            m_grid[key(cellOf(q.x()), cellOf(q.y()))].push_back(static_cast<int>(m_samples.size()) - 1);
        }
    }
    double sOf(const Seg& g, double t) const
    {
        const double f = std::clamp(t, 0.0, 1.0) * 16;
        const int i = std::min(15, static_cast<int>(f));
        return g.s0 + g.st[static_cast<size_t>(i)] + (g.st[static_cast<size_t>(i) + 1] - g.st[static_cast<size_t>(i)]) * (f - i);
    }
    double tOf(const Seg& g, double s) const
    {
        const double local = std::clamp(s - g.s0, 0.0, g.st[16]);
        for (int i = 0; i < 16; ++i) {
            if (local <= g.st[static_cast<size_t>(i) + 1] || i == 15) {
                const double span = g.st[static_cast<size_t>(i) + 1] - g.st[static_cast<size_t>(i)];
                return (i + (span > 0 ? (local - g.st[static_cast<size_t>(i)]) / span : 0.0)) / 16.0;
            }
        }
        return 1.0;
    }
    std::pair<int, double> locate(int sub, double s) const
    {
        const SubInfo& info = m_subs[static_cast<size_t>(sub)];
        const double L = info.length;
        s = std::fmod(std::fmod(s, L) + L, L);
        for (int i = info.first; i < info.first + info.count; ++i) {
            const Seg& g = m_segs[static_cast<size_t>(i)];
            if (s <= g.s1 || i == info.first + info.count - 1) {
                return {i, tOf(g, s)};
            }
        }
        return {info.first, 0.0};
    }
    int cellOf(double v) const { return static_cast<int>(std::floor(v / m_cell)); }
    static qint64 key(int x, int y) { return (static_cast<qint64>(x) << 32) ^ static_cast<quint32>(y); }

    double m_spacing, m_cell;
    std::vector<Seg> m_segs;
    std::vector<SubInfo> m_subs;
    std::vector<Sample> m_samples;
    std::unordered_map<qint64, std::vector<int>> m_grid;
};

// A traced loop (points about a grid pixel apart) as the curves it lies along: each stretch within
// `tolerance` of one of the outlines in `sets` (the first preferred) is that outline's own lines
// and curves between where the stretch starts and ends; the rest, simplified lines.
void spliceLoop(QPainterPath& out, const std::vector<QPointF>& loop, const std::vector<const CurveSet*>& sets,
                double tolerance, double jump)
{
    const size_t n = loop.size();
    if (n < 3) {
        return;
    }
    struct Label
    {
        int set = -1, sub = -1;
        double s = 0.0;
    };
    std::vector<Label> labels(n);
    // Each on the outline the one before it was on, while it's still near enough (where two run
    // close together, not flitting between them); else the nearest, the first set's first.
    // (Twice round, so the first point has one before it too.)
    for (size_t pass = 0; pass < 2; ++pass) {
        for (size_t i = 0; i < n; ++i) {
            const Label& before = labels[(i + n - 1) % n];
            if (before.sub >= 0) {
                const CurveSet::Hit h = sets[static_cast<size_t>(before.set)]->nearest(loop[i], tolerance, before.sub);
                if (h.sub >= 0) {
                    labels[i] = {before.set, h.sub, h.s};
                    continue;
                }
            }
            labels[i] = {};
            for (size_t k = 0; k < sets.size(); ++k) {
                const CurveSet::Hit h = sets[k]->nearest(loop[i], tolerance);
                if (h.sub >= 0) {
                    labels[i] = {static_cast<int>(k), h.sub, h.s};
                    break;
                }
            }
        }
    }
    // Whether i and the point after it are on the same outline, in step.
    auto stepOf = [&](size_t i) {
        const Label& a = labels[i];
        const Label& b = labels[(i + 1) % n];
        if (a.sub < 0 || a.set != b.set || a.sub != b.sub) {
            return std::optional<double>();
        }
        const double L = sets[static_cast<size_t>(a.set)]->length(a.sub);
        double d = b.s - a.s;
        if (d > L / 2) {
            d -= L;
        }
        else if (d < -L / 2) {
            d += L;
        }
        return std::abs(d) <= jump ? std::optional<double>(d) : std::optional<double>();
    };
    // Start where a stretch starts (if the whole loop is one, it's that outline whole).
    size_t first = n;
    for (size_t i = 0; i < n; ++i) {
        if (!stepOf((i + n - 1) % n)) {
            first = i;
            break;
        }
    }
    bool started = false;
    auto goTo = [&](QPointF p) {
        if (!started) {
            out.moveTo(p);
            started = true;
        }
        else {
            out.lineTo(p);
        }
    };
    if (first == n) {
        double total = 0.0;
        for (size_t i = 0; i < n; ++i) {
            total += *stepOf(i);
        }
        const Label& l = labels[0];
        const CurveSet& set = *sets[static_cast<size_t>(l.set)];
        goTo(set.at(l.sub, l.s));
        set.appendPiece(out, l.sub, l.s, l.s, total > 0, true);
        out.closeSubpath();
        return;
    }
    // Lines through points that lie on nothing, simplified (Ramer-Douglas-Peucker).
    std::vector<QPointF> loose;
    auto flushLoose = [&] {
        if (loose.empty()) {
            return;
        }
        std::vector<char> keep(loose.size(), 0);
        keep.front() = keep.back() = 1;
        std::function<void(size_t, size_t)> rdp = [&](size_t a, size_t b) {
            double far = 0;
            size_t at = a;
            for (size_t i = a + 1; i < b; ++i) {
                const QLineF ab(loose[a], loose[b]);
                const double len = ab.length();
                const QPointF d = loose[i] - loose[a];
                const double off = len > 0 ? std::abs(d.x() * (loose[b] - loose[a]).y() - d.y() * (loose[b] - loose[a]).x()) / len
                                           : QLineF(loose[a], loose[i]).length();
                if (off > far) {
                    far = off;
                    at = i;
                }
            }
            if (far > tolerance / 2) {
                keep[at] = 1;
                rdp(a, at);
                rdp(at, b);
            }
        };
        rdp(0, loose.size() - 1);
        for (size_t i = 0; i < loose.size(); ++i) {
            if (keep[i]) {
                goTo(loose[i]);
            }
        }
        loose.clear();
    };
    size_t i = first;
    for (size_t done = 0; done < n;) {
        // The stretch from i: as far as it goes in step on one outline.
        size_t len = 1;
        double total = 0.0;
        while (len < n - done) {
            const auto step = stepOf((i + len - 1) % n);
            if (!step) {
                break;
            }
            total += *step;
            ++len;
        }
        const Label& a = labels[i];
        if (a.sub >= 0 && len >= 3) {
            flushLoose();
            const Label& b = labels[(i + len - 1) % n];
            const CurveSet& set = *sets[static_cast<size_t>(a.set)];
            goTo(set.at(a.sub, a.s));
            set.appendPiece(out, a.sub, a.s, b.s, total > 0);
        }
        else {
            for (size_t k = 0; k < len; ++k) {
                loose.push_back(loop[(i + k) % n]);
            }
        }
        i = (i + len) % n;
        done += len;
    }
    flushLoose();
    out.closeSubpath();
}

// A smooth curve through points (Catmull-Rom, as cubics), on from where `path` is (its first
// point); `closed`: round to the first again.
void smoothThrough(QPainterPath& path, const std::vector<QPointF>& p, bool closed)
{
    const size_t m = p.size();
    auto at = [&](std::ptrdiff_t i) {
        if (closed) {
            return p[static_cast<size_t>((i % static_cast<std::ptrdiff_t>(m) + static_cast<std::ptrdiff_t>(m)) % static_cast<std::ptrdiff_t>(m))];
        }
        return p[static_cast<size_t>(std::clamp<std::ptrdiff_t>(i, 0, static_cast<std::ptrdiff_t>(m) - 1))];
    };
    const std::ptrdiff_t pieces = static_cast<std::ptrdiff_t>(closed ? m : m - 1);
    for (std::ptrdiff_t i = 0; i < pieces; ++i) {
        path.cubicTo(at(i) + (at(i + 1) - at(i - 1)) / 6.0, at(i + 1) - (at(i + 2) - at(i)) / 6.0, at(i + 1));
    }
}

// The outline of a stroke `half` either side of a smooth centre line (points about evenly
// spaced): its two sides as smooth curves, joined straight across at its ends (a ring: two loops,
// round opposite ways).
QPainterPath strokeOutline(const std::vector<QPointF>& c, double half, bool closed)
{
    QPainterPath out;
    out.setFillRule(Qt::WindingFill);
    const size_t m = c.size();
    if (m < 2 || (closed && m < 3)) {
        return out;
    }
    std::vector<QPointF> left(m), right(m);
    for (size_t i = 0; i < m; ++i) {
        const QPointF before = closed ? c[(i + m - 1) % m] : c[i == 0 ? 0 : i - 1];
        const QPointF after = closed ? c[(i + 1) % m] : c[std::min(m - 1, i + 1)];
        QPointF t = after - before;
        const double len = std::hypot(t.x(), t.y());
        t = len > 0 ? t / len : QPointF(1, 0);
        const QPointF n(-t.y(), t.x());
        left[i] = c[i] + n * half;
        right[i] = c[i] - n * half;
    }
    std::reverse(right.begin(), right.end());
    if (closed) {
        out.moveTo(left.front());
        smoothThrough(out, left, true);
        out.closeSubpath();
        out.moveTo(right.front());
        smoothThrough(out, right, true);
        out.closeSubpath();
        return out;
    }
    out.moveTo(left.front());
    smoothThrough(out, left, false);
    out.lineTo(right.front());  // (flat across the end)
    smoothThrough(out, right, false);
    out.closeSubpath();  // (and across the start)
    return out;
}

// Thickens the parts narrower than `width` to it, leaving the rest as it is: the shape, together
// with a disc `width` across centred on every point of its centre line (skeleton). Where the
// shape is thick enough those discs are inside it anyway; where it's thin they make it exactly
// that thick, carrying straight on through junctions with thick strokes (no dip on top of a T)
// and rounding joints between thin ones. The skeleton's spurs into the corners of thick strokes
// are pruned first (their discs would be lumps), and free ends are pulled back so a thin tip is
// thickened to its end and no further. Worked out on a fine grid: the thin parts' centre lines,
// smoothed into curves, become strokes exactly `width` wide; the result is traced, then put back
// together from the curves themselves: the letters' own (`original`: as the font has them) where
// they're still the edge, the strokes' where those are.
QPainterPath thicken(const QPainterPath& shape, double width, const QPainterPath& original)
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

    // Prune spurs: branches from a fork to a dead end that run into a corner of a thick stroke.
    // Along a spur the depth rises from nothing at the tip at the sine of half the corner's angle:
    // steeply into a corner (a 90 degree one: 0.7), gently along a sharp prong such as a serif's
    // tip (30 degrees: 0.26). Corners (60 degrees and over) go; prongs are narrow parts, and stay
    // to be thickened. Judged all together (at a stroke's cut end, both corners' spurs go), and
    // again until none go, as pruning can leave new ones.
    for (int pass = 0; pass < 4; ++pass) {
        auto isSpur = [&](const Branch& b, int& forkOut) {
            for (int side = 0; side < 2; ++side) {
                const int fork = b.ends[side], end = b.ends[1 - side];
                if (fork < 0 || end < 0 || degree[static_cast<size_t>(fork)] < 3
                    || degree[static_cast<size_t>(end)] != 1) {
                    continue;
                }
                // The rise near the tip (the first half-width of it): further in, the depth
                // depends on what the spur runs into, not on the corner.
                const size_t steps = std::min(b.pixels.size() - 1, std::max<size_t>(3, static_cast<size_t>(r)));
                const int tipPixel = side == 0 ? b.pixels.back() : b.pixels.front();
                const int inPixel = side == 0 ? b.pixels[b.pixels.size() - 1 - steps] : b.pixels[steps];
                const double rise = depthAt(inPixel) - depthAt(tipPixel);
                if (steps == 0 || rise >= 0.5 * double(steps) - 1.0) {
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
    }
    // Nothing narrow anywhere: leave the letters exactly as they are.
    const bool anyNarrow = std::ranges::any_of(branches, [&](const Branch& b) {
        return std::ranges::any_of(b.pixels, [&](int p) { return depthAt(p) < r; });
    });
    if (!anyNarrow) {
        return shape;
    }
    // Branch ends that are free (not joined to another branch).
    std::map<int, int> joins;
    for (const Branch& b : branches) {
        for (int e : b.ends) {
            if (e >= 0) {
                ++joins[e];
            }
        }
    }
    // Strokes running straight on through a fork (a stem past where an arm leaves it) as one line:
    // the fork's point is pulled aside towards the arm, so pieces ending there wouldn't line up.
    // At each fork, the two branches leaving it most nearly opposite ways (within 35 degrees of
    // straight on) join; the others still end there.
    {
        auto pointOf = [&](int i) { return QPointF(i % w + 0.5, i / w + 0.5); };
        auto direction = [&](const Branch& br, bool fromFront) {
            const size_t nn = br.pixels.size();
            const size_t along = std::min(nn - 1, static_cast<size_t>(std::max(3.0f, 2 * r)));
            const QPointF a = pointOf(fromFront ? br.pixels.front() : br.pixels.back());
            const QPointF z = pointOf(fromFront ? br.pixels[along] : br.pixels[nn - 1 - along]);
            const QPointF d = z - a;
            const double len = std::hypot(d.x(), d.y());
            return len > 0 ? d / len : QPointF();
        };
        for (bool merged = true; merged;) {
            merged = false;
            std::map<int, std::vector<std::pair<size_t, bool>>> at;  // fork -> (branch, which end)
            for (size_t i = 0; i < branches.size(); ++i) {
                const Branch& br = branches[i];
                if (br.ends[0] < 0 || br.ends[0] == br.ends[1] || br.pixels.size() < 3) {
                    continue;  // (loops)
                }
                for (int side = 0; side < 2; ++side) {
                    if (degree[static_cast<size_t>(br.ends[side])] >= 3) {
                        at[br.ends[side]].push_back({i, side == 0});
                    }
                }
            }
            for (const auto& [fork, ends] : at) {
                double best = std::cos(145.0 * 3.141592653589793 / 180);
                std::pair<size_t, size_t> pair{0, 0};
                bool found = false;
                for (size_t x = 0; x < ends.size(); ++x) {
                    for (size_t y = x + 1; y < ends.size(); ++y) {
                        if (ends[x].first == ends[y].first) {
                            continue;
                        }
                        const double c = QPointF::dotProduct(direction(branches[ends[x].first], ends[x].second),
                                                             direction(branches[ends[y].first], ends[y].second));
                        if (c < best) {
                            best = c;
                            pair = {x, y};
                            found = true;
                        }
                    }
                }
                if (!found) {
                    continue;
                }
                // The first branch run into the fork, then the second out of it.
                const auto [ia, aFront] = ends[pair.first];
                const auto [ib, bFront] = ends[pair.second];
                Branch joined;
                std::vector<int> pa = branches[ia].pixels, pb = branches[ib].pixels;
                if (aFront) {
                    std::reverse(pa.begin(), pa.end());  // (to end at the fork)
                }
                if (!bFront) {
                    std::reverse(pb.begin(), pb.end());  // (to start at it)
                }
                joined.pixels = pa;
                joined.pixels.insert(joined.pixels.end(), pb.begin() + 1, pb.end());
                joined.ends[0] = aFront ? branches[ia].ends[1] : branches[ia].ends[0];
                joined.ends[1] = bFront ? branches[ib].ends[1] : branches[ib].ends[0];
                std::vector<Branch> next;
                for (size_t i = 0; i < branches.size(); ++i) {
                    if (i != ia && i != ib) {
                        next.push_back(std::move(branches[i]));
                    }
                }
                next.push_back(std::move(joined));
                branches = std::move(next);
                --degree[static_cast<size_t>(fork)];  // (it isn't an end of those two any more)
                --degree[static_cast<size_t>(fork)];
                merged = true;
                break;  // (indices have changed: look again)
            }
        }
    }
    // The thin parts' centre lines, smoothed into curves, each made into a stroke `width` wide:
    // flat where a free end is (pulled back by however far it'd reach past the tip, so a thin tip
    // is thickened right to its end and no further), round where it meets another thin stroke.
    const QTransform toPath = toGrid.inverted();
    std::vector<QPainterPath> strokes;  // (path coordinates)
    auto pointOf = [&](int i) { return QPointF(i % w + 0.5, i / w + 0.5); };
    auto roundEnd = [&](QPointF gridPoint) {
        QPainterPath disc;
        disc.addEllipse(toPath.map(gridPoint), width / 2, width / 2);
        strokes.push_back(disc);
    };
    for (const Branch& b : branches) {
        const size_t n = b.pixels.size();
        auto trim = [&](int end, int pixel) -> size_t {
            if (end < 0 || joins[end] >= 2) {
                return 0;
            }
            return static_cast<size_t>(std::max(0.0f, r - depthAt(pixel)));
        };
        size_t front = trim(b.ends[0], b.pixels.front());
        size_t back = trim(b.ends[1], b.pixels.back());
        if (front + back >= n) {
            // Too short to pull back that far: a stub off a junction keeps its last disc there
            // (and its flat end), a lone stroke its middle (flat both ways).
            if (front > 0 && back > 0) {
                front = n / 2;
                back = n - 1 - front;
            }
            else if (front > 0) {
                front = n - 1;
            }
            else {
                back = n - 1;
            }
        }
        // Its centre line, from where it's pulled back to at each end (a loop: all of it once).
        const bool ring = b.ends[0] < 0;
        std::vector<QPointF> pts;
        std::vector<float> depths;
        const size_t stop = ring ? n - 1 : n - back;  // (a loop's last pixel is its first again)
        for (size_t q = front; q < stop; ++q) {
            pts.push_back(pointOf(b.pixels[q]));
            depths.push_back(depthAt(b.pixels[q]));
        }
        if (pts.size() < 2 || std::ranges::none_of(depths, [&](float d) { return d < r; })) {
            continue;  // (thick enough all along)
        }
        const size_t m = pts.size();
        auto wrap = [&](std::ptrdiff_t i) {
            return static_cast<size_t>((i % static_cast<std::ptrdiff_t>(m) + static_cast<std::ptrdiff_t>(m)) % static_cast<std::ptrdiff_t>(m));
        };
        // Its corners: where it turns sharply (an L's, a V's), as against curving. Across a width
        // either side it turns a lot, and nearly as much across half that (a curve turns half as
        // much over half the length). The smoothing doesn't cross them; the strokes meet round there.
        const std::ptrdiff_t span = std::max<std::ptrdiff_t>(3, static_cast<std::ptrdiff_t>(r));
        auto turn = [&](size_t i, std::ptrdiff_t over) {
            const std::ptrdiff_t at = static_cast<std::ptrdiff_t>(i);
            if (!ring && (at - over < 0 || at + over >= static_cast<std::ptrdiff_t>(m))) {
                return 0.0;
            }
            const QPointF in = pts[i] - pts[wrap(at - over)], out = pts[wrap(at + over)] - pts[i];
            const double li = std::hypot(in.x(), in.y()), lo = std::hypot(out.x(), out.y());
            if (li <= 0 || lo <= 0) {
                return 0.0;
            }
            return std::acos(std::clamp(QPointF::dotProduct(in, out) / (li * lo), -1.0, 1.0));
        };
        std::vector<double> turning(m);
        for (size_t i = 0; i < m; ++i) {
            turning[i] = turn(i, span);
        }
        std::vector<size_t> corners;
        for (size_t i = 0; i < m; ++i) {
            const double t = turning[i];
            if (t < 0.9 || turn(i, std::max<std::ptrdiff_t>(2, span / 2)) < 0.8 * t) {
                continue;
            }
            bool peak = true;  // (the sharpest point of the bend)
            for (std::ptrdiff_t d = -span; d <= span && peak; ++d) {
                const std::ptrdiff_t j = static_cast<std::ptrdiff_t>(i) + d;
                if (d == 0 || (!ring && (j < 0 || j >= static_cast<std::ptrdiff_t>(m)))) {
                    continue;
                }
                const double other = turning[wrap(j)];
                peak = other < t || (other == t && d > 0);
            }
            if (peak) {
                corners.push_back(i);
            }
        }
        // A ring: from a corner round to it again (or, without one, a closed curve).
        if (ring && !corners.empty()) {
            const std::ptrdiff_t start = static_cast<std::ptrdiff_t>(corners.front());
            std::rotate(pts.begin(), pts.begin() + start, pts.end());
            std::rotate(depths.begin(), depths.begin() + start, depths.end());
            for (size_t& c : corners) {
                c = wrap(static_cast<std::ptrdiff_t>(c) - start);
            }
            pts.push_back(pts.front());
            depths.push_back(depths.front());
        }
        const bool closed = ring && corners.empty();
        const size_t count = pts.size();
        const std::ptrdiff_t reach = std::max<std::ptrdiff_t>(3, static_cast<std::ptrdiff_t>(2 * r));
        const size_t into = static_cast<size_t>(std::ceil(r));
        // Smoothed, a stretch at a time: each point where a parabola fitted to its neighbours (a
        // width's worth either side, nearer ones counting more) puts it. Wobbles shorter than that
        // go (the pixels' steps, and a thin stroke's centre wandering as its width does), while
        // curves keep their shape.
        auto smoothed = [&](const std::vector<QPointF>& p, bool wraps) {
            const size_t len = p.size();
            std::vector<QPointF> out = p;
            for (size_t i = 0; i < len; ++i) {
                double s[5] = {0, 0, 0, 0, 0}, sx[3] = {0, 0, 0}, sy[3] = {0, 0, 0};
                for (std::ptrdiff_t d = -reach; d <= reach; ++d) {
                    std::ptrdiff_t j = static_cast<std::ptrdiff_t>(i) + d;
                    if (wraps) {
                        j = (j % static_cast<std::ptrdiff_t>(len) + static_cast<std::ptrdiff_t>(len)) % static_cast<std::ptrdiff_t>(len);
                    }
                    else if (j < 0 || j >= static_cast<std::ptrdiff_t>(len)) {
                        continue;
                    }
                    const double u = double(d) / double(reach + 1);
                    double tp = std::pow(1 - std::abs(u * u * u), 3);  // (tricube)
                    for (int e = 0; e < 5; ++e) {
                        s[e] += tp;
                        if (e < 3) {
                            sx[e] += tp * p[static_cast<size_t>(j)].x();
                            sy[e] += tp * p[static_cast<size_t>(j)].y();
                        }
                        tp *= double(d);
                    }
                }
                // [s0 s1 s2; s1 s2 s3; s2 s3 s4] (a b c) = rhs, for a: the fit at the point itself.
                auto solveA = [&](const double* rhs) {
                    const double det = s[0] * (s[2] * s[4] - s[3] * s[3]) - s[1] * (s[1] * s[4] - s[3] * s[2])
                                     + s[2] * (s[1] * s[3] - s[2] * s[2]);
                    if (std::abs(det) < 1e-9) {
                        return rhs[0] / s[0];
                    }
                    return (rhs[0] * (s[2] * s[4] - s[3] * s[3]) - s[1] * (rhs[1] * s[4] - s[3] * rhs[2])
                            + s[2] * (rhs[1] * s[3] - s[2] * rhs[2]))
                         / det;
                };
                out[i] = QPointF(solveA(sx), solveA(sy));
            }
            return out;
        };
        const size_t step = std::max<size_t>(3, static_cast<size_t>(r / 2));
        auto toCurve = [&](const std::vector<QPointF>& line) {
            std::vector<QPointF> q;
            for (size_t i = 0; i < line.size(); i += step) {
                q.push_back(toPath.map(line[i]));
            }
            if (QLineF(q.back(), toPath.map(line.back())).length() > 1e-9) {
                q.push_back(toPath.map(line.back()));
            }
            return q;
        };
        if (closed) {
            pts = smoothed(pts, true);
            // Thin all the way round: one closed stroke.
            if (std::ranges::all_of(depths, [&](float d) { return d < r; })) {
                std::vector<QPointF> q;
                for (size_t i = 0; i < count; i += step) {
                    q.push_back(toPath.map(pts[i]));
                }
                if (q.size() >= 3) {
                    strokes.push_back(strokeOutline(q, width / 2, true));
                }
                continue;
            }
            // Partly thick: as a line from its thickest point round to it again.
            const std::ptrdiff_t thickest = std::ranges::max_element(depths) - depths.begin();
            std::rotate(pts.begin(), pts.begin() + thickest, pts.end());
            std::rotate(depths.begin(), depths.begin() + thickest, depths.end());
            pts.push_back(pts.front());
            depths.push_back(depths.front());
        }
        // The pieces between corners (and the chain's own ends), each smoothed on its own.
        struct Piece
        {
            std::vector<QPointF> p;
            std::vector<float> depth;
        };
        std::vector<size_t> cuts{0};
        if (!closed) {
            for (size_t c : corners) {
                if (c > cuts.back() + 1 && c + 2 < pts.size()) {
                    cuts.push_back(c);
                }
            }
        }
        cuts.push_back(pts.size() - 1);
        std::vector<Piece> pieces;
        for (size_t c = 0; c + 1 < cuts.size(); ++c) {
            Piece pc;
            pc.p.assign(pts.begin() + static_cast<std::ptrdiff_t>(cuts[c]), pts.begin() + static_cast<std::ptrdiff_t>(cuts[c + 1]) + 1);
            pc.depth.assign(depths.begin() + static_cast<std::ptrdiff_t>(cuts[c]), depths.begin() + static_cast<std::ptrdiff_t>(cuts[c + 1]) + 1);
            const std::vector<QPointF> raw = pc.p;
            pc.p = smoothed(pc.p, false);
            // Where the chain joins other strokes (not a corner of its own), it starts from the
            // very point it shares with them, easing into the smoothed line over a width, so
            // strokes running on through a join stay in line.
            auto pin = [&](bool atStart) {
                const size_t len = pc.p.size();
                for (std::ptrdiff_t d = 0; d <= reach; ++d) {
                    if (d >= static_cast<std::ptrdiff_t>(len)) {
                        break;
                    }
                    const size_t k2 = atStart ? static_cast<size_t>(d) : len - 1 - static_cast<size_t>(d);
                    const double t = double(d) / double(reach);
                    const double keep = t * t * (3 - 2 * t);
                    pc.p[k2] = raw[k2] + (pc.p[k2] - raw[k2]) * keep;
                }
            };
            auto joinsOthers = [&](int end, bool pulledBack) { return end >= 0 && !pulledBack && joins[end] >= 2; };
            if (!ring && c == 0 && joinsOthers(b.ends[0], front > 0)) {
                pin(true);
            }
            if (!ring && c + 2 == cuts.size() && joinsOthers(b.ends[1], back > 0)) {
                pin(false);
            }
            pieces.push_back(std::move(pc));
        }
        // Corners: the pieces either side meet where their lines do, and each runs straight on past
        // there by half the width (so an L's outer corner is square, as the letter's is).
        auto endDirection = [&](const std::vector<QPointF>& p, bool atEnd) {
            const size_t backBy = std::min(p.size() - 1, into);
            const QPointF d = atEnd ? p.back() - p[p.size() - 1 - backBy] : p.front() - p[backBy];
            const double len = std::hypot(d.x(), d.y());
            return len > 0 ? d / len : QPointF();
        };
        auto joinCorner = [&](Piece& in, Piece& out) {
            const QPointF a = in.p.back(), b2 = out.p.front();
            const QPointF da = endDirection(in.p, true), db = endDirection(out.p, false);  // (both pointing away)
            QPointF meet = (a + b2) / 2;
            const double cross = da.x() * db.y() - da.y() * db.x();
            if (std::abs(cross) > 0.05) {
                // a + s da = b2 + t db
                const QPointF w0 = b2 - a;
                const double sAlong = (w0.x() * db.y() - w0.y() * db.x()) / cross;
                const QPointF at = a + da * sAlong;
                if (sAlong > -r && sAlong < 3 * r && QLineF(at, b2).length() < 3 * r) {
                    meet = at;
                }
            }
            // Mitred: past the meeting point by just enough for the sides to meet in a point (half
            // the width times the tangent of half the turn: as far again for a right angle, less
            // for a gentler one); a very sharp one (that'd reach far) meets round instead.
            const double turnAngle = std::acos(std::clamp(-QPointF::dotProduct(da, db), -1.0, 1.0));
            const double past = r * std::tan(turnAngle / 2);
            const float depthThere = std::min(in.depth.back(), out.depth.front());
            in.p.back() = meet;
            out.p.front() = meet;
            if (past <= 2 * r) {
                in.p.push_back(meet + da * past);
                in.depth.push_back(depthThere);
                out.p.insert(out.p.begin(), meet + db * past);
                out.depth.insert(out.depth.begin(), depthThere);
            }
            else if (depthThere < r) {
                roundEnd(meet);
            }
        };
        for (size_t c = 0; c + 1 < pieces.size(); ++c) {
            joinCorner(pieces[c], pieces[c + 1]);
        }
        if (ring && !closed && pieces.size() >= 2) {
            joinCorner(pieces.back(), pieces.front());  // (round to the first corner again)
        }
        else if (ring && !closed && pieces.size() == 1) {
            joinCorner(pieces.front(), pieces.front());
        }
        // Each piece's thin stretches, each on into the thick part either side by half the width
        // (where its ends are buried).
        for (size_t c = 0; c < pieces.size(); ++c) {
            const Piece& pc = pieces[c];
            const size_t last = pc.p.size() - 1;
            std::vector<std::pair<size_t, size_t>> stretches;
            for (size_t i = 0; i <= last;) {
                if (pc.depth[i] >= r) {
                    ++i;
                    continue;
                }
                size_t j = i;
                while (j + 1 <= last && pc.depth[j + 1] < r) {
                    ++j;
                }
                const size_t a = i > into ? i - into : 0, z = std::min(last, j + into);
                if (!stretches.empty() && a <= stretches.back().second) {
                    stretches.back().second = z;
                }
                else {
                    stretches.push_back({a, z});
                }
                i = j + 1;
            }
            for (const auto& [a, z] : stretches) {
                std::vector<QPointF> line(pc.p.begin() + static_cast<std::ptrdiff_t>(a), pc.p.begin() + static_cast<std::ptrdiff_t>(z) + 1);
                if (line.size() < 2) {
                    continue;
                }
                // A free end: on, flat, to where the tip is (the skeleton's end and the depth left
                // there), along the end's direction from half a width back.
                auto toTip = [&](bool atStart) {
                    const QPointF lastPt = atStart ? line.front() : line.back();
                    const QPointF d = endDirection(line, !atStart);
                    if (d.isNull()) {
                        return;
                    }
                    const int tipPixel = atStart ? b.pixels.front() : b.pixels.back();
                    const double reachOut = QPointF::dotProduct(pointOf(tipPixel) - lastPt, d) + double(depthAt(tipPixel)) + 1.0;
                    if (reachOut > 0) {
                        atStart ? (void)line.insert(line.begin(), lastPt + d * reachOut) : line.push_back(lastPt + d * reachOut);
                    }
                };
                // The chain's own ends: a free one flat to its tip, one joining others round.
                if (!ring && c == 0 && a == 0) {
                    if (front > 0) {
                        toTip(true);
                    }
                    else if (pc.depth[0] < r) {
                        roundEnd(pc.p.front());
                    }
                }
                if (!ring && c + 1 == pieces.size() && z == last) {
                    if (back > 0) {
                        toTip(false);
                    }
                    else if (pc.depth[last] < r) {
                        roundEnd(pc.p.back());
                    }
                }
                strokes.push_back(strokeOutline(toCurve(line), width / 2, false));
            }
        }
    }
    // The strokes go into the drawing of the letters: the result is what's covered (anti-aliased,
    // for smooth edges), traced...
    for (const QPainterPath& s : strokes) {
        painter.drawPath(s);
    }
    painter.end();
    std::vector<float> field(inside.size());
    for (int y = 0; y < h; ++y) {
        const uchar* row = image.constScanLine(y);
        for (int x = 0; x < w; ++x) {
            field[static_cast<size_t>(y) * w + x] = (127.5f - float(row[x])) / 255.0f;  // about -0.5 inside to 0.5 out
        }
    }
    // ...and put back together from the curves it was traced from: the letters' own outline
    // wherever that's still the edge (as the font has it), the strokes' where they are.
    const double pixel = 1.0 / k;  // (a grid pixel, in the path's units)
    CurveSet own(pixel), added(pixel);
    own.add(original);
    for (const QPainterPath& s : strokes) {
        added.add(s);
    }
    QPainterPath out;
    out.setFillRule(Qt::WindingFill);
    for (const QPolygonF& loop : traceZero(field, w, h)) {
        std::vector<QPointF> pts;
        pts.reserve(static_cast<size_t>(loop.size()));
        for (const QPointF& p : loop) {
            pts.push_back(toPath.map(p + QPointF(0.5, 0.5)));  // (a pixel's value is its centre's)
        }
        spliceLoop(out, pts, {&own, &added}, 0.6 * pixel, 4.0 * pixel);
    }
    return out;
}
}  // namespace

namespace
{
// The text's lines: as typed, or (`width` > 0, font pixels) each broken between words to fit that
// wide (a word wider than that on its own broken between letters).
QStringList linesOf(const core::Text& t, const QFontMetricsF& fm, double width)
{
    const QStringList paragraphs = QString::fromStdString(t.text).split(QLatin1Char('\n'));
    if (!(width > 0)) {
        return paragraphs;
    }
    QStringList out;
    for (const QString& paragraph : paragraphs) {
        QString line;
        for (const QString& word : paragraph.split(QLatin1Char(' '))) {
            const QString joined = line.isEmpty() ? word : line + QLatin1Char(' ') + word;
            if (fm.horizontalAdvance(joined) <= width || (line.isEmpty() && word.isEmpty())) {
                line = joined;
                continue;
            }
            if (!line.isEmpty()) {
                out << line;
            }
            // The word on a line of its own (in pieces, if it's too wide even so).
            line.clear();
            for (const QChar c : word) {
                if (!line.isEmpty() && fm.horizontalAdvance(line + c) > width) {
                    out << line;
                    line.clear();
                }
                line += c;
            }
        }
        out << line;
    }
    return out;
}

// The letters laid out in lines (font pixels, y down, the anchor at 0, 0): each line aligned on
// its own, the block placed vertically on the anchor, then mirrored in place if asked. Wrapped to
// `wrapWidth` (font pixels) if that's more than 0.
// Each letter's start along `line` (font pixels), by code point.
std::vector<double> glyphStarts(const QString& line, const QFontMetricsF& fm)
{
    std::vector<double> out;
    for (qsizetype i = 0; i < line.size(); ++i) {
        if (!line[i].isLowSurrogate()) {
            out.push_back(fm.horizontalAdvance(line.left(i)));
        }
    }
    return out;
}

// A run of text in font pixels (y down, baseline start at x, y) as one in text space.
struct PixelRun
{
    QString text;
    double x = 0, y = 0;
    double angle = 0;  // degrees, y up
    std::vector<double> glyphs;
};

QPainterPath layoutLines(const core::Text& t, const QFont& font, double capHeight, double wrapWidth = 0,
                         std::vector<PixelRun>* runs = nullptr)
{
    const QFontMetricsF fm(font);
    const QStringList lines = linesOf(t, fm, wrapWidth);
    QPainterPath path;
    path.setFillRule(Qt::WindingFill);
    double left = 0, right = 0;
    std::vector<PixelRun> placed;
    for (qsizetype i = 0; i < lines.size(); ++i) {
        const double width = fm.horizontalAdvance(lines[i]);
        const double x = t.align == core::TextAlign::Left ? 0.0 : t.align == core::TextAlign::Center ? -width / 2 : -width;
        path.addText(x, double(i) * fm.lineSpacing(), font, lines[i]);
        if (!lines[i].trimmed().isEmpty()) {
            placed.push_back({lines[i], x, double(i) * fm.lineSpacing(), 0.0, glyphStarts(lines[i], fm)});
        }
        left = i == 0 ? x : std::min(left, x);
        right = i == 0 ? x + width : std::max(right, x + width);
    }
    const double lastBaseline = double(lines.size() - 1) * fm.lineSpacing();
    double shift = 0.0;  // down
    switch (t.valign) {
        case core::TextVAlign::Baseline: break;
        case core::TextVAlign::Top: shift = capHeight; break;
        case core::TextVAlign::Middle: shift = (capHeight - lastBaseline) / 2; break;
        case core::TextVAlign::Bottom: shift = -lastBaseline; break;
    }
    path.translate(0, shift);
    if (runs && !t.flipH && !t.flipV) {  // (mirrored: only as outlines)
        for (PixelRun& r : placed) {
            r.y += shift;
            runs->push_back(std::move(r));
        }
    }
    // Mirrored about the middle of the block (caps' top to last baseline), so it stays put.
    const double cx = (left + right) / 2, cy = shift + (lastBaseline - capHeight) / 2;
    if (t.flipH || t.flipV) {
        QTransform m;
        m.translate(cx, cy);
        m.scale(t.flipH ? -1 : 1, t.flipV ? -1 : 1);
        m.translate(-cx, -cy);
        path = m.map(path);
    }
    path.setFillRule(Qt::WindingFill);
    return path;
}

// The letters set along a curve, one by one (same units, relative to the anchor): each turned
// to the curve where its middle falls, standing on it (or centred on it, or hanging below).
QPainterPath layoutAlongPath(const core::Text& t, const core::Sketch& s, const QFont& font, double capHeight,
                             double mmPerPixel, std::vector<PixelRun>* runs = nullptr)
{
    QPainterPath path;
    path.setFillRule(Qt::WindingFill);
    if (!s.hasEntity(t.path) || !s.hasEntity(t.anchor)) {
        return path;
    }
    // The curve, in font pixels relative to the anchor (y down), the way the text runs: a line
    // from its first end; arcs and circles clockwise, so the letters stand on the outside and
    // read left to right over the top; a circle from its top (so centred text is centred there).
    const core::Point& anchor = s.point(t.anchor);
    const core::Entity& e = s.entity(t.path);
    const core::EntityKind kind = e.kind();
    const auto* spline = std::get_if<core::Spline>(&e.geom);
    // (whole ellipses and closed splines go round too; elliptical curves run clockwise, as arcs;
    // a chain that comes back to its start too)
    bool closed = kind == core::EntityKind::Circle || kind == core::EntityKind::Ellipse
        || (spline && spline->closed && !spline->isPiece());
    std::vector<core::Vec2> world;
    if (!t.pathMore.empty()) {
        // A chain: each curve along it in turn, run the way that carries on from the one before
        // (the first: away from where it joins the second).
        std::vector<core::Id> chain{t.path};
        chain.insert(chain.end(), t.pathMore.begin(), t.pathMore.end());
        auto near = [](core::Vec2 a, core::Vec2 b) { return std::hypot(a.x - b.x, a.y - b.y); };
        for (size_t k = 0; k < chain.size(); ++k) {
            if (!s.hasEntity(chain[k])) {
                break;
            }
            std::vector<core::Vec2> pts = core::sample(s, chain[k], 240);
            if (pts.size() < 2) {
                continue;
            }
            if (k == 0 && chain.size() > 1 && s.hasEntity(chain[1])) {
                const std::vector<core::Vec2> next = core::sample(s, chain[1], 2);
                const double toFront = std::min(near(pts.front(), next.front()), near(pts.front(), next.back()));
                const double toBack = std::min(near(pts.back(), next.front()), near(pts.back(), next.back()));
                if (toFront < toBack) {
                    std::ranges::reverse(pts);
                }
            }
            else if (k > 0 && near(pts.back(), world.back()) < near(pts.front(), world.back())) {
                std::ranges::reverse(pts);
            }
            world.insert(world.end(), pts.begin() + (world.empty() ? 0 : 1), pts.end());
        }
    }
    else if (kind == core::EntityKind::Circle) {
        const core::Circle& c = s.circle(t.path);
        const core::Point& o = s.point(c.center);
        constexpr int kSteps = 720;
        for (int k = 0; k <= kSteps; ++k) {
            const double a = std::numbers::pi / 2 - 2 * std::numbers::pi * k / kSteps;
            world.push_back({o.x + c.radius * std::cos(a), o.y + c.radius * std::sin(a)});
        }
    }
    else {
        world = core::sample(s, t.path, 720);
        if (kind == core::EntityKind::Arc || kind == core::EntityKind::Ellipse || kind == core::EntityKind::EllipseArc) {
            std::ranges::reverse(world);  // (arcs run anticlockwise)
        }
        if (kind == core::EntityKind::Ellipse && world.size() > 2) {
            // From its top, as a circle (so centred text is centred there).
            world.pop_back();  // (the start, again)
            const auto top = std::ranges::max_element(world, {}, &core::Vec2::y);
            std::ranges::rotate(world, top);
            world.push_back(world.front());
        }
    }
    if (!t.pathMore.empty()) {
        closed = world.size() > 2 && std::hypot(world.front().x - world.back().x, world.front().y - world.back().y) < 1e-9;
    }
    std::vector<QPointF> curve;
    for (const core::Vec2& p : world) {
        curve.emplace_back((p.x - anchor.x) / mmPerPixel, -(p.y - anchor.y) / mmPerPixel);
    }
    if (curve.size() < 2) {
        return path;
    }
    if (t.flipH) {
        std::ranges::reverse(curve);  // running the other way (so on the other side, still readable)
    }
    std::vector<double> along{0.0};
    for (size_t i = 1; i < curve.size(); ++i) {
        along.push_back(along.back() + std::hypot(curve[i].x() - curve[i - 1].x(), curve[i].y() - curve[i - 1].y()));
    }
    const double length = along.back();
    // Where a distance along it falls, and the direction there (carried on past the ends).
    auto at = [&](double d, QPointF& point, QPointF& direction) {
        if (closed && length > 0) {
            d = std::fmod(d, length);  // round and round
            if (d < 0) {
                d += length;
            }
        }
        size_t i = 1;
        while (i + 1 < curve.size() && along[i] < d) {
            ++i;
        }
        const QPointF a = curve[i - 1], b = curve[i];
        const double seg = std::max(1e-12, along[i] - along[i - 1]);
        direction = (b - a) / seg;
        point = a + direction * (d - along[i - 1]);
    };

    const QFontMetricsF fm(font);
    QString text = QString::fromStdString(t.text);
    text.replace(QLatin1Char('\n'), QLatin1Char(' '));
    const double width = fm.horizontalAdvance(text);
    // Where it starts: from the curve's start, its middle or its end (a circle's top for all three:
    // starting there, centred on it, or ending there).
    const double place = closed ? 0.0 : t.align == core::TextAlign::Left ? 0.0 : t.align == core::TextAlign::Center ? length / 2 : length;
    const double start = place - (t.align == core::TextAlign::Left ? 0.0 : t.align == core::TextAlign::Center ? width / 2 : width);
    double lift = 0.0;  // the baseline's distance above the curve (font pixels)
    switch (t.valign) {
        case core::TextVAlign::Baseline:
        case core::TextVAlign::Bottom: break;
        case core::TextVAlign::Top: lift = -capHeight; break;
        case core::TextVAlign::Middle: lift = -capHeight / 2; break;
    }
    for (qsizetype i = 0; i < text.size(); ++i) {
        if (text[i].isLowSurrogate()) {
            continue;  // (the pair goes with its high half)
        }
        const qsizetype n = text[i].isHighSurrogate() && i + 1 < text.size() ? 2 : 1;
        const QString glyph = text.mid(i, n);
        const double x0 = fm.horizontalAdvance(text.left(i));
        const double advance = fm.horizontalAdvance(text.left(i + n)) - x0;
        QPainterPath g;
        g.addText(-advance / 2, -lift, font, glyph);  // its middle at 0, baseline lifted
        if (g.isEmpty()) {
            continue;
        }
        QPointF point, direction;
        at(start + x0 + advance / 2, point, direction);
        // Screen-like axes (y down): along the curve, and "up" for the letter to its left.
        const QPointF up(direction.y(), -direction.x());
        const double sign = t.flipV ? -1.0 : 1.0;  // mirrored: hanging the other way
        const QTransform frame(direction.x(), direction.y(), -up.x() * sign, -up.y() * sign, point.x(), point.y());
        path.addPath(frame.map(g));
        if (runs && !t.flipV && !glyph.trimmed().isEmpty()) {
            // The letter on its own: its baseline's start, turned with the curve (y up).
            const QPointF origin = frame.map(QPointF(-advance / 2, -lift));
            runs->push_back({glyph, origin.x(), origin.y(), std::atan2(-direction.y(), direction.x()) * 180.0 / std::numbers::pi, {0.0}});
        }
    }
    path.setFillRule(Qt::WindingFill);
    return path;
}

// A box text's box, from its corners: which way its bottom side runs (degrees) and its size.
struct Box
{
    double angle = 0, width = 0, height = 0;
};
Box boxOf(const core::Sketch& s, const core::Text& t)
{
    const core::Point& a = s.point(t.anchor);
    const core::Point& b = s.point(t.boxSide);
    const core::Point& c = s.point(t.boxCorner);
    const double ux = b.x - a.x, uy = b.y - a.y;
    const double len = std::hypot(ux, uy);
    Box box;
    if (len <= 0) {
        return box;
    }
    box.angle = std::atan2(uy, ux) * 180.0 / std::numbers::pi;
    box.width = len;
    box.height = (-(uy / len) * (c.x - a.x) + (ux / len) * (c.y - a.y));  // across, to the left
    return box;
}

// The letters in a box (same units; the anchor, the box's corner, at 0, 0): each line against
// its left side, middle or right side, the block against its top, middle or bottom.
QPainterPath layoutInBox(const core::Text& t, const core::Sketch& s, const QFont& font, double capHeight,
                         double mmPerPixel, std::vector<PixelRun>* runs = nullptr)
{
    const Box box = boxOf(s, t);
    core::Text placed = t;
    // A block hanging from the top, centred on the middle, or sitting on the bottom (a baseline
    // alignment means the bottom, in a box).
    if (placed.valign == core::TextVAlign::Baseline) {
        placed.valign = core::TextVAlign::Bottom;
    }
    const size_t before = runs ? runs->size() : 0;
    QPainterPath path = layoutLines(placed, font, capHeight, t.wrap ? box.width / mmPerPixel : 0.0, runs);
    const double x = t.align == core::TextAlign::Left ? 0.0 : t.align == core::TextAlign::Center ? box.width / 2 : box.width;
    const double y = placed.valign == core::TextVAlign::Top ? box.height
        : placed.valign == core::TextVAlign::Middle         ? box.height / 2
                                                            : 0.0;
    path.translate(x / mmPerPixel, -y / mmPerPixel);  // (font pixels, y down)
    if (runs) {
        for (size_t i = before; i < runs->size(); ++i) {
            (*runs)[i].x += x / mmPerPixel;
            (*runs)[i].y -= y / mmPerPixel;
        }
    }
    return path;
}

// A fingerprint of what a path text's curve is now (its points and size), to skip remaking.
std::size_t curveKey(const core::Sketch& s, core::Id curve)
{
    std::size_t key = std::hash<core::Id>{}(curve);
    auto mix = [&](double v) { key = key * 1000003u ^ std::hash<long long>{}(std::llround(v * 1e6)); };
    for (core::Id p : s.definingPoints(curve)) {
        mix(s.point(p).x);
        mix(s.point(p).y);
    }
    if (s.entity(curve).kind() == core::EntityKind::Circle) {
        mix(s.circle(curve).radius);
    }
    return key;
}
}  // namespace

std::vector<std::vector<core::Point>> make(const core::Text& t, const core::Sketch* sketch)
{
    return core::flattenLetters(makeLetters(t, sketch), core::letterTolerance(t.height));
}

namespace
{
// A text's font at the size letters are made at, its capitals' height there, and the scale to mm.
struct FontSetup
{
    QFont font;
    double capHeight = 0;
    double mmPerPixel = 0;
};
FontSetup fontFor(const core::Text& t)
{
    FontSetup f;
    f.font = QFont(QString::fromStdString(t.font));
    f.font.setPixelSize(static_cast<int>(kPixelSize));
    f.font.setBold(t.bold);
    f.font.setItalic(t.italic);
    f.font.setKerning(true);
    if (t.spacing != 0.0) {
        f.font.setLetterSpacing(QFont::AbsoluteSpacing, t.spacing / 100.0 * kPixelSize);
    }
    f.capHeight = QFontMetricsF(f.font).capHeight();
    if (!(f.capHeight > 0)) {
        f.capHeight = QFontMetricsF(f.font).ascent() * 0.7;
    }
    f.mmPerPixel = t.height / f.capHeight;
    return f;
}
}  // namespace

double wrappedHeight(const core::Text& t, double width)
{
    // From the top of the first line's capitals to the bottom of the last line's descenders.
    const FontSetup f = fontFor(t);
    const QFontMetricsF fm(f.font);
    const QStringList lines = linesOf(t, fm, t.wrap ? width / f.mmPerPixel : 0.0);
    return (f.capHeight + double(lines.size() - 1) * fm.lineSpacing() + fm.descent()) * f.mmPerPixel;
}

std::vector<core::TextContour> makeLetters(const core::Text& t, const core::Sketch* sketch, std::vector<core::TextRun>* runs)
{
    if (runs) {
        runs->clear();
    }
    if (t.text.empty()) {
        return {};
    }
    const FontSetup setup = fontFor(t);
    const QFont& font = setup.font;
    const double capHeight = setup.capHeight;
    const double mmPerPixel = setup.mmPerPixel;

    // As text too, unless its outlines are made into something the font hasn't got.
    std::vector<PixelRun> pixelRuns;
    std::vector<PixelRun>* wanted = runs && t.outline == core::TextOutline::AsDesigned && t.minThickness <= 0.0 ? &pixelRuns : nullptr;
    QPainterPath path = t.path != core::kNoId && sketch ? layoutAlongPath(t, *sketch, font, capHeight, mmPerPixel, wanted)
        : t.inBox() && sketch                           ? layoutInBox(t, *sketch, font, capHeight, mmPerPixel, wanted)
                                                        : layoutLines(t, font, capHeight, 0, wanted);
    if (runs) {
        const double tracking = t.spacing / 100.0 * kPixelSize * mmPerPixel;
        for (const PixelRun& r : pixelRuns) {
            core::TextRun run;
            run.text = r.text.toStdString();
            run.at = {r.x * mmPerPixel, -r.y * mmPerPixel};
            run.angle = r.angle;
            run.size = kPixelSize * mmPerPixel;
            run.tracking = r.glyphs.size() > 1 ? tracking : 0.0;
            for (double g : r.glyphs) {
                run.glyphs.push_back(g * mmPerPixel);
            }
            runs->push_back(std::move(run));
        }
    }
    switch (t.outline) {
        case core::TextOutline::AsDesigned: break;
        case core::TextOutline::Merged: path = path.simplified(); break;
        case core::TextOutline::Silhouette: path = silhouette(path.simplified()); break;
    }
    if (t.minThickness > 0.0) {
        path = thicken(path.simplified(), t.minThickness / mmPerPixel, path);
    }

    // The path's own lines and curves (the font's, as designed; joined-up or thickened outlines
    // are lines already), to mm with y up.
    std::vector<core::TextContour> out;
    core::TextContour contour;
    auto toMm = [&](double x, double y) { return core::Point{x * mmPerPixel, -y * mmPerPixel}; };
    auto same = [](core::Point a, core::Point b) { return std::hypot(a.x - b.x, a.y - b.y) < 1e-9; };
    auto finish = [&] {
        // (the line back to the start that closes it is implied)
        if (!contour.ops.empty() && contour.ops.back() == 'L' && same(contour.points.back(), contour.points.front())) {
            contour.ops.pop_back();
            contour.points.pop_back();
        }
        const bool area = contour.ops.find('C') != std::string::npos || contour.ops.size() >= 2;
        if (area) {
            out.push_back(std::move(contour));
        }
        contour = {};
    };
    for (int i = 0; i < path.elementCount(); ++i) {
        const QPainterPath::Element e = path.elementAt(i);
        if (e.isMoveTo()) {
            finish();
            contour.points.push_back(toMm(e.x, e.y));
        }
        else if (e.isLineTo()) {
            const core::Point p = toMm(e.x, e.y);
            if (!contour.points.empty() && !same(p, contour.points.back())) {
                contour.points.push_back(p);
                contour.ops += 'L';
            }
        }
        else if (e.isCurveTo() && i + 2 < path.elementCount()) {
            const QPainterPath::Element c2 = path.elementAt(i + 1), to = path.elementAt(i + 2);
            contour.points.push_back(toMm(e.x, e.y));
            contour.points.push_back(toMm(c2.x, c2.y));
            contour.points.push_back(toMm(to.x, to.y));
            contour.ops += 'C';
            i += 2;
        }
    }
    finish();
    return out;
}

// What a linked text (along a path, or in a box) depends on now, to skip remaking it if unchanged.
std::size_t linkKey(const core::Sketch& s, const core::Text& t)
{
    if (t.path != core::kNoId) {
        if (!s.hasEntity(t.path)) {
            return 0;
        }
        std::size_t key = curveKey(s, t.path);
        for (core::Id c : t.pathMore) {  // (the rest of its chain)
            key = key * 1000003u ^ (s.hasEntity(c) ? curveKey(s, c) : 0);
        }
        return key;
    }
    if (t.inBox()) {
        std::size_t key = 17;
        for (core::Id p : {t.anchor, t.boxSide, t.boxCorner}) {
            if (!s.hasEntity(p)) {
                return 0;
            }
            key = key * 1000003u ^ std::hash<long long>{}(std::llround(s.point(p).x * 1e6));
            key = key * 1000003u ^ std::hash<long long>{}(std::llround(s.point(p).y * 1e6));
        }
        return key;
    }
    return 0;
}

void update(core::Text& text, const core::Sketch* sketch)
{
    if (text.path != core::kNoId) {
        text.angle = 0.0;  // (the curve sets the letters' directions)
    }
    if (text.inBox() && sketch) {
        text.angle = boxOf(*sketch, text).angle;  // turned with its box
    }
    text.letters = makeLetters(text, sketch, &text.runs);
    text.shape = core::flattenLetters(text.letters, core::letterTolerance(text.height));
    text.layoutKey = sketch ? linkKey(*sketch, text) : 0;
}

bool fitBox(core::Sketch& sketch, core::Id id, std::string* why)
{
    if (!sketch.hasText(id)) {
        return true;
    }
    const core::Text& t = sketch.text(id);
    if (!t.inBox() || !t.wrap || !t.grow) {
        return true;
    }
    const Box box = boxOf(sketch, t);
    const double need = wrappedHeight(t, box.width);
    const double change = need - box.height;
    if (std::abs(change) < 1e-7) {
        return true;
    }
    // Which side moves: away from where the text's aligned (both halfway for the middle).
    const bool top = t.valign == core::TextVAlign::Top;
    const bool middle = t.valign == core::TextVAlign::Middle;
    const double a = box.angle * std::numbers::pi / 180.0;
    const core::Vec2 up{-std::sin(a), std::cos(a)};
    const double bottomBy = top ? -change : middle ? -change / 2 : 0.0;
    const double topBy = top ? 0.0 : middle ? change / 2 : change;
    const core::Point anchor = sketch.point(t.anchor);
    const std::vector<core::Id> corners = core::textPoints(sketch, t);  // (the anchor and the box's corners)

    core::Sketch moved = sketch;
    bool ok = false;
    {
        core::Solver solver(moved, moved.layerOf(t.anchor));
        solver.beginDrag(corners);  // (all of them: those not to move, held where they are)
        ok = solver.dragMap([&](core::Vec2 p) {
            const double height = (p.x - anchor.x) * up.x + (p.y - anchor.y) * up.y;
            const double by = height < box.height / 2 ? bottomBy : topBy;
            return core::Vec2{p.x + up.x * by, p.y + up.y * by};
        });
        if (ok) {
            solver.writeBack(moved);
        }
    }
    ok = ok && std::abs(boxOf(moved, moved.text(id)).height - need) < 1e-6;
    if (!ok) {
        if (why) {
            const char* side = top ? "bottom side" : middle ? "top and bottom sides" : "top side";
            *why = std::string("the text box's ") + side
                + " can't move to fit the text: something holds it (a dimension, a fixed point, ...)";
        }
        return false;
    }
    sketch = std::move(moved);
    return true;
}

bool fitBoxes(core::Sketch& sketch)
{
    std::vector<core::Id> ids;
    for (const auto& [id, t] : sketch.texts()) {
        if (t.inBox() && t.wrap && t.grow) {
            ids.push_back(id);
        }
    }
    bool changed = false;
    for (core::Id id : ids) {
        const Box before = boxOf(sketch, sketch.text(id));
        if (fitBox(sketch, id) && std::abs(boxOf(sketch, sketch.text(id)).height - before.height) > 1e-9) {
            changed = true;
        }
    }
    return changed;
}

void refreshPathTexts(core::Sketch& sketch)
{
    std::vector<core::Text> changed;
    for (const auto& [id, t] : sketch.texts()) {
        if ((t.path != core::kNoId || t.inBox()) && linkKey(sketch, t) != t.layoutKey) {
            changed.push_back(t);
        }
    }
    for (core::Text& t : changed) {
        update(t, &sketch);
        sketch.setText(t);
    }
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

void addUserFonts()
{
#ifdef _WIN32
    const QString dir = qEnvironmentVariable("LOCALAPPDATA") + QStringLiteral("/Microsoft/Windows/Fonts");
    for (const QFileInfo& file : QDir(dir).entryInfoList({QStringLiteral("*.ttf"), QStringLiteral("*.otf"),
                                                           QStringLiteral("*.ttc")}, QDir::Files)) {
        QFontDatabase::addApplicationFont(file.absoluteFilePath());
    }
#endif
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
