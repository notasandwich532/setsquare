#pragma once

#include "Geometry.h"
#include "Sketch.h"
#include "Transform.h"

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
    double spacing2 = 10.0;
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

// The points that define a feature's source geometry (and a circular pattern's centre): held
// in place when the feature's value changes, so the copies move rather than the original.
std::vector<Id> sourcePoints(const Sketch& sketch, const Constraint& c);

// --- offsets ---

struct OffsetRefs
{
    std::vector<Id> sources;
    std::vector<Id> offsets;  // offsets[i] is sources[i]'s offset
};
OffsetRefs offsetRefs(const Constraint& c);

// The curves joined end to end with `curve` (through points where exactly two of them meet),
// in order along the chain. A circle is a chain by itself.
std::vector<Id> chainAt(const Sketch& sketch, Id curve);

// Offsets a chain of lines and arcs (or one circle) by `distance` towards the side `toward` is
// on. Neighbouring offset curves meet where they cross (sharp corners) or continue smoothly
// (tangent joins). Returns the offset constraint's id.
Id offset(Sketch& sketch, const std::vector<Id>& curves, double distance, Vec2 toward);

}  // namespace core::feature
