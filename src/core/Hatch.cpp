#include "Hatch.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <limits>
#include <numbers>

namespace core::hatch
{

namespace
{
constexpr double kPi = std::numbers::pi;

// A family of lines as laid out: line k through o + k (dx u + dy n), running along u.
struct Family
{
    Vec2 o, u, n;
    double dx = 0, dy = 0;
    std::vector<double> dashes;
    double period = 0;  // the dash pattern's length
};

std::vector<Family> families(const Hatch& h)
{
    std::vector<Family> out;
    for (const HatchLine& l : placed(h)) {
        if (std::abs(l.dy) < 1e-9) {
            continue;  // (all one line: nothing to repeat across)
        }
        Family f;
        const double a = l.angle * kPi / 180;
        f.u = {std::cos(a), std::sin(a)};
        f.n = {-f.u.y, f.u.x};
        f.o = {l.x, l.y};
        f.dx = l.dx;
        f.dy = l.dy;
        f.dashes = l.dashes;
        for (double d : l.dashes) {
            f.period += std::abs(d);
        }
        out.push_back(std::move(f));
    }
    return out;
}

double dot(Vec2 a, Vec2 b) { return a.x * b.x + a.y * b.y; }
Vec2 sub(Vec2 a, Vec2 b) { return {a.x - b.x, a.y - b.y}; }
Vec2 along(Vec2 p, Vec2 u, double t) { return {p.x + u.x * t, p.y + u.y * t}; }

Vec2 base(const Family& f, long long k)
{
    const double kk = static_cast<double>(k);
    return {f.o.x + f.u.x * kk * f.dx + f.n.x * kk * f.dy, f.o.y + f.u.y * kk * f.dx + f.n.y * kk * f.dy};
}

// The lines of a family crossing the band the points span: k0..k1.
bool lineRange(const Family& f, const std::vector<Vec2>& pts, long long& k0, long long& k1)
{
    double lo = std::numeric_limits<double>::infinity(), hi = -lo;
    for (const Vec2& p : pts) {
        const double v = dot(sub(p, f.o), f.n);
        lo = std::min(lo, v);
        hi = std::max(hi, v);
    }
    if (!(lo <= hi)) {
        return false;
    }
    double a = lo / f.dy, b = hi / f.dy;
    if (a > b) {
        std::swap(a, b);
    }
    k0 = static_cast<long long>(std::ceil(a - 1e-9));
    k1 = static_cast<long long>(std::floor(b + 1e-9));
    return k0 <= k1;
}

// The dashes of one line between t0 and t1 (from its base point, where its pattern starts).
// False if that's more than the limit allows.
template <typename Emit>
bool dashesOf(const Family& f, double t0, double t1, size_t limit, size_t& count, const Emit& emit)
{
    if (f.dashes.empty() || f.period < 1e-9) {
        emit(t0, t1);
        return ++count <= limit;
    }
    if ((t1 - t0) / f.period > static_cast<double>(limit)) {
        return false;
    }
    double t = std::floor(t0 / f.period) * f.period;
    while (t < t1) {
        for (double d : f.dashes) {
            if (d > 0) {
                const double a = std::max(t, t0), b = std::min(t + d, t1);
                if (a < b) {
                    emit(a, b);
                    if (++count > limit) {
                        return false;
                    }
                }
            }
            else if (d == 0 && t >= t0 && t <= t1) {
                emit(t, t);
                if (++count > limit) {
                    return false;
                }
            }
            t += std::abs(d);
            if (t >= t1) {
                break;
            }
        }
    }
    return true;
}

// AutoCAD's own patterns (as in its metric acadiso.pat), and a few plain ones.
std::vector<Pattern> makeBuiltins()
{
    const double r = 3.175 * std::numbers::sqrt2;  // (4.49013: the next line along, at 45 degrees)
    return {
        {"ANSI31", "Iron, brick, stone masonry", {{45, 0, 0, 0, 3.175, {}}}},
        {"ANSI32", "Steel", {{45, 0, 0, 0, 9.525, {}}, {45, r, 0, 0, 9.525, {}}}},
        {"ANSI33", "Bronze, brass, copper", {{45, 0, 0, 0, 6.35, {}}, {45, r, 0, 0, 6.35, {3.175, -1.5875}}}},
        {"ANSI34", "Plastic, rubber",
         {{45, 0, 0, 0, 19.05, {}}, {45, r, 0, 0, 19.05, {}}, {45, 2 * r, 0, 0, 19.05, {}}, {45, 3 * r, 0, 0, 19.05, {}}}},
        {"ANSI35", "Fire brick, refractory material",
         {{45, 0, 0, 0, 6.35, {}}, {45, r, 0, 0, 6.35, {7.9375, -1.5875, 0, -1.5875}}}},
        {"ANSI36", "Marble, slate, glass", {{45, 0, 0, 5.55625, 3.175, {7.9375, -1.5875, 0, -1.5875}}}},
        {"ANSI37", "Lead, zinc, magnesium, insulation", {{45, 0, 0, 0, 3.175, {}}, {135, 0, 0, 0, 3.175, {}}}},
        {"ANSI38", "Aluminium", {{45, 0, 0, 0, 3.175, {}}, {135, 0, 0, 6.35, 3.175, {7.9375, -4.7625}}}},
        {"LINE", "Parallel lines", {{0, 0, 0, 0, 3.175, {}}}},
        {"NET", "Square grid", {{0, 0, 0, 0, 3.175, {}}, {90, 0, 0, 0, 3.175, {}}}},
        {"NET3", "Triangular grid", {{0, 0, 0, 0, 3.175, {}}, {60, 0, 0, 0, 3.175, {}}, {120, 0, 0, 0, 3.175, {}}}},
        {"DASH", "Dashed lines", {{0, 0, 0, 3.175, 3.175, {3.175, -3.175}}}},
        {"DOTS", "Dots", {{0, 0, 0, 0.79375, 1.5875, {0, -1.5875}}}},
        {"BRICK", "Brick", {{0, 0, 0, 0, 6.35, {}}, {90, 0, 0, 0, 12.7, {6.35, -6.35}}, {90, 6.35, 0, 0, 12.7, {-6.35, 6.35}}}},
        {"SQUARE", "Small squares",
         {{0, 0, 0, 0, 3.175, {3.175, -3.175}}, {90, 0, 0, 0, 3.175, {3.175, -3.175}}}},
        {"CROSS", "Crosses",
         {{0, 0, 0, 6.35, 6.35, {3.175, -9.525}}, {90, 1.5875, -1.5875, 6.35, 6.35, {3.175, -9.525}}}},
        {"ZIGZAG", "Stairs", {{0, 0, 0, 3.175, 3.175, {3.175, -3.175}}, {90, 3.175, 0, 3.175, 3.175, {3.175, -3.175}}}},
        {"EARTH", "Earth",
         {{0, 0, 0, 6.35, 6.35, {6.35, -6.35}},
          {0, 0, 2.38125, 6.35, 6.35, {6.35, -6.35}},
          {0, 0, 4.7625, 6.35, 6.35, {6.35, -6.35}},
          {90, 0.79375, 5.55625, 6.35, 6.35, {6.35, -6.35}},
          {90, 3.175, 5.55625, 6.35, 6.35, {6.35, -6.35}},
          {90, 5.55625, 5.55625, 6.35, 6.35, {6.35, -6.35}}}},
    };
}
}  // namespace

const std::vector<Pattern>& builtins()
{
    static const std::vector<Pattern> patterns = makeBuiltins();
    return patterns;
}

std::optional<Hatch> builtin(const std::string& name, double angle, double scale, double width)
{
    auto upper = [](std::string s) {
        std::ranges::transform(s, s.begin(), [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
        return s;
    };
    for (const Pattern& p : builtins()) {
        if (p.name == upper(name)) {
            return Hatch{p.name, p.lines, angle, scale, width};
        }
    }
    return std::nullopt;
}

std::vector<HatchLine> placed(const Hatch& h)
{
    const double a = h.angle * kPi / 180, c = std::cos(a), s = std::sin(a), k = h.scale;
    std::vector<HatchLine> out;
    for (const HatchLine& l : h.lines) {
        HatchLine p = l;
        p.angle = l.angle + h.angle;
        p.x = (c * l.x - s * l.y) * k;
        p.y = (s * l.x + c * l.y) * k;
        p.dx = l.dx * k;  // (along and across the line: they turn with it)
        p.dy = l.dy * k;
        for (double& d : p.dashes) {
            d *= k;
        }
        out.push_back(std::move(p));
    }
    return out;
}

double spacing(const Hatch& h)
{
    double out = std::numeric_limits<double>::infinity();
    for (const HatchLine& l : h.lines) {
        if (std::abs(l.dy) >= 1e-9) {
            out = std::min(out, std::abs(l.dy * h.scale));
        }
    }
    return out;
}

bool across(const Hatch& h, double x0, double y0, double x1, double y1, size_t limit, std::vector<Segment>& out)
{
    out.clear();
    const std::vector<Vec2> box{{x0, y0}, {x1, y0}, {x1, y1}, {x0, y1}};
    size_t count = 0;
    for (const Family& f : families(h)) {
        long long k0 = 0, k1 = 0;
        if (!lineRange(f, box, k0, k1)) {
            continue;
        }
        if (static_cast<double>(k1 - k0) > static_cast<double>(limit)) {
            out.clear();
            return false;
        }
        for (long long k = k0; k <= k1; ++k) {
            const Vec2 p = base(f, k);
            double t0 = std::numeric_limits<double>::infinity(), t1 = -t0;
            for (const Vec2& c : box) {
                const double t = dot(sub(c, p), f.u);
                t0 = std::min(t0, t);
                t1 = std::max(t1, t);
            }
            const bool ok = dashesOf(f, t0, t1, limit, count, [&](double a, double b) {
                out.push_back({along(p, f.u, a), along(p, f.u, b)});
            });
            if (!ok) {
                out.clear();
                return false;
            }
        }
    }
    return true;
}

bool within(const Hatch& h, const regions::Region& area, size_t limit, std::vector<Segment>& out)
{
    out.clear();
    std::vector<const std::vector<Vec2>*> rings{&area.outer};
    for (const auto& hole : area.holes) {
        rings.push_back(&hole);
    }
    size_t count = 0;
    std::vector<double> cuts;
    for (const Family& f : families(h)) {
        long long k0 = 0, k1 = 0;
        if (!lineRange(f, area.outer, k0, k1)) {
            continue;
        }
        if (static_cast<double>(k1 - k0) > static_cast<double>(limit)) {
            out.clear();
            return false;
        }
        for (long long k = k0; k <= k1; ++k) {
            const Vec2 p = base(f, k);
            // Where it crosses the outline (and the holes'): inside between each pair, in order.
            cuts.clear();
            for (const auto* ring : rings) {
                const size_t n = ring->size();
                for (size_t i = 0; i < n; ++i) {
                    const Vec2 a = (*ring)[i], b = (*ring)[(i + 1) % n];
                    const double sa = dot(sub(a, p), f.n), sb = dot(sub(b, p), f.n);
                    if ((sa > 0) != (sb > 0)) {
                        const double ta = dot(sub(a, p), f.u), tb = dot(sub(b, p), f.u);
                        cuts.push_back(ta + (tb - ta) * sa / (sa - sb));
                    }
                }
            }
            std::ranges::sort(cuts);
            for (size_t i = 0; i + 1 < cuts.size(); i += 2) {
                const bool ok = dashesOf(f, cuts[i], cuts[i + 1], limit, count, [&](double a, double b) {
                    out.push_back({along(p, f.u, a), along(p, f.u, b)});
                });
                if (!ok) {
                    out.clear();
                    return false;
                }
            }
        }
    }
    return true;
}

}  // namespace core::hatch
