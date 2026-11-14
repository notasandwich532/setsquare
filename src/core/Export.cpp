#include "Export.h"

#include "Curves.h"
#include "Geometry.h"
#include "Gradient.h"
#include "Hatch.h"
#include "Regions.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <functional>
#include <iterator>
#include <map>
#include <numbers>
#include <sstream>
#include <string_view>
#include <tuple>
#include <vector>

namespace core::exporter
{

namespace
{
constexpr double kPi = std::numbers::pi;
constexpr double kHairline = 0.05;  // mm: what a 0-width (hairline) stroke is written as

Vec2 add(Vec2 a, Vec2 b) { return {a.x + b.x, a.y + b.y}; }
Vec2 sub(Vec2 a, Vec2 b) { return {a.x - b.x, a.y - b.y}; }
Vec2 mul(Vec2 a, double k) { return {a.x * k, a.y * k}; }
double dot(Vec2 a, Vec2 b) { return a.x * b.x + a.y * b.y; }
double cross(Vec2 a, Vec2 b) { return a.x * b.y - a.y * b.x; }

// Up to 4 decimals (0.1 micron), without trailing zeros.
std::string num(double v)
{
    char buf[64];
    std::snprintf(buf, sizeof buf, "%.4f", v);
    std::string s = buf;
    if (s.find('.') != std::string::npos) {
        while (s.back() == '0') {
            s.pop_back();
        }
        if (s.back() == '.') {
            s.pop_back();
        }
    }
    return s == "-0" ? "0" : s;
}

std::string rgb(const Color& c)
{
    char buf[8];
    std::snprintf(buf, sizeof buf, "#%02x%02x%02x", c.r, c.g, c.b);
    return buf;
}

// Whether an item's layer is shown (hidden layers aren't exported; hidden sublayers are only
// hidden while sketching).
bool layerShown(const Sketch& s, Id id)
{
    const Id layer = s.layerOf(id);
    return layer == kNoId || !s.hasLayer(layer) || s.layer(layer).visible;
}

std::string xmlEscape(const std::string& text)
{
    std::string out;
    for (char c : text) {
        switch (c) {
            case '&': out += "&amp;"; break;
            case '<': out += "&lt;"; break;
            case '>': out += "&gt;"; break;
            case '"': out += "&quot;"; break;
            default: out += c;
        }
    }
    return out;
}

struct Item
{
    Id curve = kNoId;
    StrokeStyle style;
};

// A curve a fill's outline may run along, for writing the outline exactly.
struct Along
{
    curve::Path path;
    std::array<double, 4> box;  // x0, y0, x1, y1
};

// The solid curves of a layer (what fills are bounded by).
std::vector<Along> curvesAlong(const Sketch& s, Id layer)
{
    std::vector<Along> out;
    for (const auto& [id, e] : s.entities()) {
        if (e.kind() != EntityKind::Point && !e.construction && !s.isReference(id) && s.layerOf(id) == layer) {
            out.push_back({curve::pathOf(s, id), curve::box(s, id)});
        }
    }
    return out;
}

double wrapPi(double a)  // into (-pi, pi]
{
    while (a > kPi) {
        a -= 2 * kPi;
    }
    while (a <= -kPi) {
        a += 2 * kPi;
    }
    return a;
}

// A stretch of a closed outline along one curve, exactly: a line; part of a circle (centre,
// radius, from angle a0 turning by sweep: counter-clockwise if positive); part of an ellipse
// (its frame, from parameter a0 by sweep); or Bezier pieces.
struct Edge
{
    enum class Kind
    {
        Line,
        Arc,
        Ellipse,
        Bezier,
    };
    Kind kind = Kind::Line;
    Vec2 from, to;
    Vec2 c;
    double r = 0.0;
    curve::EllipseFrame f;
    double a0 = 0.0, sweep = 0.0;
    std::vector<curve::Bezier> pieces;
};

// One closed outline (sampled points round it) as exact edges: each stretch of it along one curve
// as that curve, found by which curves its points lie on. None if it can't be (a point on none of
// them).
std::optional<std::vector<Edge>> loopEdges(const std::vector<Vec2>& pts, const std::vector<Along>& curves)
{
    constexpr double kOn = 1e-6;
    const size_t n = pts.size();
    if (n < 2) {
        return std::nullopt;
    }
    double bx0 = 1e300, by0 = 1e300, bx1 = -1e300, by1 = -1e300;
    for (const Vec2& p : pts) {
        bx0 = std::min(bx0, p.x);
        by0 = std::min(by0, p.y);
        bx1 = std::max(bx1, p.x);
        by1 = std::max(by1, p.y);
    }
    std::vector<const curve::Path*> near;
    for (const Along& c : curves) {
        if (c.box[0] <= bx1 + kOn && c.box[2] >= bx0 - kOn && c.box[1] <= by1 + kOn && c.box[3] >= by0 - kOn) {
            near.push_back(&c.path);
        }
    }
    // Which of them each point is on.
    std::vector<std::vector<size_t>> on(n);
    for (size_t i = 0; i < n; ++i) {
        for (size_t c = 0; c < near.size(); ++c) {
            if (near[c]->distance(pts[i]) < kOn) {
                on[i].push_back(c);
            }
        }
        if (on[i].empty()) {
            return std::nullopt;
        }
    }
    auto common = [](const std::vector<size_t>& a, const std::vector<size_t>& b) {
        std::vector<size_t> out;
        std::ranges::set_intersection(a, b, std::back_inserter(out));
        return out;
    };

    std::vector<Edge> edges;
    // A stretch along curve `c` through points idx (in order).
    auto add = [&](const std::vector<size_t>& idx, size_t c) {
        const curve::Path& p = *near[c];
        Edge e;
        e.from = pts[idx.front()];
        e.to = pts[idx.back()];
        switch (p.kind()) {
            case curve::Path::Kind::Line: e.kind = Edge::Kind::Line; break;
            case curve::Path::Kind::Circle:
            case curve::Path::Kind::Arc:
            case curve::Path::Kind::Ellipse:
            case curve::Path::Kind::EllipseArc: {
                const bool round = p.kind() == curve::Path::Kind::Circle || p.kind() == curve::Path::Kind::Arc;
                auto angle = [&](Vec2 q) {
                    return round ? std::atan2(q.y - p.c.y, q.x - p.c.x) : p.ellipseShape.param(q);
                };
                for (size_t k = 1; k < idx.size(); ++k) {
                    e.sweep += wrapPi(angle(pts[idx[k]]) - angle(pts[idx[k - 1]]));
                }
                e.a0 = angle(e.from);
                e.kind = round ? Edge::Kind::Arc : Edge::Kind::Ellipse;
                e.c = p.c;
                e.r = p.r;
                e.f = p.ellipseShape;
                break;
            }
            case curve::Path::Kind::Spline: {
                // Along the whole curve's parameter (one unit per piece), from its start.
                const double size = static_cast<double>(p.pieces.size());
                auto param = [&](Vec2 q) { return p.start + p.param(q); };
                double span = 0;
                for (size_t k = 1; k < idx.size(); ++k) {
                    double step = param(pts[idx[k]]) - param(pts[idx[k - 1]]);
                    if (p.closedSpline) {
                        step = std::remainder(step, size);
                    }
                    span += step;
                }
                const double lo = span >= 0 ? param(e.from) : param(e.from) + span;
                e.pieces = curve::Path::spline(p.pieces, p.closedSpline, lo, std::abs(span)).beziers();
                if (span < 0) {
                    std::ranges::reverse(e.pieces);
                    for (curve::Bezier& b : e.pieces) {
                        std::ranges::reverse(b.p);
                    }
                }
                if (e.pieces.empty()) {
                    e.kind = Edge::Kind::Line;
                }
                else {
                    e.kind = Edge::Kind::Bezier;
                    e.pieces.front().p[0] = e.from;  // (ends exactly where the loop's are)
                    e.pieces.back().p[3] = e.to;
                }
                break;
            }
        }
        edges.push_back(std::move(e));
    };

    // Start where it goes from one curve to another (else it's all one closed curve).
    size_t first = n;
    for (size_t i = 0; i < n; ++i) {
        if (on[i].size() >= 2 && common(on[(i + n - 1) % n], on[(i + 1) % n]).empty()) {
            first = i;
            break;
        }
    }
    if (first == n) {
        std::vector<size_t> all = on[0];
        for (size_t i = 1; i < n && !all.empty(); ++i) {
            all = common(all, on[i]);
        }
        if (all.empty()) {
            return std::nullopt;
        }
        // Round it in thirds (back to the start): each well short of half way round.
        for (size_t k = 0; k < 3; ++k) {
            std::vector<size_t> third;
            for (size_t i = n * k / 3; i <= n * (k + 1) / 3; ++i) {
                third.push_back(i % n);
            }
            add(third, all.front());
        }
        return edges;
    }
    std::vector<size_t> run{first};
    std::vector<size_t> along = on[first];
    for (size_t k = 1; k <= n; ++k) {
        const size_t j = (first + k) % n;
        std::vector<size_t> both = common(along, on[j]);
        if (!both.empty()) {
            along = std::move(both);
            run.push_back(j);
            continue;
        }
        add(run, along.front());
        const size_t last = run.back();
        along = common(on[last], on[j]);
        if (along.empty()) {
            return std::nullopt;  // a jump from one curve to another with nothing in common
        }
        run = {last, j};
    }
    add(run, along.front());
    return edges;
}

// One closed outline as exact SVG path commands (see loopEdges): a line as L, a circle's or
// ellipse's part as A, a spline's as C. Empty if it can't be: then it's written as the points.
std::string exactLoop(const std::vector<Vec2>& pts, const std::vector<Along>& curves,
                      const std::function<std::string(Vec2)>& xy)
{
    const std::optional<std::vector<Edge>> edges = loopEdges(pts, curves);
    if (!edges || edges->empty()) {
        return {};
    }
    std::string d = "M" + xy(edges->front().from);
    for (const Edge& e : *edges) {
        switch (e.kind) {
            case Edge::Kind::Line: d += " L" + xy(e.to); break;
            case Edge::Kind::Arc:
            case Edge::Kind::Ellipse: {
                const bool round = e.kind == Edge::Kind::Arc;
                // In halves where it's more than half way round (SVG's arcs are ambiguous there).
                const int parts = std::abs(e.sweep) > kPi * 0.999 ? 2 : 1;
                const std::string radii = round ? num(e.r) + ',' + num(e.r) + " 0"
                                                : num(e.f.a) + ',' + num(e.f.b) + ' '
                        + num(-std::atan2(e.f.u.y, e.f.u.x) * 180 / kPi);
                for (int k = 1; k <= parts; ++k) {
                    const double a = e.a0 + e.sweep * k / parts;
                    const Vec2 q = k == parts ? e.to
                        : round                ? Vec2{e.c.x + e.r * std::cos(a), e.c.y + e.r * std::sin(a)}
                                               : e.f.at(a);
                    // Counter-clockwise with y up is clockwise with y down: sweep-flag 0.
                    d += " A" + radii + " 0," + (e.sweep > 0 ? "0 " : "1 ") + xy(q);
                }
                break;
            }
            case Edge::Kind::Bezier:
                for (const curve::Bezier& b : e.pieces) {
                    d += " C" + xy(b.p[1]) + ' ' + xy(b.p[2]) + ' ' + xy(b.p[3]);
                }
                break;
        }
    }
    return d + " Z";
}

// A Bezier piece as points after its start (which `to` has already), within `tolerance`: halved
// until its control points are that close to the line between its ends.
void flattenBezier(const curve::Bezier& b, double tolerance, std::vector<Vec2>& to, int depth = 0)
{
    const auto& p = b.p;
    auto off = [&](Vec2 q) {
        const double dx = p[3].x - p[0].x, dy = p[3].y - p[0].y, len = std::hypot(dx, dy);
        const double qx = q.x - p[0].x, qy = q.y - p[0].y;
        return len > 1e-12 ? std::abs(dx * qy - dy * qx) / len : std::hypot(qx, qy);
    };
    if (depth >= 12 || std::max(off(p[1]), off(p[2])) <= tolerance) {
        to.push_back(p[3]);
        return;
    }
    flattenBezier(b.part(0, 0.5), tolerance, to, depth + 1);
    flattenBezier(b.part(0.5, 1), tolerance, to, depth + 1);
}

// Whether a text goes out as text (else as its letters' outlines).
bool asText(const Text& t, bool outlinesWanted)
{
    return !outlinesWanted && !t.runs.empty() && !t.font.empty();
}

// A hatch's lines repeat in a tile (a parallelogram: `a` along its first family's lines, `b`
// across them, from `origin`, all in drawing coordinates) if every family's lines land on its own
// again when moved by either side. The smallest such tile within a few repeats; none if there's
// none (families at angles that never meet up, or dashes that don't).
struct HatchTile
{
    Vec2 origin, a, b;
};
std::optional<HatchTile> hatchTile(const Hatch& h)
{
    const std::vector<HatchLine> families = hatch::placed(h);
    if (families.empty()) {
        return std::nullopt;
    }
    auto dir = [](const HatchLine& f) {
        const double r = f.angle * kPi / 180.0;
        return Vec2{std::cos(r), std::sin(r)};
    };
    auto period = [](const HatchLine& f) {
        double p = 0;
        for (double d : f.dashes) {
            p += std::abs(d);
        }
        return p;
    };
    // Whether moving the family's lines by v lands them on its own lines (and dashes on dashes).
    auto keeps = [&](const HatchLine& f, Vec2 v) {
        const Vec2 u = dir(f), n{-u.y, u.x};
        if (std::abs(f.dy) < 1e-9) {
            return false;
        }
        const double j = dot(v, n) / f.dy;
        if (std::abs(j - std::round(j)) > 1e-6 * std::max(1.0, std::abs(j))) {
            return false;
        }
        const double p = period(f);
        if (p <= 1e-9) {
            return true;  // (unbroken lines)
        }
        const double t = dot(v, u) - std::round(j) * f.dx;
        return std::abs(std::remainder(t, p)) < 1e-6 * std::max(1.0, p);
    };
    auto keepsAll = [&](Vec2 v) { return std::ranges::all_of(families, [&](const HatchLine& f) { return keeps(f, v); }); };

    const HatchLine& f0 = families.front();
    const Vec2 u = dir(f0), n{-u.y, u.x};
    if (std::abs(f0.dy) < 1e-9) {
        return std::nullopt;
    }
    // Along the first family: lengths its own dashes and the others' crossings repeat in.
    std::vector<double> bases{std::abs(f0.dy)};
    for (const HatchLine& f : families) {
        const double sine = std::abs(cross(u, dir(f)));
        if (sine > 1e-9) {
            bases.push_back(std::abs(f.dy) / sine);
        }
        if (const double p = period(f); p > 1e-9) {
            bases.push_back(p);
        }
    }
    std::vector<double> lengths;
    for (double b : bases) {
        for (int k = 1; k <= 12; ++k) {
            lengths.push_back(b * k);
        }
    }
    std::ranges::sort(lengths);
    std::optional<double> along;
    for (double c : lengths) {
        if (c > 1e-9 && keepsAll(mul(u, c))) {
            along = c;
            break;
        }
    }
    if (!along) {
        return std::nullopt;
    }
    for (int j = 1; j <= 12; ++j) {
        const double shift = std::fmod(j * f0.dx, *along);
        for (double t : {0.0, shift}) {
            const Vec2 v = add(mul(n, j * f0.dy), mul(u, t));
            if (keepsAll(v)) {
                return HatchTile{{f0.x, f0.y}, mul(u, *along), v};
            }
        }
    }
    return std::nullopt;
}

// Dash patterns in multiples of the line width, as the canvas draws them.
std::vector<double> dashPattern(Dash dash)
{
    switch (dash) {
        case Dash::Dashed: return {5, 3};
        case Dash::Dotted: return {1, 2};
        case Dash::DashDot: return {8, 3, 1, 3};
        case Dash::DashDotDot: return {8, 3, 1, 3, 1, 3};
        case Dash::Solid: break;
    }
    return {};
}
}  // namespace

std::string toSvg(const Sketch& s, const SvgOptions& o)
{
    // What goes in: per layer (bottom first; hidden ones left out), by kind.
    struct Area
    {
        Color color;
        Gradient gradient;
        regions::Region region;
        Hatch hatch;  // when active: drawn with its lines
    };
    struct TextArea
    {
        const Text* text = nullptr;
        bool asText = false;  // (else its outlines)
        Color color;
        Gradient gradient;
        std::vector<TextContour> contours;  // in world coordinates (lines and curves)
    };
    // A sublayer's part: its texts, lines (stroked, then plain), construction.
    struct Sub
    {
        const Sublayer* sublayer = nullptr;
        std::vector<Item> strokes, plain, construction;
        std::vector<TextArea> texts;
        bool empty() const { return strokes.empty() && plain.empty() && construction.empty() && texts.empty(); }
    };
    struct Group
    {
        const Layer* layer = nullptr;
        std::vector<Area> fills;
        std::vector<Sub> subs;  // in order
        bool empty() const
        {
            return fills.empty() && std::ranges::all_of(subs, [](const Sub& sub) { return sub.empty(); });
        }
    };
    const StrokeStyle constructionStyle{{0x9a, 0xa0, 0xa6, 255}, 0.18, Dash::Dashed, {}};
    const regions::Arrangements arrangements(s, kPi / 720);  // quarter-degree steps on arcs
    std::vector<Group> groups;
    for (auto it = s.layers().rbegin(); it != s.layers().rend(); ++it) {
        if (!it->visible) {
            continue;
        }
        Group g;
        g.layer = &*it;
        for (Id sub : s.sublayersOf(it->id)) {
            g.subs.push_back({&s.sublayer(sub), {}, {}, {}, {}});
        }
        if (g.subs.empty()) {
            continue;
        }
        // (an item in a sublayer that's gone, or none: in the first)
        auto subOf = [&](Id sublayer) -> Sub& {
            for (Sub& sub : g.subs) {
                if (sub.sublayer->id == sublayer) {
                    return sub;
                }
            }
            return g.subs.front();
        };
        for (const auto& [id, e] : s.entities()) {
            if (e.kind() == EntityKind::Point || s.isReference(id) || s.layerOf(id) != it->id) {
                continue;
            }
            Sub& sub = subOf(e.sublayer);
            if (e.construction) {
                if (o.construction) {
                    sub.construction.push_back({id, constructionStyle});
                }
            }
            else if (const StrokeStyle* style = s.stroke(id)) {
                if (!style->none()) {  // (no line: nothing to draw)
                    sub.strokes.push_back({id, *style});
                }
            }
            else if (o.undecorated) {
                sub.plain.push_back({id, o.plain});
            }
        }
        if (o.fills) {
            for (const auto& [id, f] : s.fills()) {
                if (f.layer != it->id) {
                    continue;
                }
                if (auto region = regions::resolve(arrangements, f)) {
                    g.fills.push_back({f.color, f.gradient, std::move(*region), f.hatch});
                }
            }
        }
        // Texts: their letters in world coordinates, filled.
        for (const auto& [id, t] : s.texts()) {
            if (t.shape.empty() || !s.hasEntity(t.anchor) || s.layerOf(id) != it->id) {
                continue;
            }
            TextArea area{&t, asText(t, o.textAsOutlines), t.hasColor ? t.color : o.plain.color,
                          t.hasColor ? t.gradient : Gradient{}, {}};
            std::vector<TextContour> letters = t.letters;
            if (letters.empty()) {  // (made without the curves: its points)
                for (const auto& poly : t.shape) {
                    letters.push_back({poly, std::string(poly.empty() ? 0 : poly.size() - 1, 'L')});
                }
            }
            for (TextContour& c : letters) {
                for (Point& p : c.points) {
                    p = textToWorld(s.point(t.anchor), t.angle, p);  // (turning and moving keeps curves exact)
                }
                area.contours.push_back(std::move(c));
            }
            subOf(t.sublayer).texts.push_back(std::move(area));
        }
        if (!g.empty()) {
            groups.push_back(std::move(g));
        }
    }
    // With a page (and asked for): the SVG is the page, at its size.
    const std::optional<Page> page = o.page && !o.canvas ? s.page() : std::nullopt;
    if (groups.empty() && !page) {
        throw SketchError("there's nothing to export");
    }

    // The extent of it all (with the lines' thickness), in sketch coordinates.
    double x0 = 1e300, y0 = 1e300, x1 = -1e300, y1 = -1e300;
    auto include = [&](Vec2 p, double pad) {
        x0 = std::min(x0, p.x - pad);
        y0 = std::min(y0, p.y - pad);
        x1 = std::max(x1, p.x + pad);
        y1 = std::max(y1, p.y + pad);
    };
    for (const Group& g : groups) {
        for (const Sub& sub : g.subs) {
            for (const auto* list : {&sub.strokes, &sub.plain, &sub.construction}) {
                for (const Item& item : *list) {
                    const double pad = std::max(item.style.width, kHairline) / 2;
                    for (const Vec2& p : sample(s, item.curve, 256)) {
                        include(p, pad);
                    }
                }
            }
            for (const TextArea& t : sub.texts) {
                for (const auto& contour : t.contours) {
                    for (const Point& p : contour.points) {
                        include({p.x, p.y}, 0.0);  // (control points too: a little generous at worst)
                    }
                }
            }
        }
        for (const Area& a : g.fills) {
            for (const Vec2& p : a.region.outer) {
                include(p, 0.0);
            }
        }
    }
    double m = std::max(0.0, o.margin);
    if (page) {
        m = 0.0;
        const std::array<double, 4> r = page->rect();
        x0 = r[0];
        y0 = r[1];
        x1 = r[2];
        y1 = r[3];
    }
    else if (o.canvas) {
        // A fixed frame, in its own units.
        m = 0.0;
        x0 = (*o.canvas)[0];
        y0 = (*o.canvas)[1];
        x1 = (*o.canvas)[2];
        y1 = (*o.canvas)[3];
    }
    const double width = x1 - x0 + 2 * m, height = y1 - y0 + 2 * m;
    // Sketch y points up, SVG's down.
    auto X = [&](double x) { return num(x - x0 + m); };
    auto Y = [&](double y) { return num(y1 - y + m); };
    const std::function<std::string(Vec2)> xy = [&](Vec2 p) { return X(p.x) + ',' + Y(p.y); };
    std::map<Id, std::vector<Along>> alongByLayer;
    auto curvesOf = [&](Id layer) -> const std::vector<Along>& {
        auto it = alongByLayer.find(layer);
        if (it == alongByLayer.end()) {
            it = alongByLayer.emplace(layer, curvesAlong(s, layer)).first;
        }
        return it->second;
    };

    std::ostringstream out;
    const std::string unit = o.canvas ? "" : "mm";
    out << "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
        << "<svg xmlns=\"http://www.w3.org/2000/svg\" "
           "xmlns:inkscape=\"http://www.inkscape.org/namespaces/inkscape\" version=\"1.1\"\n"
        << "     width=\"" << num(width) << unit << "\" height=\"" << num(height) << unit << "\" viewBox=\"0 0 "
        << num(width) << ' ' << num(height) << "\">\n"
        << (o.canvas ? "  <!-- Setsquare icon -->\n" : "  <!-- Setsquare export: 1 unit = 1 mm -->\n");
    // The drawing goes in after the gradients it uses (collected as it's written).
    const std::string head = out.str();
    out.str({});
    if (page && page->hasColor) {
        // The paper, under everything.
        out << "  <rect id=\"page\" x=\"0\" y=\"0\" width=\"" << num(width) << "\" height=\"" << num(height)
            << "\" fill=\"" << rgb(page->color) << '"';
        if (page->color.a != 255) {
            out << " fill-opacity=\"" << num(page->color.a / 255.0) << '"';
        }
        out << " stroke=\"none\"/>\n";
    }
    std::string defs;
    int gradients = 0;
    std::map<std::string, std::string> written;  // a definition (without its id) -> its id
    // A gradient's definition, in drawing units (userSpaceOnUse: it doesn't depend on how SVG
    // would measure the shape's box); returns the paint that refers to it. The same gradient
    // used again (a fill and its outline's strokes) is written once.
    auto gradientPaint = [&](const gradient::Placed& p) {
        const std::string id = "gradient" + std::to_string(gradients + 1);
        std::string d;
        if (p.kind == GradientKind::Linear) {
            d = "    <linearGradient id=\"" + id + "\" gradientUnits=\"userSpaceOnUse\" x1=\"" + X(p.start.x) + "\" y1=\""
                + Y(p.start.y) + "\" x2=\"" + X(p.end.x) + "\" y2=\"" + Y(p.end.y) + '"';
        }
        else {
            // A circle round the origin, moved, turned and squashed into place (y down: turned the
            // other way, and the focal point's sideways offset flips).
            d = "    <radialGradient id=\"" + id + "\" gradientUnits=\"userSpaceOnUse\" cx=\"0\" cy=\"0\" r=\""
                + num(p.radius) + '"';
            if (std::hypot(p.focalLocal.x, p.focalLocal.y) > 1e-9) {
                d += " fx=\"" + num(p.focalLocal.x) + "\" fy=\"" + num(-p.focalLocal.y) + '"';
            }
            d += " gradientTransform=\"translate(" + X(p.center.x) + ' ' + Y(p.center.y) + ')';
            if (std::abs(p.angle) > 1e-12) {
                d += " rotate(" + num(-p.angle * 180 / kPi) + ')';
            }
            if (std::abs(p.aspect - 1.0) > 1e-12) {
                d += " scale(1 " + num(p.aspect) + ')';
            }
            d += '"';
        }
        if (p.spread != GradientSpread::Pad) {
            d += std::string(" spreadMethod=\"") + toString(p.spread) + '"';
        }
        d += ">\n";
        for (const GradientStop& stop : p.stops) {
            d += "      <stop offset=\"" + num(stop.offset) + "\" stop-color=\"" + rgb(stop.color) + '"';
            if (stop.color.a != 255) {
                d += " stop-opacity=\"" + num(stop.color.a / 255.0) + '"';
            }
            d += "/>\n";
        }
        d += p.kind == GradientKind::Linear ? "    </linearGradient>\n" : "    </radialGradient>\n";
        std::string key = d;
        key.replace(key.find(id), id.size(), "");
        if (auto it = written.find(key); it != written.end()) {
            return "url(#" + it->second + ")";
        }
        ++gradients;
        written.emplace(std::move(key), id);
        defs += d;
        return "url(#" + id + ")";
    };
    // fill="..." or stroke="..." (with its opacity), in a colour or a gradient.
    // (A gradient whose geometry is gone: its average colour.)
    auto paintAttrs = [&](const char* what, Color color, const Gradient& g) {
        if (g.active()) {
            if (const auto placed = gradient::place(g, s)) {
                return std::string(" ") + what + "=\"" + gradientPaint(*placed) + '"';
            }
            color = g.average();
        }
        std::string a = std::string(" ") + what + "=\"" + rgb(color) + '"';
        if (color.a != 255) {
            a += std::string(" ") + what + "-opacity=\"" + num(color.a / 255.0) + '"';
        }
        return a;
    };

    // A hatch as a <pattern> of its lines (where they repeat in a tile; see hatchTile), the tile
    // laid along its first family's lines; returns the paint that refers to it, or nothing.
    int patterns = 0;
    auto hatchPaint = [&](const Hatch& h, Color color) -> std::optional<std::string> {
        if (hatch::spacing(h) < 0.01) {
            return std::nullopt;  // (too fine to be lines at all: filled solid, as on the canvas)
        }
        const std::optional<HatchTile> tile = hatchTile(h);
        if (!tile) {
            return std::nullopt;
        }
        const double along = std::hypot(tile->a.x, tile->a.y);
        const Vec2 u = mul(tile->a, 1.0 / along), n{-u.y, u.x};
        const double high = dot(tile->b, n);  // (the tile's height across the lines)
        if (!(along > 1e-9) || !(high > 1e-9)) {
            return std::nullopt;
        }
        // Its lines over the tile (and a little round it: the tile cuts them).
        const double w = h.width > 0 ? h.width : kHairline;
        double bx0 = 1e300, by0 = 1e300, bx1 = -1e300, by1 = -1e300;
        for (const Vec2& c : {tile->origin, add(tile->origin, tile->a), add(tile->origin, tile->b),
                              add(add(tile->origin, tile->a), tile->b)}) {
            bx0 = std::min(bx0, c.x - w);
            by0 = std::min(by0, c.y - w);
            bx1 = std::max(bx1, c.x + w);
            by1 = std::max(by1, c.y + w);
        }
        std::vector<hatch::Segment> pieces;
        if (!hatch::across(h, bx0, by0, bx1, by1, 4000, pieces) || pieces.empty()) {
            return std::nullopt;
        }
        // Pattern space: x along the lines, y across them (the tile's b over its height); in the
        // SVG (y down), so the transform flips.
        const Vec2 bn = mul(tile->b, 1.0 / high);
        const double det = cross(u, bn);
        auto local = [&](Vec2 p) {
            const Vec2 d = sub(p, tile->origin);
            return Vec2{cross(d, bn) / det, cross(u, d) / det};
        };
        std::string d;
        for (const hatch::Segment& p : pieces) {
            const Vec2 a = local(p.a), b = local(p.b);
            d += "M" + num(a.x) + ',' + num(a.y)
                + (p.a.x == p.b.x && p.a.y == p.b.y ? std::string(" l0,0 ") : " L" + num(b.x) + ',' + num(b.y) + ' ');
        }
        d.pop_back();
        const std::string id = "hatch" + std::to_string(++patterns);
        defs += "    <pattern id=\"" + id + "\" patternUnits=\"userSpaceOnUse\" x=\"0\" y=\"0\" width=\"" + num(along)
            + "\" height=\"" + num(high) + "\" patternTransform=\"matrix(" + num(u.x) + ' ' + num(-u.y) + ' ' + num(bn.x)
            + ' ' + num(-bn.y) + ' ' + X(tile->origin.x) + ' ' + Y(tile->origin.y) + ")\">\n      <path d=\"" + d
            + "\" fill=\"none\" stroke=\"" + rgb(color) + '"'
            + (color.a < 255 ? " stroke-opacity=\"" + num(color.a / 255.0) + '"' : std::string()) + " stroke-width=\""
            + num(w) + "\" stroke-linecap=\"round\"/>\n    </pattern>\n";
        return "url(#" + id + ")";
    };

    // Each layer is an Inkscape layer: its fills, then its sublayers (groups, by name).
    std::string indent = "  ";
    std::string prefix;
    auto group = [&](const std::string& id, const std::string& label) {
        out << indent << "<g id=\"" << prefix << id << "\" inkscape:label=\"" << xmlEscape(label) << "\">\n";
    };
    auto close = [&] { out << indent << "</g>\n"; };

    auto writeCurve = [&](const Item& item) {
        const StrokeStyle& st = item.style;
        const double w = st.width > 0.0 ? st.width : kHairline;
        std::string style = " fill=\"none\"" + paintAttrs("stroke", st.color, st.gradient);
        style += " stroke-width=\"" + num(w) + '"';
        const std::vector<double> dashes = dashPattern(st.dash);
        if (dashes.empty()) {
            style += " stroke-linecap=\"round\" stroke-linejoin=\"round\"";
        }
        else {
            const double unit = std::max(w, 0.25);  // hairline dashes stay visible
            style += " stroke-linecap=\"butt\" stroke-dasharray=\"";
            for (size_t i = 0; i < dashes.size(); ++i) {
                style += (i ? "," : "") + num(dashes[i] * unit);
            }
            style += '"';
        }

        const Entity& e = s.entity(item.curve);
        if (const auto* l = std::get_if<Line>(&e.geom)) {
            const Point& a = s.point(l->p1);
            const Point& b = s.point(l->p2);
            out << indent << "  <line x1=\"" << X(a.x) << "\" y1=\"" << Y(a.y) << "\" x2=\"" << X(b.x) << "\" y2=\""
                << Y(b.y) << '"' << style << "/>\n";
        }
        else if (const auto* c = std::get_if<Circle>(&e.geom)) {
            const Point& centre = s.point(c->center);
            out << indent << "  <circle cx=\"" << X(centre.x) << "\" cy=\"" << Y(centre.y) << "\" r=\"" << num(c->radius)
                << '"' << style << "/>\n";
        }
        else if (s.isElliptic(item.curve)) {
            // Elliptical arc commands (two halves for a whole ellipse). Counter-clockwise with y up
            // is clockwise with y down (sweep-flag 0), and angles turn the other way.
            const curve::Path p = curve::pathOf(s, item.curve);
            const curve::EllipseFrame& f = p.ellipseShape;
            const std::string radii = num(f.a) + ',' + num(f.b) + ' ' + num(-std::atan2(f.u.y, f.u.x) * 180 / kPi);
            const Vec2 a = p.at(0);
            std::string d = "M" + X(a.x) + ',' + Y(a.y);
            const int halves = p.closed() ? 2 : 1;
            for (int h = 1; h <= halves; ++h) {
                const double to = p.end() * h / halves;
                const Vec2 b = p.at(to);
                d += " A" + radii + ' ' + (p.end() / halves > kPi ? "1" : "0") + ",0 " + X(b.x) + ',' + Y(b.y);
            }
            out << indent << "  <path d=\"" << d << (p.closed() ? " Z" : "") << '"' << style << "/>\n";
        }
        else if (e.kind() == EntityKind::Spline) {
            const curve::Path p = curve::pathOf(s, item.curve);
            const std::vector<curve::Bezier> pieces = p.beziers();
            std::string d;
            for (size_t i = 0; i < pieces.size(); ++i) {
                const auto& q = pieces[i].p;
                if (i == 0) {
                    d += "M" + X(q[0].x) + ',' + Y(q[0].y);
                }
                d += " C" + X(q[1].x) + ',' + Y(q[1].y) + ' ' + X(q[2].x) + ',' + Y(q[2].y) + ' ' + X(q[3].x) + ','
                    + Y(q[3].y);
            }
            out << indent << "  <path d=\"" << d << (p.closed() ? " Z" : "") << '"' << style << "/>\n";
        }
        else {
            // Counter-clockwise with y up is clockwise with y down: SVG's sweep-flag 0.
            const ArcShape a = arcShape(s, item.curve);
            const double end = a.startAngle + a.sweep;
            out << indent << "  <path d=\"M" << X(a.center.x + a.radius * std::cos(a.startAngle)) << ','
                << Y(a.center.y + a.radius * std::sin(a.startAngle)) << " A" << num(a.radius) << ','
                << num(a.radius) << " 0 " << (a.sweep > kPi ? 1 : 0) << ",0 "
                << X(a.center.x + a.radius * std::cos(end)) << ',' << Y(a.center.y + a.radius * std::sin(end))
                << '"' << style << "/>\n";
        }
    };

    for (const Group& g : groups) {
        out << "  <g id=\"layer" << g.layer->id << "\" inkscape:groupmode=\"layer\" inkscape:label=\""
            << xmlEscape(g.layer->name) << "\">\n";
        indent = "    ";
        prefix = "layer" + std::to_string(g.layer->id) + "-";
        if (!g.fills.empty()) {
            group("fills", "Fills");
            for (const Area& a : g.fills) {
                // A hatch: its tile as a pattern; else its lines, cut exactly to the area (dots as
                // round caps on no length).
                std::optional<std::string> pattern =
                    a.hatch.active() && !o.hatchLines ? hatchPaint(a.hatch, a.color) : std::nullopt;
                std::vector<hatch::Segment> pieces;
                if (a.hatch.active() && !pattern && hatch::within(a.hatch, a.region, 2000000, pieces)) {
                    std::string lines;
                    for (const hatch::Segment& p : pieces) {
                        lines += "M" + X(p.a.x) + ',' + Y(p.a.y) + (p.a.x == p.b.x && p.a.y == p.b.y ? std::string(" l0,0 ") : " L" + X(p.b.x) + ',' + Y(p.b.y) + ' ');
                    }
                    if (!lines.empty()) {
                        lines.pop_back();
                        out << indent << "  <path d=\"" << lines << "\" fill=\"none\" stroke=\"" << rgb(a.color) << '"'
                            << (a.color.a < 255 ? " stroke-opacity=\"" + num(a.color.a / 255.0) + '"' : std::string())
                            << " stroke-width=\"" << num(a.hatch.width > 0 ? a.hatch.width : kHairline)
                            << "\" stroke-linecap=\"round\"/>\n";
                    }
                    continue;
                }
                std::string d;
                const std::vector<Along>& along = curvesOf(g.layer->id);
                auto loop = [&](const std::vector<Vec2>& pts) {
                    if (const std::string exact = exactLoop(pts, along, xy); !exact.empty()) {
                        d += exact + ' ';
                        return;
                    }
                    for (size_t i = 0; i < pts.size(); ++i) {
                        d += (i == 0 ? "M" : "L") + X(pts[i].x) + ',' + Y(pts[i].y) + ' ';
                    }
                    d += "Z ";
                };
                loop(a.region.outer);
                for (const auto& hole : a.region.holes) {
                    loop(hole);
                }
                d.pop_back();
                out << indent << "  <path d=\"" << d << '"'
                    << (pattern ? " fill=\"" + *pattern + '"' : paintAttrs("fill", a.color, a.gradient))
                    << " fill-rule=\"evenodd\" stroke=\"none\"/>\n";
            }
            close();
        }

        for (const Sub& sub : g.subs) {
            if (sub.empty()) {
                continue;
            }
            group("sublayer" + std::to_string(sub.sublayer->id), sub.sublayer->name);
            for (const TextArea& t : sub.texts) {
                if (t.asText) {
                    // Each run (a line, or a letter along a path) as a <text>, placed and turned.
                    const Text& tx = *t.text;
                    std::string font = " font-family=\"'" + xmlEscape(tx.font) + "'\"";
                    if (tx.bold) {
                        font += " font-weight=\"bold\"";
                    }
                    if (tx.italic) {
                        font += " font-style=\"italic\"";
                    }
                    const std::string paint = paintAttrs("fill", t.color, t.gradient);
                    for (const TextRun& r : tx.runs) {
                        const Point w = textToWorld(s.point(tx.anchor), tx.angle, r.at);
                        const double turn = tx.angle + r.angle;
                        out << indent << "  <text xml:space=\"preserve\" x=\"0\" y=\"0\" transform=\"translate(" << X(w.x)
                            << ' ' << Y(w.y) << ')' << (std::abs(turn) > 1e-9 ? " rotate(" + num(-turn) + ')' : std::string())
                            << '"' << font << " font-size=\"" << num(r.size) << '"'
                            << (r.tracking != 0.0 ? " letter-spacing=\"" + num(r.tracking) + '"' : std::string()) << paint
                            << '>' << xmlEscape(r.text) << "</text>\n";
                    }
                    continue;
                }
                std::string d;
                for (const auto& contour : t.contours) {
                    if (contour.points.empty()) {
                        continue;
                    }
                    auto at = [&](size_t i) { return X(contour.points[i].x) + ',' + Y(contour.points[i].y); };
                    d += "M" + at(0);
                    size_t i = 1;
                    for (char op : contour.ops) {
                        const size_t n = op == 'C' ? 3 : 1;
                        if (i + n > contour.points.size()) {
                            break;
                        }
                        Point q;
                        if (op == 'C' && quadraticOf(contour.points[i - 1], contour.points[i], contour.points[i + 1], contour.points[i + 2], q)) {
                            d += " Q" + X(q.x) + ',' + Y(q.y) + ' ' + at(i + 2);  // (a point shorter)
                        }
                        else {
                            d += op == 'C' ? " C" + at(i) + ' ' + at(i + 1) + ' ' + at(i + 2) : " L" + at(i);
                        }
                        i += n;
                    }
                    d += " Z ";
                }
                if (d.empty()) {
                    continue;
                }
                d.pop_back();
                out << indent << "  <path d=\"" << d << '"' << paintAttrs("fill", t.color, t.gradient)
                    << " fill-rule=\"nonzero\" stroke=\"none\"/>\n";
            }
            for (const Item& item : sub.strokes) {
                writeCurve(item);
            }
            for (const Item& item : sub.plain) {
                writeCurve(item);
            }
            if (!sub.construction.empty()) {
                const std::string outer = indent;
                indent += "  ";
                group("sublayer" + std::to_string(sub.sublayer->id) + "-construction", "Construction");
                for (const Item& item : sub.construction) {
                    writeCurve(item);
                }
                close();
                indent = outer;
            }
            close();
        }
        out << "  </g>\n";
    }
    out << "</svg>\n";
    return head + (defs.empty() ? std::string() : "  <defs>\n" + defs + "  </defs>\n") + out.str();
}
// ---------------------------------------------------------------------------------------------
// DXF
// ---------------------------------------------------------------------------------------------

namespace
{
// The AutoCAD colour index palette (index -> RGB).
std::array<Color, 256> aciPalette()
{
    std::array<Color, 256> p{};
    const Color basic[10] = {{0, 0, 0, 255},       {255, 0, 0, 255},     {255, 255, 0, 255}, {0, 255, 0, 255},
                             {0, 255, 255, 255},   {0, 0, 255, 255},     {255, 0, 255, 255}, {255, 255, 255, 255},
                             {128, 128, 128, 255}, {192, 192, 192, 255}};
    for (int i = 0; i < 10; ++i) {
        p[static_cast<size_t>(i)] = basic[i];
    }
    // 10-249: 24 hues in 15 degree steps; in each, five brightnesses, full then half saturation.
    const double values[5] = {1.0, 0.8, 0.6, 0.5, 0.3};
    for (int i = 10; i < 250; ++i) {
        const double h = (i / 10 - 1) * 15.0;
        const double v = values[(i % 10) / 2];
        const double sat = i % 2 == 0 ? 1.0 : 0.5;
        const double c = v * sat;
        const double x = c * (1 - std::abs(std::fmod(h / 60.0, 2.0) - 1));
        const double m = v - c;
        double r = 0, g = 0, b = 0;
        if (h < 60) {
            r = c, g = x;
        }
        else if (h < 120) {
            r = x, g = c;
        }
        else if (h < 180) {
            g = c, b = x;
        }
        else if (h < 240) {
            g = x, b = c;
        }
        else if (h < 300) {
            r = x, b = c;
        }
        else {
            r = c, b = x;
        }
        auto byte = [](double u) { return static_cast<std::uint8_t>(std::lround(u * 255)); };
        p[static_cast<size_t>(i)] = Color{byte(r + m), byte(g + m), byte(b + m), 255};
    }
    const std::uint8_t greys[6] = {51, 80, 105, 130, 190, 255};
    for (int i = 0; i < 6; ++i) {
        p[static_cast<size_t>(250 + i)] = Color{greys[i], greys[i], greys[i], 255};
    }
    return p;
}

// R12 linetypes for the dash styles (pattern lengths in mm, for lines around 0.5 mm wide).
struct LineType
{
    const char* name;
    const char* description;
    std::vector<double> pattern;  // dashes positive, gaps negative
};
const LineType kLineTypes[] = {
    {"CONTINUOUS", "Solid line", {}},
    {"DASHED", "Dashed __ __ __", {2.5, -1.5}},
    {"DOT", "Dotted . . . .", {0.0, -1.0}},
    {"DASHDOT", "Dash dot __ . __ .", {4.0, -1.5, 0.0, -1.5}},
    {"DIVIDE", "Dash dot dot __ . . __", {4.0, -1.5, 0.0, -1.5, 0.0, -1.5}},
};

const char* lineTypeFor(Dash dash)
{
    switch (dash) {
        case Dash::Solid: return "CONTINUOUS";
        case Dash::Dashed: return "DASHED";
        case Dash::Dotted: return "DOT";
        case Dash::DashDot: return "DASHDOT";
        case Dash::DashDotDot: return "DIVIDE";
    }
    return "CONTINUOUS";
}

// DXF is lines of group code, then value.
class DxfWriter
{
public:
    void pair(int code, const std::string& value) { m_out << code << "\r\n" << value << "\r\n"; }
    void pair(int code, int value) { pair(code, std::to_string(value)); }
    void real(int code, double value) { pair(code, numFine(value)); }
    void point(int code, Vec2 p)  // x, y, z (as 10, 20, 30)
    {
        real(code, p.x);
        real(code + 10, p.y);
        real(code + 20, 0.0);
    }
    std::string str() const { return m_out.str(); }

