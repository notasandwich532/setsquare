#include "Builders.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <numbers>

namespace core::build
{

namespace
{
constexpr double kPi = std::numbers::pi;
constexpr double kEps = 1e-9;
using CT = ConstraintType;

Vec2 pos(const Sketch& s, Id p)
{
    const Point& q = s.point(p);
    return {q.x, q.y};
}
Vec2 add(Vec2 a, Vec2 b) { return {a.x + b.x, a.y + b.y}; }
Vec2 sub(Vec2 a, Vec2 b) { return {a.x - b.x, a.y - b.y}; }
Vec2 mul(Vec2 a, double k) { return {a.x * k, a.y * k}; }
double dot(Vec2 a, Vec2 b) { return a.x * b.x + a.y * b.y; }
double cross(Vec2 a, Vec2 b) { return a.x * b.y - a.y * b.x; }
double norm(Vec2 a) { return std::hypot(a.x, a.y); }
Vec2 rot90(Vec2 a) { return {-a.y, a.x}; }  // counter-clockwise
Vec2 polar(Vec2 c, double r, double angle) { return {c.x + r * std::cos(angle), c.y + r * std::sin(angle)}; }

Vec2 unit(Vec2 a, const char* what)
{
    const double n = norm(a);
    if (n < kEps) {
        throw SketchError(std::string(what) + ": points are too close together");
    }
    return {a.x / n, a.y / n};
}

Id point(Sketch& s, Vec2 p)
{
    return s.addPoint(p.x, p.y);
}

void requirePositive(double v, const char* what)
{
    if (!(v > kEps)) {
        throw SketchError(std::string(what) + " must be positive");
    }
}

EntityKind kindOf(const Sketch& s, Id id)
{
    return s.entity(id).kind();
}

// Line endpoints as positions.
void lineEnds(const Sketch& s, Id line, Vec2& a, Vec2& b)
{
    const Line& l = s.line(line);
    a = pos(s, l.p1);
    b = pos(s, l.p2);
}

bool lineIntersection(Vec2 a1, Vec2 d1, Vec2 a2, Vec2 d2, Vec2& out)
{
    const double c = cross(d1, d2);
    if (std::abs(c) < kEps * norm(d1) * norm(d2)) {
        return false;
    }
    const double t = cross(sub(a2, a1), d2) / c;
    out = add(a1, mul(d1, t));
    return true;
}

double distanceToCurve(const Sketch& s, Id curve, Vec2 p)
{
    return distanceTo(s, curve, p);
}
}  // namespace

// =============================================================================================
// Rectangles
// =============================================================================================

namespace
{
Rectangle closeRectangle(Sketch& s, std::array<Id, 4> corners)
{
    Rectangle r;
    r.corners = corners;
    for (int i = 0; i < 4; ++i) {
        r.sides[i] = s.addLine(corners[i], corners[(i + 1) % 4]);
    }
    return r;
}
}  // namespace

Rectangle rectangle(Sketch& s, Id corner, Id opposite)
{
    const Vec2 a = pos(s, corner), c = pos(s, opposite);
    if (std::abs(c.x - a.x) < kEps || std::abs(c.y - a.y) < kEps) {
        throw SketchError("rectangle: corners must differ in both x and y");
    }
    Rectangle r = closeRectangle(s, {corner, point(s, {c.x, a.y}), opposite, point(s, {a.x, c.y})});
    s.addConstraint(CT::Horizontal, {r.sides[0]});
    s.addConstraint(CT::Vertical, {r.sides[1]});
    s.addConstraint(CT::Horizontal, {r.sides[2]});
    s.addConstraint(CT::Vertical, {r.sides[3]});
    return r;
}

Rectangle rectangle3(Sketch& s, Id a, Id b, Vec2 widthPoint)
{
    const Vec2 pa = pos(s, a), pb = pos(s, b);
    const Vec2 n = rot90(unit(sub(pb, pa), "3-point rectangle"));
    const double w = dot(sub(widthPoint, pa), n);
    if (std::abs(w) < kEps) {
        throw SketchError("3-point rectangle: the third point must be off the first side");
    }
    Rectangle r = closeRectangle(s, {a, b, point(s, add(pb, mul(n, w))), point(s, add(pa, mul(n, w)))});
    s.addConstraint(CT::Perpendicular, {r.sides[0], r.sides[1]});
    s.addConstraint(CT::Parallel, {r.sides[0], r.sides[2]});
    s.addConstraint(CT::Parallel, {r.sides[1], r.sides[3]});
    return r;
}

Rectangle rectangleCenter(Sketch& s, Id center, Vec2 corner)
{
    const Vec2 c = pos(s, center);
    const double hx = corner.x - c.x, hy = corner.y - c.y;
    if (std::abs(hx) < kEps || std::abs(hy) < kEps) {
        throw SketchError("centre rectangle: the corner must differ from the centre in both x and y");
    }
    Rectangle r = closeRectangle(s, {point(s, {c.x - hx, c.y - hy}), point(s, {c.x + hx, c.y - hy}),
                                     point(s, {c.x + hx, c.y + hy}), point(s, {c.x - hx, c.y + hy})});
    s.addConstraint(CT::Horizontal, {r.sides[0]});
    s.addConstraint(CT::Vertical, {r.sides[1]});
    s.addConstraint(CT::Horizontal, {r.sides[2]});
    s.addConstraint(CT::Vertical, {r.sides[3]});
    r.center = center;
    r.diagonals = {s.addLine(r.corners[0], r.corners[2]), s.addLine(r.corners[1], r.corners[3])};
    for (Id d : r.diagonals) {
        s.entity(d).construction = true;
    }
    // One diagonal pins the centre; the other passes through it anyway.
    s.addConstraint(CT::Midpoint, {center, r.diagonals[0]});
    return r;
}

// =============================================================================================
// Circles and arcs
// =============================================================================================

Id circle2(Sketch& s, Vec2 a, Vec2 b)
{
    const double r = norm(sub(b, a)) / 2;
    requirePositive(r, "2-point circle: diameter");
    return s.addCircle(point(s, mul(add(a, b), 0.5)), r);
}

Id circle3(Sketch& s, Vec2 a, Vec2 b, Vec2 c)
{
    Vec2 centre;
    double r = 0;
    if (!circumcircle(a, b, c, centre, r)) {
        throw SketchError("3-point circle: the points are in a line");
    }
    return s.addCircle(point(s, centre), r);
}

Id circleTangent(Sketch& s, const std::vector<Id>& curves, Vec2 near, double radius)
{
    if (curves.size() != 2 && curves.size() != 3) {
        throw SketchError("tangent circle: pick 2 or 3 curves");
    }
    for (Id c : curves) {
        if (kindOf(s, c) == EntityKind::Point) {
            throw SketchError("tangent circle: pick lines, circles or arcs");
        }
    }
    const bool allLines = std::ranges::all_of(curves, [&](Id c) { return kindOf(s, c) == EntityKind::Line; });

    // A good starting position matters: the solver keeps each tangency on the side it starts on.
    Vec2 centre = near;
    double r = radius;
    if (curves.size() == 2 && allLines) {
        Vec2 a1, b1, a2, b2;
        lineEnds(s, curves[0], a1, b1);
        lineEnds(s, curves[1], a2, b2);
        const Vec2 d1 = unit(sub(b1, a1), "tangent circle"), d2 = unit(sub(b2, a2), "tangent circle");
        Vec2 x;
        if (lineIntersection(a1, d1, a2, d2, x)) {
            requirePositive(radius, "tangent circle: radius");
            // The bisector direction pointing into the mouse's quadrant.
            const Vec2 toMouse = sub(near, x);
            Vec2 best{};
            double bestDot = -std::numeric_limits<double>::infinity();
            for (Vec2 b : {add(d1, d2), sub(d1, d2), mul(add(d1, d2), -1), mul(sub(d1, d2), -1)}) {
                if (norm(b) < kEps) {
                    continue;
                }
                const Vec2 u = unit(b, "tangent circle");
                if (dot(u, toMouse) > bestDot) {
                    bestDot = dot(u, toMouse);
                    best = u;
                }
            }
            const double sinHalf = std::abs(cross(best, d1));
            centre = add(x, mul(best, radius / sinHalf));
        }
        else {
            // Parallel: centred between them, nearest the mouse.
            const Vec2 n = rot90(d1);
            const double gap = dot(sub(a2, a1), n);
            const Vec2 onMid = add(a1, mul(n, gap / 2));
            centre = add(onMid, mul(d1, dot(sub(near, onMid), d1)));
            r = std::abs(gap) / 2;
        }
    }
    else if (curves.size() == 3 && allLines) {
        Vec2 a[3], b[3], d[3];
        for (int i = 0; i < 3; ++i) {
            lineEnds(s, curves[i], a[i], b[i]);
            d[i] = unit(sub(b[i], a[i]), "tangent circle");
        }
        Vec2 A, B, C;  // triangle vertices: A opposite line 0, etc.
        if (lineIntersection(a[1], d[1], a[2], d[2], A) && lineIntersection(a[0], d[0], a[2], d[2], B)
            && lineIntersection(a[0], d[0], a[1], d[1], C)) {
            const double la = norm(sub(B, C)), lb = norm(sub(A, C)), lc = norm(sub(A, B));
            const double area = std::abs(cross(sub(B, A), sub(C, A))) / 2;
            const double sp = (la + lb + lc) / 2;
            // Incircle and the three excircles; take the one centred nearest the mouse.
            struct Cand
            {
                double wa, wb, wc, rad;
            };
            const Cand cands[4] = {{la, lb, lc, area / sp}, {-la, lb, lc, area / (sp - la)},
                                   {la, -lb, lc, area / (sp - lb)}, {la, lb, -lc, area / (sp - lc)}};
            double bestDist = std::numeric_limits<double>::infinity();
            for (const Cand& k : cands) {
                const double w = k.wa + k.wb + k.wc;
                if (std::abs(w) < kEps) {
                    continue;
                }
                const Vec2 cc = mul(add(add(mul(A, k.wa), mul(B, k.wb)), mul(C, k.wc)), 1.0 / w);
                if (norm(sub(cc, near)) < bestDist) {
                    bestDist = norm(sub(cc, near));
                    centre = cc;
                    r = k.rad;
                }
            }
        }
    }
    if (!(r > kEps)) {
        double sum = 0;
        for (Id c : curves) {
            sum += distanceToCurve(s, c, centre);
        }
        r = std::max(sum / static_cast<double>(curves.size()), 1e-3);
    }

    const Id circle = s.addCircle(point(s, centre), r);
    for (Id c : curves) {
        s.addConstraint(CT::Tangent, {c, circle});
    }
    return circle;
}

Id arcTangent(Sketch& s, Id curve, Id start, Id end)
{
    const Vec2 ps = pos(s, start), pe = pos(s, end);
    Vec2 t;  // direction of travel leaving `start`
    if (kindOf(s, curve) == EntityKind::Line) {
        const Line& l = s.line(curve);
        if (start != l.p1 && start != l.p2) {
            throw SketchError("tangent arc: start must be an end of the line");
        }
        const Vec2 other = pos(s, start == l.p2 ? l.p1 : l.p2);
        t = unit(sub(ps, other), "tangent arc");
    }
    else if (kindOf(s, curve) == EntityKind::Arc) {
        const Arc& a = s.arc(curve);
        const Vec2 c = pos(s, a.center);
        if (start == a.end) {
            t = unit(rot90(sub(ps, c)), "tangent arc");  // carry on counter-clockwise
        }
        else if (start == a.start) {
            t = unit(mul(rot90(sub(ps, c)), -1), "tangent arc");  // carry on clockwise
        }
        else {
            throw SketchError("tangent arc: start must be an end of the arc");
        }
    }
    else {
        throw SketchError("tangent arc: continue from a line or an arc");
    }

    const Vec2 n = rot90(t);
    const Vec2 v = sub(pe, ps);
    const double denom = 2 * dot(v, n);
    if (std::abs(denom) < kEps * std::max(1.0, norm(v))) {
        throw SketchError("tangent arc: the end is straight ahead (that's a line)");
    }
    const double r = dot(v, v) / denom;  // signed: positive turns left
    const Id centre = point(s, add(ps, mul(n, r)));
    const Id arc = r > 0 ? s.addArc(centre, start, end) : s.addArc(centre, end, start);
    s.addConstraint(CT::Tangent, {curve, arc});
    return arc;
}

// =============================================================================================
// Polygons
// =============================================================================================

namespace
{
void checkSides(int n)
{
    if (n < 3 || n > 64) {
        throw SketchError("polygon: sides must be between 3 and 64");
    }
}

void closePolygon(Sketch& s, Polygon& p)
{
    const size_t n = p.vertices.size();
    for (size_t i = 0; i < n; ++i) {
        p.sides.push_back(s.addLine(p.vertices[i], p.vertices[(i + 1) % n]));
    }
    for (size_t i = 1; i < n; ++i) {
        s.addConstraint(CT::Equal, {p.sides[0], p.sides[i]});
    }
}
}  // namespace

Polygon polygon(Sketch& s, Id center, Vec2 at, int sides, PolygonKind kind)
{
    checkSides(sides);
    const Vec2 c = pos(s, center);
    const double dist = norm(sub(at, c));
    requirePositive(dist, "polygon: size");
    const double a0 = std::atan2(at.y - c.y, at.x - c.x);
    const double step = 2 * kPi / sides;

    Polygon p;
    p.center = center;
    // Regularity always comes from "corners on a circle + equal sides". (Equal sides tangent to
    // a circle is not enough for an even count: a rhombus fits that and isn't a square.)
    double cornerRadius = dist;
    double firstAngle = a0;
    if (kind == PolygonKind::Circumscribed) {
        // `at` is the middle of side 0, so the corners sit half a step either side of it.
        cornerRadius = dist / std::cos(kPi / sides);
        firstAngle = a0 - step / 2;
    }
    for (int k = 0; k < sides; ++k) {
        p.vertices.push_back(point(s, polar(c, cornerRadius, firstAngle + k * step)));
    }
    p.circle = s.addCircle(center, cornerRadius);
    s.entity(p.circle).construction = true;
    closePolygon(s, p);
    for (Id v : p.vertices) {
        s.addConstraint(CT::Coincident, {v, p.circle});
    }
    if (kind == PolygonKind::Circumscribed) {
        // The inscribed circle too (across-flats size), touching one side; the rest follow.
        p.innerCircle = s.addCircle(center, dist);
        s.entity(p.innerCircle).construction = true;
        s.addConstraint(CT::Tangent, {p.sides[0], p.innerCircle});
    }
    return p;
}

Polygon polygonEdge(Sketch& s, Id a, Id b, int sides, bool left)
{
    checkSides(sides);
    const Vec2 pa = pos(s, a), pb = pos(s, b);
    const double len = norm(sub(pb, pa));
    Vec2 n = rot90(unit(sub(pb, pa), "edge polygon"));
    if (!left) {
        n = mul(n, -1);
    }
    const Vec2 c = add(mul(add(pa, pb), 0.5), mul(n, (len / 2) / std::tan(kPi / sides)));
    const double radius = len / (2 * std::sin(kPi / sides));
    const double step = (left ? 1 : -1) * 2 * kPi / sides;
    const double a0 = std::atan2(pa.y - c.y, pa.x - c.x);

    Polygon p;
    p.center = point(s, c);
    p.vertices = {a, b};
    for (int k = 2; k < sides; ++k) {
        p.vertices.push_back(point(s, polar(c, radius, a0 + k * step)));
    }
    p.circle = s.addCircle(p.center, radius);
    s.entity(p.circle).construction = true;
    closePolygon(s, p);
    for (Id v : p.vertices) {
        s.addConstraint(CT::Coincident, {v, p.circle});
    }
    return p;
}

// =============================================================================================
// Slots
// =============================================================================================

Slot slot(Sketch& s, Id center1, Id center2, double width)
{
    requirePositive(width, "slot: width");
    const Vec2 c1 = pos(s, center1), c2 = pos(s, center2);
    const Vec2 u = unit(sub(c2, c1), "slot");
    const Vec2 n = rot90(u);
    const double r = width / 2;

    // Corners: top runs c1->c2 on the left of the axis, bottom runs back on the right.
    const Id topStart = point(s, add(c1, mul(n, r)));
    const Id topEnd = point(s, add(c2, mul(n, r)));
    const Id bottomStart = point(s, sub(c2, mul(n, r)));
    const Id bottomEnd = point(s, sub(c1, mul(n, r)));

    Slot out;
    out.centers = {center1, center2};
    const Id top = s.addLine(topStart, topEnd);
    const Id bottom = s.addLine(bottomStart, bottomEnd);
    // Caps, counter-clockwise: round the far end of c2 from bottom to top, and of c1 from top
    // to bottom.
    const Id cap2 = s.addArc(center2, bottomStart, topEnd);
    const Id cap1 = s.addArc(center1, topStart, bottomEnd);
    out.curves = {top, cap2, bottom, cap1};
    out.centerline = s.addLine(center1, center2);
    s.entity(out.centerline).construction = true;

    for (Id side : {top, bottom}) {
        for (Id cap : {cap1, cap2}) {
            s.addConstraint(CT::Tangent, {side, cap});
        }
    }
    s.addConstraint(CT::Equal, {cap1, cap2});
    return out;
}

Slot slotOverall(Sketch& s, Vec2 end1, Vec2 end2, double width)
{
    requirePositive(width, "slot: width");
    const Vec2 u = unit(sub(end2, end1), "slot");
    if (norm(sub(end2, end1)) <= width + kEps) {
        throw SketchError("slot: the overall length must be more than the width");
    }
    const double r = width / 2;
    return slot(s, point(s, add(end1, mul(u, r))), point(s, sub(end2, mul(u, r))), width);
}

Slot slotCenterPoint(Sketch& s, Id center, Vec2 capCenter, double width)
{
    const Vec2 m = pos(s, center);
    Slot out = slot(s, point(s, sub(mul(m, 2), capCenter)), point(s, capCenter), width);
    s.addConstraint(CT::Midpoint, {center, out.centerline});
    out.midpoint = center;
    return out;
}

Slot arcSlot(Sketch& s, Id center, Vec2 start, Vec2 end, double width)
{
    requirePositive(width, "arc slot: width");
    const Vec2 c = pos(s, center);
    const double R = norm(sub(start, c));
    const double h = width / 2;
    if (R <= h + kEps) {
        throw SketchError("arc slot: the width must be less than the diameter of the centreline");
    }
    const double aS = std::atan2(start.y - c.y, start.x - c.x);
    const double aE = std::atan2(end.y - c.y, end.x - c.x);

    Slot out;
    const Id S = point(s, polar(c, R, aS));
    const Id E = point(s, polar(c, R, aE));
    out.centers = {S, E};
    out.centerline = s.addArc(center, S, E);
    s.entity(out.centerline).construction = true;

    const Id oS = point(s, polar(c, R + h, aS)), oE = point(s, polar(c, R + h, aE));
    const Id iS = point(s, polar(c, R - h, aS)), iE = point(s, polar(c, R - h, aE));
    const Id outer = s.addArc(center, oS, oE);
    const Id inner = s.addArc(center, iS, iE);
    const Id capS = s.addArc(S, iS, oS);  // bulges back past the start
    const Id capE = s.addArc(E, oE, iE);  // bulges on past the end
    out.curves = {outer, capE, inner, capS};

    // Each cap meets both sides tangentially at their shared ends; together they leave just the
    // width (an extra "equal caps" would be redundant). Even so, one of the arcs' own rules is
    // implied by the rest (the centreline, outer, inner and one cap fix the other cap's radius),
    // which the solver allows for when the slot is fully constrained.
    s.addConstraint(CT::Tangent, {capS, outer});
    s.addConstraint(CT::Tangent, {capS, inner});
    s.addConstraint(CT::Tangent, {capE, outer});
    s.addConstraint(CT::Tangent, {capE, inner});
    return out;
}

Slot arcSlot3(Sketch& s, Vec2 start, Vec2 end, Vec2 through, double width)
{
    Vec2 c;
    double r = 0;
    if (!circumcircle(start, through, end, c, r)) {
        throw SketchError("3-point arc slot: the points are in a line");
    }
    // Walking start -> through -> end clockwise means the ccw centreline runs end -> start.
    const bool ccw = cross(sub(through, start), sub(end, start)) > 0;
    const Id centre = point(s, c);
    return ccw ? arcSlot(s, centre, start, end, width) : arcSlot(s, centre, end, start, width);
}

}  // namespace core::build
