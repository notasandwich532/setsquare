#pragma once

#include "Geometry.h"
#include "Sketch.h"

#include <array>
#include <vector>

// Any curve of a sketch as a parametric path, for the code that has to treat them all alike:
// picking, trimming, finding fill areas, drawing and export. Lines, circles and arcs have exact
// formulas here; ellipses, elliptical arcs and splines (made of cubic Bezier pieces) too, and
// what has no formula (nearest points, crossings) is found numerically.

namespace core::curve
{

struct Bezier
{
    std::array<Vec2, 4> p;
    Vec2 at(double t) const;
    Vec2 derivative(double t) const;
    Bezier part(double from, double to) const;  // the same curve between those parameters
};

// The cubic pieces of a spline: through the points (fit: centripetal Catmull-Rom), or pulled by
// them (control: a clamped cubic B-spline, or a periodic one when closed).
std::vector<Bezier> splineBeziers(const std::vector<Vec2>& points, SplineKind kind, bool closed);
std::vector<Vec2> splinePoints(const Sketch& sketch, const Spline& spline);  // its points' positions

// An ellipse: centre, unit direction of the major axis, and the two semi-axes. The parameter is
// the angle in its own frame: c + a cos(t) u + b sin(t) rot90(u), counter-clockwise.
struct EllipseFrame
{
    Vec2 c;
    Vec2 u{1, 0};
    double a = 1, b = 1;
    Vec2 at(double t) const;
    Vec2 derivative(double t) const;
    double param(Vec2 p) const;  // of the point of the ellipse in p's direction (exact on it)
};
EllipseFrame ellipseFrame(const Sketch& sketch, Id ellipseOrArc);

class Path
{
public:
    enum class Kind
    {
        Line,
        Circle,
        Arc,
        Ellipse,
        EllipseArc,
        Spline,
    };

    // Parameters run from 0 to end(): 0..1 along a line; the angle from the start for circles,
    // arcs, ellipses and elliptical arcs; along a spline, one unit per Bezier piece.
    Kind kind() const { return m_kind; }
    double end() const;
    bool closed() const;  // circle, ellipse, whole closed spline (the parameter wraps)
    Vec2 at(double t) const;
    Vec2 derivative(double t) const;
    // The parameter of the nearest point of the curve to p (exact for lines, circles, arcs).
    double param(Vec2 p) const;
    Vec2 closest(Vec2 p) const { return at(param(p)); }
    double distance(Vec2 p) const;
    // n + 1 points from `from` to `to` (in parameter).
    std::vector<Vec2> sample(double from, double to, int n) const;
    // Enough points to draw or export it smoothly (about `perUnit` per quarter turn / piece).
    std::vector<Vec2> polyline(int perUnit = 16) const;
    double length() const;
    // A spline path as Bezier pieces from its start to its end (cut where it starts or ends part
    // way along one).
    std::vector<Bezier> beziers() const;

    static Path line(Vec2 a, Vec2 b);
    static Path circle(Vec2 c, double r);
    static Path arc(Vec2 c, double r, double start, double sweep);
    static Path ellipse(const EllipseFrame& e);
    static Path ellipseArc(const EllipseFrame& e, double start, double sweep);
    // A spline's pieces; [from, from + span] of them (wrapping round a closed one).
    static Path spline(std::vector<Bezier> pieces, bool closed, double from, double span);

    // What the path is made of (for drawing and export).
    Vec2 a, b;                // line
    Vec2 c;                   // circle, arc
    double r = 0;             // circle, arc
    double start = 0;         // arc, ellipse arc (angle); spline (parameter of its start)
    double sweep = 0;         // arc, ellipse, ellipse arc, spline
    EllipseFrame ellipseShape;
    std::vector<Bezier> pieces;  // spline: the whole curve's
    bool closedSpline = false;   // spline: the whole curve is closed

private:
    Kind m_kind = Kind::Line;
    Vec2 splineAt(double u, bool derivative) const;
};

// The path of a curve (line, circle, arc, ellipse, elliptical arc or spline).
Path pathOf(const Sketch& sketch, Id curve);

// Where two curves cross (within their extents), found numerically to about 1e-9 of their size.
std::vector<Vec2> intersections(const Path& p1, const Path& p2);

}  // namespace core::curve