    // Up to 9 decimals (curves' knots and control points want more than 4).
    static std::string numFine(double v)
    {
        char buf[64];
        std::snprintf(buf, sizeof buf, "%.9f", v);
        std::string s = buf;
        if (s.find('.') != std::string::npos) {
            while (s.back() == '0') {
                s.pop_back();
            }
            if (s.back() == '.') {
                s.pop_back();
            }
        }
        return s == "-0" ? "0" : s;
    }

private:
    std::ostringstream m_out;
};

std::string hex(unsigned v)
{
    char buf[16];
    std::snprintf(buf, sizeof buf, "%X", v);
    return buf;
}

// The code points of UTF-8 text (a bad byte as itself).
std::vector<char32_t> codePoints(const std::string& s)
{
    std::vector<char32_t> out;
    for (size_t i = 0; i < s.size();) {
        const auto c = static_cast<unsigned char>(s[i]);
        const int n = c < 0x80 ? 1 : (c >> 5) == 6 ? 2 : (c >> 4) == 14 ? 3 : (c >> 3) == 30 ? 4 : 1;
        char32_t cp = n == 1 ? c : n == 2 ? (c & 0x1F) : n == 3 ? (c & 0x0F) : (c & 0x07);
        for (int k = 1; k < n && i + k < s.size(); ++k) {
            cp = (cp << 6) | (static_cast<unsigned char>(s[i + k]) & 0x3F);
        }
        out.push_back(cp);
        i += static_cast<size_t>(n);
    }
    return out;
}

// Text for DXF: ASCII as itself, anything else as \U+XXXX (the file's code page is ANSI).
std::string dxfText(const std::vector<char32_t>& cps)
{
    std::string out;
    for (char32_t c : cps) {
        if (c >= 0x20 && c < 0x7F) {
            out += static_cast<char>(c);
        }
        else if (c >= 0x80) {
            char buf[16];
            std::snprintf(buf, sizeof buf, "\\U+%04X", static_cast<unsigned>(c));
            out += buf;
        }
    }
    return out;
}

// The nearest of AutoCAD's line weights (hundredths of a mm) to a width in mm.
int lineWeight(double mm)
{
    static const int weights[] = {0, 5, 9, 13, 15, 18, 20, 25, 30, 35, 40, 50, 53, 60, 70, 80, 90, 100, 106, 120, 140, 158, 200, 211};
    const double want = mm * 100.0;
    return *std::ranges::min_element(weights, {}, [&](int w) { return std::abs(w - want); });
}

// Cubic Bezier pieces, end to end, as one clamped degree-3 B-spline: its control points and knots.
void bezierSpline(const std::vector<curve::Bezier>& pieces, std::vector<Vec2>& controls, std::vector<double>& knots)
{
    controls.clear();
    knots.clear();
    if (pieces.empty()) {
        return;
    }
    controls.push_back(pieces.front().p[0]);
    for (const curve::Bezier& b : pieces) {
        controls.insert(controls.end(), {b.p[1], b.p[2], b.p[3]});
    }
    const size_t n = pieces.size();
    knots.assign(4, 0.0);
    for (size_t k = 1; k < n; ++k) {
        knots.insert(knots.end(), 3, static_cast<double>(k));
    }
    knots.insert(knots.end(), 4, static_cast<double>(n));
}
}  // namespace

Color aciColor(int index)
{
    static const std::array<Color, 256> palette = aciPalette();
    return index >= 1 && index <= 255 ? palette[static_cast<size_t>(index)] : Color{0, 0, 0, 255};
}

int nearestAci(const Color& c)
{
    // Black and white both come out as 7, which CAD shows contrasting with its background.
    const int hi = std::max({c.r, c.g, c.b}), lo = std::min({c.r, c.g, c.b});
    if (hi < 40 || lo > 230) {
        return 7;
    }
    static const std::array<Color, 256> palette = aciPalette();
    int best = 1;
    long bestDistance = -1;
    for (int i = 1; i < 256; ++i) {
        if (i == 7) {
            continue;
        }
        const Color& p = palette[static_cast<size_t>(i)];
        const long dr = c.r - p.r, dg = c.g - p.g, db = c.b - p.b;
        const long d = dr * dr + dg * dg + db * db;
        if (bestDistance < 0 || d < bestDistance) {
            bestDistance = d;
            best = i;
        }
    }
    return best;
}

std::string toDxf(const Sketch& s, const DxfOptions& o)
{
    // How an entity looks: its line type, colour (AutoCAD's index, and the true colour), weight.
    struct Look
    {
        const char* lineType = nullptr;  // null: the layer's
        int aci = 256;                   // 256: the layer's
        std::optional<Color> color;      // a true colour too
        int weight = -1;                 // -1: the layer's
    };
    auto lookOf = [&](Color c) {
        Look l;
        if (o.styles) {
            l.aci = nearestAci(c);
            l.color = c;
        }
        return l;
    };

    // --- what goes in -------------------------------------------------------------------------
    struct Item
    {
        Id curve = kNoId;
        const char* layer = "GEOMETRY";
        Look look;
    };
    std::vector<Item> items;
    for (const auto& [id, e] : s.entities()) {
        if (e.kind() == EntityKind::Point || s.isReference(id) || !layerShown(s, id)) {
            continue;
        }
        if (e.construction) {
            if (o.construction) {
                items.push_back({id, "CONSTRUCTION", {}});
            }
        }
        else if (const StrokeStyle* style = s.stroke(id); style && o.styles && !style->none()) {
            // (DXF lines are one colour: a gradient's average)
            Look l = lookOf(style->gradient.active() ? style->gradient.average() : style->color);
            l.lineType = lineTypeFor(style->dash);
            l.weight = lineWeight(style->width);
            items.push_back({id, "STROKES", l});
        }
        else {
            items.push_back({id, "GEOMETRY", {}});
        }
    }
    struct TextItem
    {
        const Text* text;
        bool asText;
        Look look;
    };
    std::vector<TextItem> texts;
    for (const auto& [id, t] : s.texts()) {
        if (!s.hasEntity(t.anchor) || !layerShown(s, id) || (t.letters.empty() && t.shape.empty() && t.runs.empty())) {
            continue;
        }
        Look l;
        if (t.hasColor) {
            l = lookOf(t.gradient.active() ? t.gradient.average() : t.color);
        }
        texts.push_back({&t, asText(t, o.textAsOutlines), l});
    }
    struct FillItem
    {
        const Fill* fill;
        regions::Region region;
        Look look;
    };
    std::vector<FillItem> fills;
    if (o.fills) {
        const regions::Arrangements arrangements(s, kPi / 720);
        for (const auto& [id, f] : s.fills()) {
            if (s.hasLayer(f.layer) && !s.layer(f.layer).visible) {
                continue;
            }
            if (auto region = regions::resolve(arrangements, f)) {
                const Color c = f.gradient.active() && !f.hatch.active() ? f.gradient.average() : f.color;
                Look l;  // (a fill's colour is all it has: kept even without styles)
                l.aci = nearestAci(c);
                l.color = c;
                if (f.hatch.active() && o.styles) {
                    l.weight = lineWeight(f.hatch.width);
                }
                fills.push_back({&f, std::move(*region), l});
            }
        }
    }
    if (items.empty() && texts.empty() && fills.empty()) {
        throw SketchError("there's nothing to export");
    }

    // --- its extent ---------------------------------------------------------------------------
    double x0 = 1e300, y0 = 1e300, x1 = -1e300, y1 = -1e300;
    auto include = [&](Vec2 p) {
        x0 = std::min(x0, p.x);
        y0 = std::min(y0, p.y);
        x1 = std::max(x1, p.x);
        y1 = std::max(y1, p.y);
    };
    for (const Item& item : items) {
        for (const Vec2& p : sample(s, item.curve, 256)) {
            include(p);
        }
    }
    for (const TextItem& t : texts) {
        for (const auto& poly : t.text->shape) {
            for (const Point& p : poly) {
                const Point w = textToWorld(s.point(t.text->anchor), t.text->angle, p);
                include({w.x, w.y});
            }
        }
    }
    for (const FillItem& f : fills) {
        for (const Vec2& p : f.region.outer) {
            include(p);
        }
    }
    if (x0 > x1) {
        x0 = y0 = x1 = y1 = 0.0;
    }

    // --- handles: fixed ones first (tables and objects refer to each other) ---------------------
    unsigned next = 1;
    auto handle = [&] { return hex(next++); };
    const std::string hVportTable = handle(), hLtypeTable = handle(), hLayerTable = handle(), hStyleTable = handle(),
                      hViewTable = handle(), hUcsTable = handle(), hAppidTable = handle(), hDimstyleTable = handle(),
                      hBlockRecordTable = handle();
    const std::string hRoot = handle(), hGroups = handle(), hLayouts = handle(), hPlotStyles = handle(),
                      hNormal = handle();
    const std::string hModel = handle(), hPaper = handle(), hModelLayout = handle(), hPaperLayout = handle();

    // --- text styles: one per font (and weight and slant) ----------------------------------------
    struct Style
    {
        std::string name, family;
        bool bold = false, italic = false;
    };
    std::vector<Style> styles{{"Standard", "", false, false}};
    auto styleFor = [&](const Text& t) -> const std::string& {
        for (const Style& st : styles) {
            if (st.family == t.font && st.bold == t.bold && st.italic == t.italic) {
                return st.name;
            }
        }
        std::string name = t.font + (t.bold ? " Bold" : "") + (t.italic ? " Italic" : "");
        for (char& c : name) {
            if (std::string_view("<>/\\\":;?*|,=`").find(c) != std::string_view::npos || static_cast<unsigned char>(c) >= 0x80) {
                c = '_';
            }
        }
        styles.push_back({name, t.font, t.bold, t.italic});
        return styles.back().name;
    };
    for (const TextItem& t : texts) {
        if (t.asText) {
            styleFor(*t.text);
        }
    }

    // --- entities ---------------------------------------------------------------------------
    DxfWriter e;
    auto begin = [&](const char* type, const char* layer, const Look& look) {
        e.pair(0, type);
        e.pair(5, handle());
        e.pair(330, hModel);
        e.pair(100, "AcDbEntity");
        e.pair(8, layer);
        if (look.lineType) {
            e.pair(6, look.lineType);
        }
        if (look.aci != 256) {
            e.pair(62, look.aci);
        }
        if (look.color) {
            e.pair(420, (look.color->r << 16) | (look.color->g << 8) | look.color->b);
        }
        if (look.weight >= 0) {
            e.pair(370, look.weight);
        }
    };
    auto writeSpline = [&](const std::vector<curve::Bezier>& pieces, const char* layer, const Look& look) {
        std::vector<Vec2> controls;
        std::vector<double> knots;
        bezierSpline(pieces, controls, knots);
        if (controls.size() < 4) {
            return;
        }
        begin("SPLINE", layer, look);
        e.pair(100, "AcDbSpline");
        e.real(210, 0.0);
        e.real(220, 0.0);
        e.real(230, 1.0);
        e.pair(70, 8);  // planar
        e.pair(71, 3);
        e.pair(72, static_cast<int>(knots.size()));
        e.pair(73, static_cast<int>(controls.size()));
        e.pair(74, 0);
        e.real(42, 1e-10);
        e.real(43, 1e-10);
        for (double k : knots) {
            e.real(40, k);
        }
        for (const Vec2& c : controls) {
            e.point(10, c);
        }
    };
    auto writeLoopPolyline = [&](const std::vector<Vec2>& pts, const char* layer, const Look& look) {
        begin("LWPOLYLINE", layer, look);
        e.pair(100, "AcDbPolyline");
        e.pair(90, static_cast<int>(pts.size()));
        e.pair(70, 1);  // closed
        for (const Vec2& p : pts) {
            e.real(10, p.x);
            e.real(20, p.y);
        }
    };

    // Fills first, so the lines are drawn over them.
    std::map<Id, std::vector<Along>> alongByLayer;
    for (const FillItem& f : fills) {
        const Hatch& h = f.fill->hatch;
        const bool hatched = h.active();
        begin("HATCH", "FILLS", f.look);
        e.pair(100, "AcDbHatch");
        e.point(10, {0, 0});
        e.real(210, 0.0);
        e.real(220, 0.0);
        e.real(230, 1.0);
        std::string name = hatched ? (h.pattern.empty() ? std::string("USER") : h.pattern) : std::string("SOLID");
        for (char& c : name) {
            if (c == ' ' || static_cast<unsigned char>(c) >= 0x80) {
                c = '_';
            }
        }
        e.pair(2, name);
        e.pair(70, hatched ? 0 : 1);
        e.pair(71, 0);
        std::vector<const std::vector<Vec2>*> loops{&f.region.outer};
        for (const auto& hole : f.region.holes) {
            loops.push_back(&hole);
        }
        e.pair(91, static_cast<int>(loops.size()));
        auto curvesOf = [&](Id layer) -> const std::vector<Along>& {
            auto it = alongByLayer.find(layer);
            if (it == alongByLayer.end()) {
                it = alongByLayer.emplace(layer, curvesAlong(s, layer)).first;
            }
            return it->second;
        };
        auto norm360 = [](double deg) {
            const double d = std::fmod(deg, 360.0);
            return d < 0 ? d + 360.0 : d;
        };
        for (size_t k = 0; k < loops.size(); ++k) {
            // Its edges exactly (lines, arcs, elliptical arcs, splines: as the curves along it),
            // else as a polyline through its points. (The first: the outside.)
            const std::optional<std::vector<Edge>> edges = loopEdges(*loops[k], curvesOf(f.fill->layer));
            if (!edges || edges->empty()) {
                e.pair(92, k == 0 ? 3 : 2);
                e.pair(72, 0);
                e.pair(73, 1);
                e.pair(93, static_cast<int>(loops[k]->size()));
                for (const Vec2& p : *loops[k]) {
                    e.real(10, p.x);
                    e.real(20, p.y);
                }
                e.pair(97, 0);
                continue;
            }
            e.pair(92, k == 0 ? 1 : 0);
            e.pair(93, static_cast<int>(edges->size()));
            for (const Edge& edge : *edges) {
                switch (edge.kind) {
                    case Edge::Kind::Line:
                        e.pair(72, 1);
                        e.real(10, edge.from.x);
                        e.real(20, edge.from.y);
                        e.real(11, edge.to.x);
                        e.real(21, edge.to.y);
                        break;
                    case Edge::Kind::Arc:
                    case Edge::Kind::Ellipse: {
                        // Counter-clockwise as it is; clockwise as DXF keeps it, mirrored.
                        double a0 = edge.a0, a1 = edge.a0 + edge.sweep;
                        Vec2 major = mul(edge.f.u, edge.f.a);
                        double ratio = edge.f.a > 0 ? edge.f.b / edge.f.a : 1.0;
                        if (edge.kind == Edge::Kind::Ellipse && edge.f.b > edge.f.a) {
                            major = mul(Vec2{-edge.f.u.y, edge.f.u.x}, edge.f.b);  // (the longer axis)
                            ratio = edge.f.a / edge.f.b;
                            a0 -= kPi / 2;
                            a1 -= kPi / 2;
                        }
                        const bool ccw = edge.sweep > 0;
                        e.pair(72, edge.kind == Edge::Kind::Arc ? 2 : 3);
                        e.real(10, edge.kind == Edge::Kind::Arc ? edge.c.x : edge.f.c.x);
                        e.real(20, edge.kind == Edge::Kind::Arc ? edge.c.y : edge.f.c.y);
                        if (edge.kind == Edge::Kind::Arc) {
                            e.real(40, edge.r);
                        }
                        else {
                            e.real(11, major.x);
                            e.real(21, major.y);
                            e.real(40, ratio);
                        }
                        e.real(50, norm360((ccw ? a0 : -a0) * 180 / kPi));
                        e.real(51, norm360((ccw ? a1 : -a1) * 180 / kPi));
                        e.pair(73, ccw ? 1 : 0);
                        break;
                    }
                    case Edge::Kind::Bezier: {
                        std::vector<Vec2> controls;
                        std::vector<double> knots;
                        bezierSpline(edge.pieces, controls, knots);
                        e.pair(72, 4);
                        e.pair(94, 3);
                        e.pair(73, 0);
                        e.pair(74, 0);
                        e.pair(95, static_cast<int>(knots.size()));
                        e.pair(96, static_cast<int>(controls.size()));
                        for (double kn : knots) {
                            e.real(40, kn);
                        }
                        for (const Vec2& c : controls) {
                            e.real(10, c.x);
                            e.real(20, c.y);
                        }
                        break;
                    }
                }
            }
            e.pair(97, 0);
        }
        e.pair(75, 0);  // odd parity: holes are holes
        if (hatched) {
            e.pair(76, hatch::builtin(h.pattern) ? 1 : 2);
            e.real(52, h.angle);
            e.real(41, h.scale);
            e.pair(77, 0);
            const std::vector<HatchLine> lines = hatch::placed(h);
            e.pair(78, static_cast<int>(lines.size()));
            for (const HatchLine& l : lines) {
                const double r = l.angle * kPi / 180;
                const Vec2 u{std::cos(r), std::sin(r)}, n{-u.y, u.x};
                const Vec2 offset = add(mul(u, l.dx), mul(n, l.dy));
                e.real(53, l.angle);
                e.real(43, l.x);
                e.real(44, l.y);
                e.real(45, offset.x);
                e.real(46, offset.y);
                e.pair(79, static_cast<int>(l.dashes.size()));
                for (double d : l.dashes) {
                    e.real(49, d);
                }
            }
        }
        else {
            e.pair(76, 1);
        }
        e.pair(98, 0);
    }

    // The geometry, exactly: DXF is y up and arcs run counter-clockwise, as here.
    auto degrees = [](double r) {
        double d = std::fmod(r * 180.0 / kPi, 360.0);
        return d < 0 ? d + 360.0 : d;
    };
    for (const Item& item : items) {
        const Entity& ent = s.entity(item.curve);
        if (const auto* l = std::get_if<Line>(&ent.geom)) {
            const Point& a = s.point(l->p1);
            const Point& b = s.point(l->p2);
            begin("LINE", item.layer, item.look);
            e.pair(100, "AcDbLine");
            e.point(10, {a.x, a.y});
            e.point(11, {b.x, b.y});
        }
        else if (const auto* c = std::get_if<Circle>(&ent.geom)) {
            const Point& centre = s.point(c->center);
            begin("CIRCLE", item.layer, item.look);
            e.pair(100, "AcDbCircle");
            e.point(10, {centre.x, centre.y});
            e.real(40, c->radius);
        }
        else if (s.isElliptic(item.curve)) {
            const curve::Path p = curve::pathOf(s, item.curve);
            curve::EllipseFrame f = p.ellipseShape;
            double t0 = 0.0, t1 = 2 * kPi;
            if (!p.closed()) {
                t0 = f.param(p.at(0));
                t1 = f.param(p.at(p.end()));
            }
            // DXF wants the longer axis as the major one: else turn the frame a quarter.
            if (f.b > f.a) {
                f.u = {-f.u.y, f.u.x};
                std::swap(f.a, f.b);
                t0 -= kPi / 2;
                t1 -= kPi / 2;
            }
            if (!p.closed()) {
                while (t1 <= t0) {
                    t1 += 2 * kPi;
                }
            }
            begin("ELLIPSE", item.layer, item.look);
            e.pair(100, "AcDbEllipse");
            e.point(10, f.c);
            e.point(11, mul(f.u, f.a));
            e.real(210, 0.0);
            e.real(220, 0.0);
            e.real(230, 1.0);
            e.real(40, f.a > 0 ? f.b / f.a : 1.0);
            e.real(41, t0);
            e.real(42, t1);
        }
        else if (ent.kind() == EntityKind::Spline) {
            writeSpline(curve::pathOf(s, item.curve).beziers(), item.layer, item.look);
        }
        else {
            const ArcShape a = arcShape(s, item.curve);
            begin("ARC", item.layer, item.look);
            e.pair(100, "AcDbCircle");
            e.point(10, a.center);
            e.real(40, a.radius);
            e.pair(100, "AcDbArc");
            e.real(50, degrees(a.startAngle));
            e.real(51, degrees(a.startAngle + a.sweep));
        }
    }

    // Texts: in their font, a TEXT for each run (each letter on its own where it's spaced out,
    // which TEXT can't say); or their letters' outlines.
    for (const TextItem& ti : texts) {
        const Text& t = *ti.text;
        const Point& anchor = s.point(t.anchor);
        if (ti.asText) {
            const std::string& style = styleFor(t);
            auto writeText = [&](const std::vector<char32_t>& cps, Point at, double turn) {
                const std::string words = dxfText(cps);
                if (words.find_first_not_of(' ') == std::string::npos) {
                    return;
                }
                begin("TEXT", "TEXT", ti.look);
                e.pair(100, "AcDbText");
                e.point(10, {at.x, at.y});
                e.real(40, t.height);  // (a DXF text's height is its capitals', as here)
                e.pair(1, words);
                if (std::abs(turn) > 1e-9) {
                    e.real(50, std::fmod(std::fmod(turn, 360.0) + 360.0, 360.0));
                }
                e.pair(7, style);
                e.pair(100, "AcDbText");
            };
            for (const TextRun& r : t.runs) {
                const std::vector<char32_t> cps = codePoints(r.text);
                const double turn = t.angle + r.angle;
                if (r.tracking == 0.0 || r.glyphs.size() != cps.size()) {
                    writeText(cps, textToWorld(anchor, t.angle, r.at), turn);
                    continue;
                }
                const double a = r.angle * kPi / 180;
                for (size_t k = 0; k < cps.size(); ++k) {
                    const Point local{r.at.x + r.glyphs[k] * std::cos(a), r.at.y + r.glyphs[k] * std::sin(a)};
                    writeText({cps[k]}, textToWorld(anchor, t.angle, local), turn);
                }
            }
            continue;
        }
        // Outlines: a closed polyline where a contour is all straight, else a spline (exact).
        std::vector<TextContour> letters = t.letters;
        if (letters.empty()) {
            for (const auto& poly : t.shape) {
                letters.push_back({poly, std::string(poly.empty() ? 0 : poly.size() - 1, 'L')});
            }
        }
        for (const TextContour& c : letters) {
            if (c.points.size() < 2) {
                continue;
            }
            std::vector<Vec2> pts;
            for (const Point& p : c.points) {
                const Point w = textToWorld(anchor, t.angle, p);
                pts.push_back({w.x, w.y});
            }
            if (c.ops.find('C') == std::string::npos) {
                if (pts.size() >= 3) {
                    writeLoopPolyline(pts, "TEXT", ti.look);
                }
                continue;
            }
            std::vector<curve::Bezier> pieces;
            auto straight = [](Vec2 a, Vec2 b) {
                return curve::Bezier{{a, add(a, mul(sub(b, a), 1.0 / 3)), add(a, mul(sub(b, a), 2.0 / 3)), b}};
            };
            size_t i = 1;
            for (char op : c.ops) {
                if (op == 'C' && i + 2 < pts.size()) {
                    pieces.push_back({{pts[i - 1], pts[i], pts[i + 1], pts[i + 2]}});
                    i += 3;
                }
                else if (op == 'L' && i < pts.size()) {
                    pieces.push_back(straight(pts[i - 1], pts[i]));
                    i += 1;
                }
                else {
                    break;
                }
            }
            const Vec2 last = pts[i - 1];
            if (std::hypot(last.x - pts[0].x, last.y - pts[0].y) > 1e-9) {
                pieces.push_back(straight(last, pts[0]));  // (back to its start)
            }
            writeSpline(pieces, "TEXT", ti.look);
        }
    }
    const std::string entities = e.str();

    // --- header, tables, blocks, objects ------------------------------------------------------
    DxfWriter w;
    w.pair(0, "SECTION");
    w.pair(2, "HEADER");
    w.pair(9, "$ACADVER");
    w.pair(1, "AC1018");
    w.pair(9, "$DWGCODEPAGE");
    w.pair(3, "ANSI_1252");
    w.pair(9, "$INSBASE");
    w.point(10, {0, 0});
    w.pair(9, "$EXTMIN");
    w.point(10, {x0, y0});
    w.pair(9, "$EXTMAX");
    w.point(10, {x1, y1});
    w.pair(9, "$LIMMIN");
    w.real(10, x0);
    w.real(20, y0);
    w.pair(9, "$LIMMAX");
    w.real(10, x1);
    w.real(20, y1);
    w.pair(9, "$LTSCALE");
    w.real(40, 1.0);
    w.pair(9, "$CLAYER");
    w.pair(8, "0");
    w.pair(9, "$TEXTSTYLE");
    w.pair(7, "Standard");
    w.pair(9, "$LUNITS");
    w.pair(70, 2);
    w.pair(9, "$INSUNITS");
    w.pair(70, 4);  // millimetres
    w.pair(9, "$MEASUREMENT");
    w.pair(70, 1);  // metric
    w.pair(9, "$LWDISPLAY");
    w.pair(290, 1);  // (show line weights)
    w.pair(9, "$HANDSEED");
    const size_t seedAt = w.str().size();  // (written last, when every handle's known)
    w.pair(0, "ENDSEC");
    const std::string headerHead = w.str().substr(0, seedAt);
    const std::string headerTail = w.str().substr(seedAt);

    DxfWriter t;
    t.pair(0, "SECTION");
    t.pair(2, "CLASSES");
    for (const auto& [record, cpp] : {std::pair{"ACDBDICTIONARYWDFLT", "AcDbDictionaryWithDefault"},
                                      std::pair{"ACDBPLACEHOLDER", "AcDbPlaceHolder"}, std::pair{"LAYOUT", "AcDbLayout"}}) {
        t.pair(0, "CLASS");
        t.pair(1, record);
        t.pair(2, cpp);
        t.pair(3, "ObjectDBX Classes");
        t.pair(90, 0);
        t.pair(91, 0);
        t.pair(280, 0);
        t.pair(281, 0);
    }
    t.pair(0, "ENDSEC");

    t.pair(0, "SECTION");
    t.pair(2, "TABLES");
    auto table = [&](const char* name, const std::string& h, int count) {
        t.pair(0, "TABLE");
        t.pair(2, name);
        t.pair(5, h);
        t.pair(330, "0");
        t.pair(100, "AcDbSymbolTable");
        t.pair(70, count);
    };
    auto entry = [&](const char* type, const std::string& owner, const char* subclass, const std::string& name) {
        t.pair(0, type);
        t.pair(5, handle());
        t.pair(330, owner);
        t.pair(100, "AcDbSymbolTableRecord");
        t.pair(100, subclass);
        t.pair(2, name);
        t.pair(70, 0);
    };
    // The view: on the drawing.
    table("VPORT", hVportTable, 1);
    entry("VPORT", hVportTable, "AcDbViewportTableRecord", "*Active");
    t.point(10, {0, 0});
    t.real(11, 1.0);
    t.real(21, 1.0);
    t.real(12, (x0 + x1) / 2);
    t.real(22, (y0 + y1) / 2);
    t.real(13, 0.0);
    t.real(23, 0.0);
    t.real(14, 10.0);
    t.real(24, 10.0);
    t.real(15, 10.0);
    t.real(25, 10.0);
    t.real(16, 0.0);
    t.real(26, 0.0);
    t.real(36, 1.0);
    t.real(17, 0.0);
    t.real(27, 0.0);
    t.real(37, 0.0);
    t.real(40, std::max({y1 - y0, (x1 - x0) / 1.5, 1.0}) * 1.1);
    t.real(41, 1.5);
    t.real(42, 50.0);
    t.real(43, 0.0);
    t.real(44, 0.0);
    t.real(50, 0.0);
    t.real(51, 0.0);
    t.pair(71, 0);
    t.pair(72, 1000);
    t.pair(73, 1);
    t.pair(74, 3);
    t.pair(75, 0);
    t.pair(76, 0);
    t.pair(77, 0);
    t.pair(78, 0);
    t.pair(281, 0);
    t.pair(65, 0);
    t.real(146, 0.0);
    t.pair(0, "ENDTAB");

    table("LTYPE", hLtypeTable, 2 + static_cast<int>(std::size(kLineTypes)));
    for (const char* name : {"ByBlock", "ByLayer"}) {
        entry("LTYPE", hLtypeTable, "AcDbLinetypeTableRecord", name);
        t.pair(3, "");
        t.pair(72, 65);
        t.pair(73, 0);
        t.real(40, 0.0);
    }
    for (const LineType& lt : kLineTypes) {
        double total = 0.0;
        for (double d : lt.pattern) {
            total += std::abs(d);
        }
        entry("LTYPE", hLtypeTable, "AcDbLinetypeTableRecord", lt.name);
        t.pair(3, lt.description);
        t.pair(72, 65);  // 'A'
        t.pair(73, static_cast<int>(lt.pattern.size()));
        t.real(40, total);
        for (double d : lt.pattern) {
            t.real(49, d);
            t.pair(74, 0);
        }
    }
    t.pair(0, "ENDTAB");

    struct LayerDef
    {
        const char* name;
        int color;  // negative: switched off
        const char* lineType;
    };
    const LayerDef layers[] = {{"0", 7, "CONTINUOUS"},     {"GEOMETRY", 7, "CONTINUOUS"}, {"STROKES", 7, "CONTINUOUS"},
                               {"TEXT", 7, "CONTINUOUS"},  {"FILLS", 7, "CONTINUOUS"},    {"CONSTRUCTION", -8, "DASHED"}};
    table("LAYER", hLayerTable, static_cast<int>(std::size(layers)));
    for (const LayerDef& l : layers) {
        entry("LAYER", hLayerTable, "AcDbLayerTableRecord", l.name);
        t.pair(62, l.color);
        t.pair(6, l.lineType);
        t.pair(370, -3);
        t.pair(390, hNormal);
    }
    t.pair(0, "ENDTAB");

    table("STYLE", hStyleTable, static_cast<int>(styles.size()));
    for (const Style& st : styles) {
        entry("STYLE", hStyleTable, "AcDbTextStyleTableRecord", st.name);
        t.real(40, 0.0);
        t.real(41, 1.0);
        t.real(50, 0.0);
        t.pair(71, 0);
        t.real(42, 2.5);
        // Its font: a TrueType file (by its family's name: CAD finds it by that, in the XDATA).
        t.pair(3, st.family.empty() ? std::string("arial.ttf") : st.family + ".ttf");
        t.pair(4, "");
        if (!st.family.empty()) {
            t.pair(1001, "ACAD");
            t.pair(1000, st.family);
            t.pair(1071, (st.italic ? 0x1000000 : 0) | (st.bold ? 0x2000000 : 0) | 34);  // (34: variable pitch, Swiss)
        }
    }
    t.pair(0, "ENDTAB");

    table("VIEW", hViewTable, 0);
    t.pair(0, "ENDTAB");
    table("UCS", hUcsTable, 0);
    t.pair(0, "ENDTAB");
    table("APPID", hAppidTable, 1);
    entry("APPID", hAppidTable, "AcDbRegAppTableRecord", "ACAD");
    t.pair(0, "ENDTAB");

    table("DIMSTYLE", hDimstyleTable, 1);
    t.pair(100, "AcDbDimStyleTable");
    t.pair(0, "DIMSTYLE");
    t.pair(105, handle());
    t.pair(330, hDimstyleTable);
    t.pair(100, "AcDbSymbolTableRecord");
    t.pair(100, "AcDbDimStyleTableRecord");
    t.pair(2, "Standard");
    t.pair(70, 0);
    t.pair(0, "ENDTAB");

    table("BLOCK_RECORD", hBlockRecordTable, 2);
    for (const auto& [h, name, layout] : {std::tuple{hModel, "*Model_Space", hModelLayout}, std::tuple{hPaper, "*Paper_Space", hPaperLayout}}) {
        t.pair(0, "BLOCK_RECORD");
        t.pair(5, h);
        t.pair(330, hBlockRecordTable);
        t.pair(100, "AcDbSymbolTableRecord");
        t.pair(100, "AcDbBlockTableRecord");
        t.pair(2, name);
        t.pair(340, layout);
    }
    t.pair(0, "ENDTAB");
    t.pair(0, "ENDSEC");

    t.pair(0, "SECTION");
    t.pair(2, "BLOCKS");
    for (const auto& [owner, name, paper] : {std::tuple{hModel, "*Model_Space", false}, std::tuple{hPaper, "*Paper_Space", true}}) {
        t.pair(0, "BLOCK");
        t.pair(5, handle());
        t.pair(330, owner);
        t.pair(100, "AcDbEntity");
        if (paper) {
            t.pair(67, 1);
        }
        t.pair(8, "0");
        t.pair(100, "AcDbBlockBegin");
        t.pair(2, name);
        t.pair(70, 0);
        t.point(10, {0, 0});
        t.pair(3, name);
        t.pair(1, "");
        t.pair(0, "ENDBLK");
        t.pair(5, handle());
        t.pair(330, owner);
        t.pair(100, "AcDbEntity");
        if (paper) {
            t.pair(67, 1);
        }
        t.pair(8, "0");
        t.pair(100, "AcDbBlockEnd");
    }
    t.pair(0, "ENDSEC");
    const std::string tables = t.str();

    DxfWriter ob;
    ob.pair(0, "SECTION");
    ob.pair(2, "OBJECTS");
    auto dictionary = [&](const std::string& h, const std::string& owner,
                          std::initializer_list<std::pair<const char*, std::string>> items) {
        ob.pair(0, "DICTIONARY");
        ob.pair(5, h);
        ob.pair(330, owner);
        ob.pair(100, "AcDbDictionary");
        ob.pair(281, 1);
        for (const auto& [name, target] : items) {
            ob.pair(3, name);
            ob.pair(350, target);
        }
    };
    dictionary(hRoot, "0", {{"ACAD_GROUP", hGroups}, {"ACAD_LAYOUT", hLayouts}, {"ACAD_PLOTSTYLENAME", hPlotStyles}});
    dictionary(hGroups, hRoot, {});
    dictionary(hLayouts, hRoot, {{"Model", hModelLayout}, {"Layout1", hPaperLayout}});
    ob.pair(0, "ACDBDICTIONARYWDFLT");
    ob.pair(5, hPlotStyles);
    ob.pair(330, hRoot);
    ob.pair(100, "AcDbDictionary");
    ob.pair(281, 1);
    ob.pair(3, "Normal");
    ob.pair(350, hNormal);
    ob.pair(100, "AcDbDictionaryWithDefault");
    ob.pair(340, hNormal);
    ob.pair(0, "ACDBPLACEHOLDER");
    ob.pair(5, hNormal);
    ob.pair(330, hPlotStyles);
    for (const auto& [h, name, tab, block] : {std::tuple{hModelLayout, "Model", 0, hModel}, std::tuple{hPaperLayout, "Layout1", 1, hPaper}}) {
        ob.pair(0, "LAYOUT");
        ob.pair(5, h);
        ob.pair(330, hLayouts);
        ob.pair(100, "AcDbPlotSettings");
        ob.pair(1, "");
        ob.pair(4, "A3");
        ob.pair(6, "");
        for (int code : {40, 41, 42, 43}) {
            ob.real(code, code % 2 ? 20.0 : 7.5);
        }
        ob.real(44, 420.0);
        ob.real(45, 297.0);
        for (int code : {46, 47, 48, 49, 140, 141}) {
            ob.real(code, 0.0);
        }
        ob.real(142, 1.0);
        ob.real(143, 1.0);
        ob.pair(70, tab == 0 ? 1024 : 0);
        ob.pair(72, 1);
        ob.pair(73, 0);
        ob.pair(74, 5);
        ob.pair(7, "");
        ob.pair(75, 16);
        ob.pair(76, 0);
        ob.pair(77, 2);
        ob.pair(78, 300);
        ob.real(147, 1.0);
        ob.real(148, 0.0);
        ob.real(149, 0.0);
        ob.pair(100, "AcDbLayout");
        ob.pair(1, name);
        ob.pair(70, 1);
        ob.pair(71, tab);
        ob.real(10, 0.0);
        ob.real(20, 0.0);
        ob.real(11, 420.0);
        ob.real(21, 297.0);
        ob.point(12, {0, 0});
        ob.point(14, {x0, y0});
        ob.point(15, {x1, y1});
        ob.real(146, 0.0);
        ob.point(13, {0, 0});
        ob.point(16, {1, 0});
        ob.point(17, {0, 1});
        ob.pair(76, 1);
        ob.pair(330, block);
    }
    ob.pair(0, "ENDSEC");
    ob.pair(0, "EOF");

    DxfWriter seed;
    seed.pair(5, hex(next));
    return headerHead + seed.str() + headerTail + tables + "0\r\nSECTION\r\n2\r\nENTITIES\r\n" + entities
        + "0\r\nENDSEC\r\n" + ob.str();
}

}  // namespace core::exporter
