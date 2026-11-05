#include "Curves.h"

#include <algorithm>
#include <cmath>
#include <numbers>

namespace core::curve
{

namespace
{
constexpr double kTwoPi = 2.0 * std::numbers::pi;

Vec2 add(Vec2 a, Vec2 b) { return {a.x + b.x, a.y + b.y}; }
Vec2 sub(Vec2 a, Vec2 b) { return {a.x - b.x, a.y - b.y}; }
Vec2 mul(Vec2 a, double k) { return {a.x * k, a.y * k}; }
double dot(Vec2 a, Vec2 b) { return a.x * b.x + a.y * b.y; }
double norm(Vec2 a) { return std::hypot(a.x, a.y); }
Vec2 rot90(Vec2 a) { return {-a.y, a.x}; }
Vec2 lerp(Vec2 a, Vec2 b, double t) { return add(a, mul(sub(b, a), t)); }

double wrap(double a)  // into [0, 2pi)
{
    a = std::fmod(a, kTwoPi);
    return a < 0 ? a + kTwoPi : a;
}

Vec2 pos(const Sketch& s, Id p)
{
    return {s.point(p).x, s.point(p).y};
}

Bezier straight(Vec2 a, Vec2 b)
{
    return {{a, lerp(a, b, 1.0 / 3), lerp(a, b, 2.0 / 3), b}};
}

// One piece of a centripetal Catmull-Rom curve, from p1 to p2 (p0 and p3 their neighbours), as
// a cubic Bezier.
Bezier catmullRom(Vec2 p0, Vec2 p1, Vec2 p2, Vec2 p3)
{
    auto d = [](Vec2 a, Vec2 b) { return std::max(std::sqrt(norm(sub(b, a))), 1e-9); };
    const double d1 = d(p0, p1), d2 = d(p1, p2), d3 = d(p2, p3);
    const Vec2 b1 = mul(add(sub(mul(p2, d1 * d1), mul(p0, d2 * d2)), mul(p1, 2 * d1 * d1 + 3 * d1 * d2 + d2 * d2)),
                        1.0 / (3 * d1 * (d1 + d2)));
    const Vec2 b2 = mul(add(sub(mul(p1, d3 * d3), mul(p3, d2 * d2)), mul(p2, 2 * d3 * d3 + 3 * d3 * d2 + d2 * d2)),
                        1.0 / (3 * d3 * (d3 + d2)));
    return {{p1, b1, b2, p2}};
}

// One knot inserted into a B-spline of degree p (knots U, control points P).
void insertKnot(std::vector<double>& U, std::vector<Vec2>& P, int p, double u)
{
    const int k = static_cast<int>(std::ranges::upper_bound(U, u) - U.begin()) - 1;
    std::vector<Vec2> Q(P.size() + 1);
    for (int i = 0; i < static_cast<int>(Q.size()); ++i) {
        const size_t si = static_cast<size_t>(i);
        if (i <= k - p) {
            Q[si] = P[si];
        }
        else if (i >= k + 1) {
            Q[si] = P[si - 1];
        }
        else {
            const double span = U[static_cast<size_t>(i + p)] - U[si];
            const double a = span > 0 ? (u - U[si]) / span : 0.0;
            Q[si] = add(mul(P[si - 1], 1 - a), mul(P[si], a));
        }
    }
    U.insert(U.begin() + k + 1, u);
    P = std::move(Q);
}

// Do segments a-b and c-d cross (ends included, with a little slack)? Their parameters if so.
bool segmentsCross(Vec2 a, Vec2 b, Vec2 c, Vec2 d, double& s, double& t)
{
    const Vec2 r = sub(b, a), q = sub(d, c);
    const double denom = r.x * q.y - r.y * q.x;
    if (std::abs(denom) < 1e-300) {
        return false;
    }
    const Vec2 w = sub(c, a);
    s = (w.x * q.y - w.y * q.x) / denom;
    t = (w.x * r.y - w.y * r.x) / denom;
    constexpr double kSlack = 1e-6;
    return s >= -kSlack && s <= 1 + kSlack && t >= -kSlack && t <= 1 + kSlack;
}
}  // namespace

// ---------------------------------------------------------------------------------------------

Vec2 Bezier::at(double t) const
{
    const double u = 1 - t;
    const double b0 = u * u * u, b1 = 3 * u * u * t, b2 = 3 * u * t * t, b3 = t * t * t;
    return {b0 * p[0].x + b1 * p[1].x + b2 * p[2].x + b3 * p[3].x, b0 * p[0].y + b1 * p[1].y + b2 * p[2].y + b3 * p[3].y};
}

Vec2 Bezier::derivative(double t) const
{
    const double u = 1 - t;
    const Vec2 d0 = sub(p[1], p[0]), d1 = sub(p[2], p[1]), d2 = sub(p[3], p[2]);
    return mul(add(add(mul(d0, u * u), mul(d1, 2 * u * t)), mul(d2, t * t)), 3.0);
}

Bezier Bezier::part(double from, double to) const
{
    // de Casteljau: the curve from 0 to `to`, then the end part of that from from/to.
    auto split = [](const Bezier& b, double t, bool keepFirst) {
        const Vec2 p01 = lerp(b.p[0], b.p[1], t), p12 = lerp(b.p[1], b.p[2], t), p23 = lerp(b.p[2], b.p[3], t);
        const Vec2 q0 = lerp(p01, p12, t), q1 = lerp(p12, p23, t);
        const Vec2 m = lerp(q0, q1, t);
        return keepFirst ? Bezier{{b.p[0], p01, q0, m}} : Bezier{{m, q1, p23, b.p[3]}};
    };
    Bezier head = to < 1.0 ? split(*this, to, true) : *this;
    return from > 0.0 ? split(head, to > 0 ? from / to : 0.0, false) : head;
}

std::vector<Bezier> splineBeziers(const std::vector<Vec2>& pts, SplineKind kind, bool closed)
{
    const size_t n = pts.size();
    std::vector<Bezier> out;
    if (n < 2) {
        return out;
    }
    auto P = [&](long i) {
        const long m = static_cast<long>(n);
        return pts[static_cast<size_t>(((i % m) + m) % m)];
    };
    if (kind == SplineKind::Fit) {
        if (n == 2 && !closed) {
            out.push_back(straight(pts[0], pts[1]));
            return out;
        }
        const long pieces = closed ? static_cast<long>(n) : static_cast<long>(n) - 1;
        for (long i = 0; i < pieces; ++i) {
            const Vec2 p1 = P(i), p2 = P(i + 1);
            // Open ends: a point mirrored beyond the end stands in for the missing neighbour.
            const Vec2 p0 = !closed && i == 0 ? sub(mul(p1, 2), p2) : P(i - 1);
            const Vec2 p3 = !closed && i == pieces - 1 ? sub(mul(p2, 2), p1) : P(i + 2);
            out.push_back(catmullRom(p0, p1, p2, p3));
        }
        return out;
    }
    // Control points.
    if (closed) {
        for (long i = 0; i < static_cast<long>(n); ++i) {
            const Vec2 q0 = P(i - 1), q1 = P(i), q2 = P(i + 1), q3 = P(i + 2);
            out.push_back({{mul(add(add(q0, mul(q1, 4)), q2), 1.0 / 6), mul(add(mul(q1, 2), q2), 1.0 / 3),
                            mul(add(q1, mul(q2, 2)), 1.0 / 3), mul(add(add(q1, mul(q2, 4)), q3), 1.0 / 6)}});
        }
        return out;
    }
    if (n == 2) {
        out.push_back(straight(pts[0], pts[1]));
        return out;
    }
    if (n == 3) {  // a quadratic, raised to a cubic
        out.push_back({{pts[0], lerp(pts[0], pts[1], 2.0 / 3), lerp(pts[2], pts[1], 2.0 / 3), pts[2]}});
        return out;
    }
    // Clamped uniform cubic: knots 0,0,0,0,1,...,m,m,m,m. Each inner knot raised to multiplicity
    // three leaves the Bezier pieces' control points in a row.
    const int m = static_cast<int>(n) - 3;
    std::vector<double> U(4, 0.0);
    for (int k = 1; k < m; ++k) {
        U.push_back(k);
    }
    U.insert(U.end(), 4, static_cast<double>(m));
    std::vector<Vec2> Q = pts;
    for (int k = 1; k < m; ++k) {
        insertKnot(U, Q, 3, k);
        insertKnot(U, Q, 3, k);
    }
    for (int j = 0; j < m; ++j) {
        const size_t i = static_cast<size_t>(3 * j);
        out.push_back({{Q[i], Q[i + 1], Q[i + 2], Q[i + 3]}});
    }
    return out;
}

std::vector<Vec2> splinePoints(const Sketch& s, const Spline& spline)
{
    std::vector<Vec2> out;
    for (Id p : spline.points) {
        out.push_back(pos(s, p));
    }
    return out;
}

// ---------------------------------------------------------------------------------------------

Vec2 EllipseFrame::at(double t) const
{
    return add(c, add(mul(u, a * std::cos(t)), mul(rot90(u), b * std::sin(t))));
}

Vec2 EllipseFrame::derivative(double t) const
{
    return add(mul(u, -a * std::sin(t)), mul(rot90(u), b * std::cos(t)));
}

double EllipseFrame::param(Vec2 p) const
{
    const Vec2 d = sub(p, c);
    const double x = dot(d, u) / std::max(a, 1e-300), y = dot(d, rot90(u)) / std::max(b, 1e-300);
    return wrap(std::atan2(y, x));
}

EllipseFrame ellipseFrame(const Sketch& s, Id id)
{
    const std::array<Id, 3> axes = s.ellipseAxes(id);
    EllipseFrame f;
    f.c = pos(s, axes[0]);
    const Vec2 major = sub(pos(s, axes[1]), f.c);
    f.a = norm(major);
    f.u = f.a > 0 ? mul(major, 1.0 / f.a) : Vec2{1, 0};
    f.b = norm(sub(pos(s, axes[2]), f.c));
    return f;
}

// ---------------------------------------------------------------------------------------------

Path Path::line(Vec2 a, Vec2 b)
{
    Path p;
    p.m_kind = Kind::Line;
    p.a = a;
    p.b = b;
    return p;
}

Path Path::circle(Vec2 c, double r)
{
    Path p;
    p.m_kind = Kind::Circle;
    p.c = c;
    p.r = r;
    p.sweep = kTwoPi;
    return p;
}

Path Path::arc(Vec2 c, double r, double start, double sweep)
{
    Path p = circle(c, r);
    p.m_kind = Kind::Arc;
    p.start = start;
    p.sweep = sweep;
    return p;
}

Path Path::ellipse(const EllipseFrame& e)
{
    Path p;
    p.m_kind = Kind::Ellipse;
    p.ellipseShape = e;
    p.sweep = kTwoPi;
    return p;
}

Path Path::ellipseArc(const EllipseFrame& e, double start, double sweep)
{
    Path p = ellipse(e);
    p.m_kind = Kind::EllipseArc;
    p.start = start;
    p.sweep = sweep;
    return p;
}

Path Path::spline(std::vector<Bezier> pieces, bool closed, double from, double span)
{
    Path p;
    p.m_kind = Kind::Spline;
    p.pieces = std::move(pieces);
    p.closedSpline = closed;
    p.start = from;
    p.sweep = span;
    return p;
}

double Path::end() const
{
    return m_kind == Kind::Line ? 1.0 : sweep;
}

bool Path::closed() const
{
    return m_kind == Kind::Circle || m_kind == Kind::Ellipse
        || (m_kind == Kind::Spline && closedSpline && sweep >= static_cast<double>(pieces.size()) - 1e-12);
}

Vec2 Path::splineAt(double u, bool deriv) const
{
    const double n = static_cast<double>(pieces.size());
    if (pieces.empty()) {
        return {};
    }
    if (closedSpline) {
        u = std::fmod(u, n);
        if (u < 0) {
            u += n;
        }
    }
    // Past an open spline's ends, its end pieces carry on (smoothly: the solver's points kept on
    // it may be pulled a little past the end on their way, and a clamped curve would stop them).
    const size_t i = static_cast<size_t>(std::clamp(std::floor(u), 0.0, n - 1));
    const double local = u - static_cast<double>(i);
    return deriv ? pieces[i].derivative(local) : pieces[i].at(local);
}

Vec2 Path::at(double t) const
{
    switch (m_kind) {
        case Kind::Line: return lerp(a, b, t);
        case Kind::Circle:
        case Kind::Arc: return {c.x + r * std::cos(start + t), c.y + r * std::sin(start + t)};
        case Kind::Ellipse:
        case Kind::EllipseArc: return ellipseShape.at(start + t);
        case Kind::Spline: return splineAt(start + t, false);
    }
    return {};
}

Vec2 Path::derivative(double t) const
{
    switch (m_kind) {
        case Kind::Line: return sub(b, a);
        case Kind::Circle:
        case Kind::Arc: return {-r * std::sin(start + t), r * std::cos(start + t)};
        case Kind::Ellipse:
        case Kind::EllipseArc: return ellipseShape.derivative(start + t);
        case Kind::Spline: return splineAt(start + t, true);
    }
    return {};
}

double Path::param(Vec2 p) const
{
    switch (m_kind) {
        case Kind::Line: {
            const Vec2 d = sub(b, a);
            const double l2 = dot(d, d);
            return l2 > 0 ? std::clamp(dot(sub(p, a), d) / l2, 0.0, 1.0) : 0.0;
        }
        case Kind::Circle: return wrap(std::atan2(p.y - c.y, p.x - c.x) - start);
        case Kind::Arc: {
            const double t = wrap(std::atan2(p.y - c.y, p.x - c.x) - start);
            if (t <= sweep) {
                return t;
            }
            return norm(sub(at(0), p)) <= norm(sub(at(sweep), p)) ? 0.0 : sweep;
        }
        default: break;
    }
    // Numerically: the nearest of many samples, then narrowed down (golden section) around it.
    const int n = m_kind == Kind::Spline ? std::max(48, static_cast<int>(std::ceil(sweep * 32))) : 128;
    const double h = end() / n;
    int best = 0;
    double bestD = 1e300;
    for (int i = 0; i <= n; ++i) {
        const double d = norm(sub(at(h * i), p));
        if (d < bestD) {
            bestD = d;
            best = i;
        }
    }
    double lo = h * (best - 1), hi = h * (best + 1);
    if (!closed()) {
        lo = std::max(lo, 0.0);
        hi = std::min(hi, end());
    }
    const double g = (std::sqrt(5.0) - 1) / 2;
    double x1 = hi - g * (hi - lo), x2 = lo + g * (hi - lo);
    double f1 = norm(sub(at(x1), p)), f2 = norm(sub(at(x2), p));
    for (int i = 0; i < 80 && hi - lo > 1e-15 * std::max(1.0, end()); ++i) {
        if (f1 < f2) {
            hi = x2;
            x2 = x1;
            f2 = f1;
            x1 = hi - g * (hi - lo);
            f1 = norm(sub(at(x1), p));
        }
        else {
            lo = x1;
            x1 = x2;
            f1 = f2;
            x2 = lo + g * (hi - lo);
            f2 = norm(sub(at(x2), p));
        }
    }
    double t = (lo + hi) / 2;
    if (closed()) {
        t = std::fmod(t, end());
        if (t < 0) {
            t += end();
        }
    }
    return t;
}

double Path::distance(Vec2 p) const
{
    return norm(sub(closest(p), p));
}

std::vector<Vec2> Path::sample(double from, double to, int n) const
{
    std::vector<Vec2> out;
    for (int i = 0; i <= n; ++i) {
        out.push_back(at(from + (to - from) * i / n));
    }
    return out;
}

std::vector<Vec2> Path::polyline(int perUnit) const
{
    switch (m_kind) {
        case Kind::Line: return {a, b};
        case Kind::Spline: return sample(0, sweep, std::max(2, static_cast<int>(std::ceil(sweep * perUnit))));
        default: return sample(0, sweep, std::max(4, static_cast<int>(std::ceil(sweep / (kTwoPi / 4) * perUnit))));
    }
}

double Path::length() const
{
    const std::vector<Vec2> pts = polyline(64);
    double total = 0;
    for (size_t i = 1; i < pts.size(); ++i) {
        total += norm(sub(pts[i], pts[i - 1]));
    }
    return total;
}

std::vector<Bezier> Path::beziers() const
{
    std::vector<Bezier> out;
    if (m_kind != Kind::Spline || pieces.empty()) {
        return out;
    }
    const double n = static_cast<double>(pieces.size());
    double u = start;
    const double stop = start + sweep;
    while (u < stop - 1e-12) {
        double wrapped = closedSpline ? std::fmod(u, n) : std::clamp(u, 0.0, n);
        if (wrapped < 0) {
            wrapped += n;
        }
        const size_t i = std::min(static_cast<size_t>(wrapped), pieces.size() - 1);
        const double local = wrapped - static_cast<double>(i);
        const double take = std::min(1.0 - local, stop - u);
        out.push_back(pieces[i].part(local, local + take));
        u += take;
        if (take <= 1e-12) {
            break;
        }
    }
    return out;
}

Path pathOf(const Sketch& s, Id id)
{
    const Entity& e = s.entity(id);
    switch (e.kind()) {
        case EntityKind::Line: return Path::line(pos(s, s.line(id).p1), pos(s, s.line(id).p2));
        case EntityKind::Circle: return Path::circle(pos(s, s.circle(id).center), s.circle(id).radius);
        case EntityKind::Arc: {
            const ArcShape a = arcShape(s, id);
            return Path::arc(a.center, a.radius, a.startAngle, a.sweep);
        }
        case EntityKind::Ellipse: return Path::ellipse(ellipseFrame(s, id));
        case EntityKind::EllipseArc: {
            const EllipseFrame f = ellipseFrame(s, id);
            const EllipseArc& ea = s.ellipseArc(id);
            const double t0 = f.param(pos(s, ea.start));
            double sweep = wrap(f.param(pos(s, ea.end)) - t0);
            if (sweep == 0.0) {
                sweep = kTwoPi;
            }
            return Path::ellipseArc(f, t0, sweep);
        }
        case EntityKind::Spline: {
            const Spline& sp = s.spline(id);
            std::vector<Bezier> pieces = splineBeziers(splinePoints(s, sp), sp.kind, sp.closed);
            const double n = static_cast<double>(pieces.size());
            if (!sp.isPiece()) {
                return Path::spline(std::move(pieces), sp.closed, 0.0, n);
            }
            const Path whole = Path::spline(pieces, sp.closed, 0.0, n);
            double u0 = whole.param(pos(s, sp.start)), u1 = whole.param(pos(s, sp.end));
            if (sp.closed) {
                double span = std::fmod(u1 - u0, n);
                if (span <= 0) {
                    span += n;
                }
                return Path::spline(std::move(pieces), true, u0, span);
            }
            if (u1 < u0) {
                std::swap(u0, u1);
            }
            return Path::spline(std::move(pieces), false, u0, u1 - u0);
        }
        case EntityKind::Point: break;
    }
    throw SketchError("not a curve");
}

std::vector<Vec2> intersections(const Path& p1, const Path& p2)
{
    auto segments = [](const Path& p) {
        switch (p.kind()) {
            case Path::Kind::Line: return 1;
            case Path::Kind::Spline: return std::max(8, static_cast<int>(std::ceil(p.end() * 48)));
            default: return std::max(16, static_cast<int>(std::ceil(p.end() / kTwoPi * 192)));
        }
    };
    const int n1 = segments(p1), n2 = segments(p2);
    const std::vector<Vec2> s1 = p1.sample(0, p1.end(), n1), s2 = p2.sample(0, p2.end(), n2);
    double scale = 1e-9;
    for (const auto* pts : {&s1, &s2}) {
        for (const Vec2& q : *pts) {
            scale = std::max({scale, std::abs(q.x), std::abs(q.y)});
        }
    }
    const double h1 = p1.end() / n1, h2 = p2.end() / n2;
    std::vector<Vec2> found;
    for (int i = 0; i < n1; ++i) {
        for (int j = 0; j < n2; ++j) {
            double a = 0, b = 0;
            if (!segmentsCross(s1[static_cast<size_t>(i)], s1[static_cast<size_t>(i + 1)], s2[static_cast<size_t>(j)],
                               s2[static_cast<size_t>(j + 1)], a, b)) {
                continue;
            }
            // Refined on the curves themselves: Newton on p1(s) - p2(t) = 0.
            double s = (i + std::clamp(a, 0.0, 1.0)) * h1, t = (j + std::clamp(b, 0.0, 1.0)) * h2;
            bool ok = false;
            for (int k = 0; k < 40; ++k) {
                const Vec2 f = sub(p1.at(s), p2.at(t));
                if (norm(f) <= 1e-12 * scale) {
                    ok = true;
                    break;
                }
                const Vec2 d1 = p1.derivative(s), d2 = p2.derivative(t);
                const double det = -d1.x * d2.y + d2.x * d1.y;  // of [d1, -d2]
                if (std::abs(det) < 1e-300) {
                    break;
                }
                const double ds = (-f.x * -d2.y - -d2.x * -f.y) / det;
                const double dt = (d1.x * -f.y - -f.x * d1.y) / det;
                s += ds;
                t += dt;
                if (!p1.closed()) {
                    s = std::clamp(s, 0.0, p1.end());
                }
                if (!p2.closed()) {
                    t = std::clamp(t, 0.0, p2.end());
                }
            }
            const Vec2 q = p1.at(s);
            ok = ok || norm(sub(q, p2.at(t))) <= 1e-8 * scale;
            if (!ok) {
                continue;
            }
            if (std::ranges::none_of(found, [&](Vec2 o) { return norm(sub(o, q)) <= 1e-7 * scale; })) {
                found.push_back(q);
            }
        }
    }
    return found;
}

}  // namespace core::curve
