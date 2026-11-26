#pragma once

#include "Geometry.h"
#include "Sketch.h"

#include <utility>
#include <vector>

// Editing operations that cut and join curves: trim, extend, break, fillet and chamfer.
// Each works on lines, arcs and (where it makes sense) circles, and throws SketchError when it
// can't apply. Operations take a `near` position (where the user clicked) to choose which part
// of a curve is meant; replaying with the same positions gives the same result.
//
// Any geometry in the curve's layer (construction too, as in Fusion) acts as a cutting edge,
// but never the origin/axes or the page's edges.

namespace core::modify
{

// Where two curves cross or touch, within their actual extents.
std::vector<Vec2> intersections(const Sketch& sketch, Id a, Id b);

// The curves the segment from `a` to `b` crosses (construction included, origin/axes not), with
// where, in order from `a`. For drag-to-trim.
struct Crossing
{
    Id curve = kNoId;
    Vec2 at;
};
std::vector<Crossing> crossings(const Sketch& sketch, Vec2 a, Vec2 b);

// Removes the part of `curve` around `near`, up to the nearest crossings with other curves; a
// curve that crosses nothing is removed whole. A circle cut at two or more crossings becomes
// an arc (keeping its constraints). New ends stay on the curves that cut them.
void trim(Sketch& sketch, Id curve, Vec2 near);

// Like trim, but the piece is kept as construction geometry instead of removed (still joined to
// the rest: collinear / on the same circle). Returns the construction piece.
Id trimToConstruction(Sketch& sketch, Id curve, Vec2 near);

// A line as it was before trimming to construction cut it up: the far ends of the run of
// collinear pieces it's joined to end to end, through construction pieces (a piece the trim
// turned to construction, and the pieces either side of it). Its own ends when it hasn't been
// cut so. Lines are equal by this, so trimming part of one to construction doesn't change it.
std::pair<Id, Id> wholeLine(const Sketch& sketch, Id line);

// Lengthens the end of a line or arc nearest `near` until it meets another curve.
void extend(Sketch& sketch, Id curve, Vec2 near);

// Splits `curve` at the crossings either side of `near`, without removing anything. Returns
// the pieces (the original id is reused for one of them). Line pieces stay collinear, arc
// pieces on the same circle.
std::vector<Id> breakCurve(Sketch& sketch, Id curve, Vec2 near);

// After cutting: removes "point on curve" constraints whose point has become one of the curve's
// own ends (a point kept on a circle that was then trimmed there), which say nothing and show as
// redundant. Returns how many.
int dropTrivialCoincidences(Sketch& sketch);

// What would happen at `near` (for previews): the part trim would remove, the extension extend
// would add, or the piece break would separate. Empty when the operation doesn't apply.
std::vector<Vec2> trimPreview(const Sketch& sketch, Id curve, Vec2 near);
std::vector<Vec2> extendPreview(const Sketch& sketch, Id curve, Vec2 near);
std::vector<Vec2> breakPreview(const Sketch& sketch, Id curve, Vec2 near);

struct Corner
{
    std::vector<Id> pieces;  // new solid segments along the original curves
    Id joint = kNoId;        // the fillet arc or chamfer line
    std::vector<Id> dimensions;
};

// Rounds the corner between two lines/arcs with a tangent arc of `radius`. `nearA`/`nearB` say
// which side of each curve to keep (where they were clicked). The original curves become
// construction geometry, so the sharp corner and anything dimensioned to it survive; the new
// solid pieces and the arc carry the outline. Adds a radius dimension.
Corner fillet(Sketch& sketch, Id a, Vec2 nearA, Id b, Vec2 nearB, double radius);

enum class ChamferKind
{
    EqualDistance,  // value1 from the corner along both lines
    TwoDistances,   // value1 along a, value2 along b
    DistanceAngle,  // value1 along a, value2 degrees between a and the chamfer
};

// Cuts the corner between two lines with a straight chamfer, kept as construction like fillet.
// Adds dimensions for the chosen kind.
Corner chamfer(Sketch& sketch, Id a, Vec2 nearA, Id b, Vec2 nearB, ChamferKind kind, double value1,
               double value2 = 0.0);

// The two curves meeting at `point` (a shared end of exactly two solid lines/arcs), for picking a
// corner by clicking its vertex. Returns false if it isn't such a corner; otherwise fills in the
// curves and positions along each, away from the corner.
bool cornerAt(const Sketch& sketch, Id point, Id& a, Vec2& nearA, Id& b, Vec2& nearB);

}  // namespace core::modify
