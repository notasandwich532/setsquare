#pragma once

#include "Geometry.h"
#include "Sketch.h"
#include "Transform.h"

#include <set>
#include <vector>

// Linked copies: rectangular and circular patterns, and offsets. Each is one feature constraint
// (ConstraintType::LinearPattern / CircularPattern / Offset) that keeps the generated geometry
// tied to its source, so editing the source updates the copies, and changing the feature's value
// (spacing, total angle, offset distance) moves them. Deleting the feature constraint leaves the
// copies as ordinary, unlinked geometry; deleting a source or copy deletes the feature too.

namespace core::feature
{

// --- patterns ---

struct LinearPatternParams
{
    int count = 3;         // along the first direction, including the original
    double spacing = 10.0;
    double angle = 0.0;    // degrees: the first direction; the second is 90 degrees on from it
    int count2 = 1;
    double spacing2 = 10.0;  // negative: the rows go the other way
    bool symmetric = false;   // `count` each way along the first direction (sharing the original)
    bool symmetric2 = false;  // likewise along the second
};

struct CircularPatternParams
{
    int count = 6;         // including the original
    double angle = 360.0;  // total: a full turn spaces the copies evenly around it, less spans it
};

// A pattern's references, decoded.
struct Pattern
{
    Id center = kNoId;  // circular patterns
    std::vector<Id> sources;
    std::vector<std::vector<Id>> copies;  // per instance after the original, matching `sources`
};
Pattern pattern(const Constraint& c);
LinearPatternParams linearParams(const Constraint& c);
CircularPatternParams circularParams(const Constraint& c);

// Where instance k sits (0 is the original): the map from the sources to its copies.
Affine instanceTransform(const Sketch& sketch, const Constraint& c, size_t k);

// The selection's entities that a pattern repeats: curves and lone points (not the endpoints of
// selected curves, which come along anyway).
std::vector<Id> patternSources(const Sketch& sketch, const std::vector<Id>& selection);

// Each returns the pattern constraint's id.
Id linearPattern(Sketch& sketch, const std::vector<Id>& selection, const LinearPatternParams& params);
Id circularPattern(Sketch& sketch, const std::vector<Id>& selection, Id center, const CircularPatternParams& params);

// Changes a pattern's settings. A different count regenerates the copies, and the pattern gets
// a new id (returned).
Id setLinearPattern(Sketch& sketch, Id pattern, const LinearPatternParams& params);
Id setCircularPattern(Sketch& sketch, Id pattern, const CircularPatternParams& params);

// Puts a pattern's copies exactly where its settings say (from the sources' current positions),
// so a solve after changing the settings starts from the answer. Patterns only.
void placeCopies(Sketch& sketch, Id pattern);

// Patterns' texts (Constraint::texts): each instance has a copy of each, on the instance's copies
// of what it hangs on, turned as the instance is, looking just like the original (made to, after
// any change: the copies aren't edited on their own). Copies of patterns or texts that are gone,
// or instances there no longer are, go. Their letters, where they changed, need making again
// (layoutKey 0: see textshapes). Returns whether anything changed.
bool syncPatternTexts(Sketch& sketch);

// The points that define a feature's source geometry (and a circular pattern's centre): held
// in place when the feature's value changes, so the copies move rather than the original.
std::vector<Id> sourcePoints(const Sketch& sketch, const Constraint& c);

// --- offsets ---

struct OffsetRefs
{
    std::vector<Id> sources;
    std::vector<Id> offsets;   // offsets[i] is sources[i]'s offset
    std::vector<Id> offsets2;  // both-sides offsets: the other side's (else empty)
    std::vector<Id> caps;      // capped both-sides offsets: the arcs round the chain's ends
};
OffsetRefs offsetRefs(const Constraint& c);

// The curves joined end to end with `curve` (through points where exactly two of them meet),
// in order along the chain. A circle is a chain by itself.
std::vector<Id> chainAt(const Sketch& sketch, Id curve);

// Curves joined end to end, in order along their chain (from a free end, if it has one). Throws
// SketchError if they branch, aren't one connected chain, or one of them is closed (a circle, say).
std::vector<Id> chainOrder(const Sketch& sketch, const std::vector<Id>& curves);

// Offsets a chain of lines and arcs (or one circle) by `distance` towards the side `toward` is
// on. Neighbouring offset curves meet where they cross (sharp corners) or continue smoothly
// (tangent joins). With `bothSides`, it offsets to both sides at once (one distance drives
// both; `toward` doesn't matter), and with `caps` too, an open chain's ends are closed with
// half circles round its end points, tangent to both sides: a slot-like outline. Returns the
// offset constraint's id.
Id offset(Sketch& sketch, const std::vector<Id>& curves, double distance, Vec2 toward, bool bothSides = false,
          bool caps = false);

// An ellipse's or spline's offset is a spline through points at the distance from it, evenly
// along it. Those points (all but its ends, which join the chain's other offsets and are solved)
// follow the source exactly: placed from it after each solve, not solved. Returns whether any
// moved. (`placedOffsetPoints`: which points those are, for the solver to leave alone.)
bool placeOffsetPoints(Sketch& sketch);
std::set<Id> placedOffsetPoints(const Sketch& sketch);

// --- dividing a line ---
//
// Points spaced along a line and kept there as the line moves (ConstraintType::Divide, refs
// (line, points...), value = the number of points). The gaps from one end of the line, past each
// point, to the other end are in the ratio `parts`: n points evenly is n + 1 equal parts, and
// 3:2:1 puts two points with the biggest gap first. `reversed` measures from the line's second
// end (p2) instead of its first.

struct DivideParams
{
    std::vector<double> parts{1.0, 1.0};  // one more than the points; all positive
    bool reversed = false;
    bool byRatio = false;  // how it was asked for (the command panel shows that way again)
};

DivideParams evenly(int points);
// "3:2:1" (or with spaces/commas) -> {3, 2, 1}. Throws SketchError unless it's at least two
// positive numbers.
std::vector<double> parseRatio(const std::string& text);
std::string ratioText(const std::vector<double>& parts);  // {3, 2, 1} -> "3:2:1"

DivideParams divideParams(const Constraint& c);
// Where each point goes, as fractions along the line from p1 (0) to p2 (1), in point order.
std::vector<double> divideFractions(const DivideParams& p);

// Adds the points and the feature; returns the feature's id.
Id divide(Sketch& sketch, Id line, const DivideParams& params);
// Changes the spacing. With a different number of points, they're made afresh (and the feature
// gets a new id, returned).
Id setDivide(Sketch& sketch, Id divide, const DivideParams& params);
// Puts the points exactly where the settings say along the line as it is now.
void placeDividePoints(Sketch& sketch, Id divide);

}  // namespace core::feature
