# PlaneGCS (vendored)

2D geometric constraint solver from FreeCAD's Sketcher workbench.

- Source: https://github.com/FreeCAD/FreeCAD, `src/Mod/Sketcher/App/planegcs`
- Commit: `faf7e671f47eb04f8d6a5a52a721ac42a342f276`
- License: LGPL-2.1-or-later (see `LICENSE`)

## Local modifications
- `Geo.h`, `Constraints.h`, `GCS.h`: `#include "../../SketcherGlobal.h"` changed to `#include "SketcherGlobal.h"`.
- `shim/` provides stand-ins for the FreeCAD headers PlaneGCS pulls in
  (`SketcherGlobal.h`, `Base/Console.h`, `Base/Tools.h`, `FCConfig.h`, `boost_graph_adjacency_list.hpp`).

To update: copy the new upstream files over `planegcs/`, re-apply the include change, rebuild.
