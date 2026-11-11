# PlaneGCS (vendored)

2D geometric constraint solver from FreeCAD's Sketcher workbench.

- Source: https://github.com/FreeCAD/FreeCAD, `src/Mod/Sketcher/App/planegcs`
- Commit: `faf7e671f47eb04f8d6a5a52a721ac42a342f276`
- License: LGPL-2.1-or-later (see `LICENSE`)

## Local modifications
- `Geo.h`, `Constraints.h`, `GCS.h`: `#include "../../SketcherGlobal.h"` changed to `#include "SketcherGlobal.h"`.
- `GCS.h`/`GCS.cpp`: `OptionalTagBase`. Tags at or above it mark optional parts of a constraint
  (e.g. a symmetry's "square to the line", implied when the points are already level), and the
  redundancy heuristic in `System::identifyConflictingRedundantConstraints` picks them first.
  Search for "(drawapp)".
- `GCS.cpp`, `System::solve(Algorithm, bool)`: returns the worst status of the independent parts,
  not just the last part's (one part failing used to be reported as success when a later one
  solved).
- `shim/` provides stand-ins for the FreeCAD headers PlaneGCS pulls in
  (`SketcherGlobal.h`, `Base/Console.h`, `Base/Tools.h`, `FCConfig.h`, `boost_graph_adjacency_list.hpp`).

To update: copy the new upstream files over `planegcs/`, re-apply the include change, rebuild.
