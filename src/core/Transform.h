#pragma once

#include "Geometry.h"
#include "Sketch.h"

#include <map>
#include <vector>

// Moving, copying, scaling and mirroring groups of entities.

namespace core
{

// x' = a*x + b*y + tx,  y' = c*x + d*y + ty.
struct Affine
{
    double a = 1.0, b = 0.0, c = 0.0, d = 1.0;
    double tx = 0.0, ty = 0.0;

    Vec2 apply(Vec2 p) const { return {a * p.x + b * p.y + tx, c * p.x + d * p.y + ty}; }
    Vec2 applyLinear(Vec2 v) const { return {a * v.x + b * v.y, c * v.x + d * v.y}; }
    double det() const { return a * d - b * c; }
    double scale() const;  // how lengths change (uniform maps)
    bool mirrors() const { return det() < 0.0; }

    static Affine translation(double dx, double dy);
    static Affine rotation(Vec2 about, double degrees);  // counter-clockwise
    static Affine scaling(Vec2 about, double factor);
    static Affine mirror(Vec2 a, Vec2 b);  // across the line through a and b
    Affine then(const Affine& next) const;  // this, followed by next
};

namespace xform
{

// The entities a selection stands for: the selected points and curves plus the points that
// define those curves (so a line brings its endpoints). Constraints in the selection, the axes
// and the axes' hidden ends are left out.
std::vector<Id> expand(const Sketch& sketch, const std::vector<Id>& selection);

// Copies the selected entities from `from` into `to`, mapped by `t`, and returns old -> new ids
// for every copied entity and constraint. Copies are never fixed (the origin comes along as a
// plain point). With `constraints`, constraints among the copied entities come too: dimensions
// scale with the map, horizontal/vertical ones are kept, swapped or dropped depending on the
// rotation, and features are left out. A mirroring map reverses arcs so they stay
// counter-clockwise.
std::map<Id, Id> copy(const Sketch& from, Sketch& to, const std::vector<Id>& selection, const Affine& t,
                      bool constraints = true);
inline std::map<Id, Id> copy(Sketch& sketch, const std::vector<Id>& selection, const Affine& t,
                             bool constraints = true)
{
    return copy(sketch, sketch, selection, t, constraints);
}

// Moves the selected entities in place, without solving: positions and radii are set directly,
// and length dimensions between them scale with the map. Throws SketchError for fixed geometry
// or a mirroring map.
void apply(Sketch& sketch, const std::vector<Id>& selection, const Affine& t);

// The selection as a sketch of its own, with the constraints among it: what the clipboard holds.
Sketch extract(const Sketch& from, const std::vector<Id>& selection);

// Centre of the bounding box of a fragment's points: the spot that follows the cursor when
// pasting.
Vec2 anchor(const Sketch& fragment);

// Copies everything in `fragment` into `to`, moved so its anchor lands on `at`. Returns
// fragment ids -> new ids.
std::map<Id, Id> paste(Sketch& to, const Sketch& fragment, Vec2 at);

// Mirrors the selection across `line`, keeping each copy symmetric to its original with
// Symmetric constraints (and Equal for circles). Points on the mirror line are shared rather
// than copied. Returns old -> new ids of the mirrored entities.
std::map<Id, Id> mirror(Sketch& sketch, const std::vector<Id>& selection, Id line);

}  // namespace xform
}  // namespace core
