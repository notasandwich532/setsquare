#pragma once

#include "Sketch.h"

#include <set>
#include <vector>

// Groups (ConstraintType::Group): geometry held rigidly together, like Fusion's rigid group. The
// members keep their places relative to each other, but the group as a whole is free to move and
// turn, unless something else holds it (a fixed point, a constraint to the origin or axes, ...).
//
// A group lists its points (the first two are its anchors) and circles (whose radii it keeps).
// Every further point keeps its place in the frame of the anchors: p = a0 + s*u + t*perp(u), with
// u = a1 - a0 kept at its length. Constraints wholly inside a group are carried by it, so the
// solver leaves them out (they'd say again what the group says); except the first that sets an
// orientation (horizontal, vertical, or a horizontal / vertical distance), which then stops the
// group turning, as it stopped its members.

namespace core::group
{

struct Members
{
    std::vector<Id> points;   // the first two: the anchors
    std::vector<Id> circles;  // radii kept
};
Members members(const Sketch& sketch, const Constraint& group);

// Makes a group of the items (curves and points, with the points they're built on; not the
// origin or axes), as they are now. Returns its id. Throws SketchError unless there are at least
// two different places in it.
Id create(Sketch& sketch, const std::vector<Id>& items);

// Whether an entity is wholly inside a group (a curve: all the points it's built on; a circle:
// it's a member, so its radius is kept too).
bool covers(const Sketch& sketch, const Members& members, Id entity);

// The group a constraint lies wholly inside, if any (kNoId).
Id holder(const Sketch& sketch, Id constraint);

// Constraints that tie their geometry to the sketch's directions as well as to each other.
bool setsOrientation(ConstraintType type);

}  // namespace core::group
