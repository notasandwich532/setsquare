#pragma once

#include "Geometry.h"
#include "Sketch.h"

#include <functional>
#include <memory>
#include <set>
#include <vector>

namespace core
{

struct SolveResult
{
    bool ok = false;  // solved, and no conflicting constraints
    int dof = 0;      // remaining degrees of freedom
    std::vector<Id> conflicting;         // constraint ids that over-constrain inconsistently
    std::vector<Id> redundant;           // constraint ids that are implied by others
    std::vector<Id> partiallyRedundant;  // constraint ids partly implied by others
    std::set<Id> fullyConstrained;       // entity ids with no remaining freedom
    std::set<Id> failedLayers;           // layers that couldn't be solved (or conflict)
};

// Maps a Sketch onto a PlaneGCS system. The system is built once from a snapshot of the sketch;
// rebuild (construct a new Solver) after the sketch's structure changes. Results are copied
// back to the sketch with writeBack().
class Solver
{
public:
    explicit Solver(const Sketch& sketch);
    // Just one layer's geometry and constraints (with the origin and axes; projections in it are
    // fixed). kNoId, or a sketch with one layer: all of it.
    Solver(const Sketch& sketch, Id layer);
    ~Solver();
    Solver(const Solver&) = delete;
    Solver& operator=(const Solver&) = delete;

    // Full solve with diagnosis (DOF, conflicts, per-entity constrainedness).
    SolveResult solve();

    // Copies solved positions/radii into the sketch.
    void writeBack(Sketch& sketch) const;

    // Interactive dragging: the given points are pulled towards their start position plus an
    // offset with low priority, so real constraints always win. Fixed points are ignored.
    void beginDrag(const std::vector<Id>& points);
    // Drags the rim of a circle or arc: the curve is pulled through the grabbed position
    // (x, y) + offset, so a free circle grows/shrinks about its centre.
    void beginDragEdge(Id roundEntity, double x, double y);
    bool dragTo(double dx, double dy);  // returns false if the solver could not converge
    // Like dragTo, but each target is `to` of where it started (e.g. a rotation).
    bool dragMap(const std::function<Vec2(Vec2)>& to);
    void endDrag();
    // The drag couldn't move points the solve calls free: they're held, but only exactly (a
    // singular arrangement, e.g. a point on an ellipse as far from its centre as the major
    // radius, which can only be at the tip), which the diagnosis can't see.
    bool dragLocked() const;

private:
    struct Impl;
    std::unique_ptr<Impl> d;
};

// Convenience: build, solve, and write back when the solve succeeded. Layers are solved one by
// one (each on its own: nothing ties them but projections), those projected from first, each
// written back if it solved; the result is all of theirs together.
SolveResult solve(Sketch& sketch);

// Like solve, but where the sketch has freedom left, the given points stay put if they can
// (e.g. a pattern's source when its spacing changes, so the copies move instead).
SolveResult solveHolding(Sketch& sketch, const std::vector<Id>& points);

}  // namespace core
