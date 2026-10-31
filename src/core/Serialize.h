#pragma once

#include "Sketch.h"

#include <string>

// Sketch <-> JSON (.dsk files). The format is versioned; ids are kept so that files diff well
// and scripts can refer to things by id.
//
// {
//   "format": "drawing_app.sketch", "version": 1, "origin": 1,
//   "entities": [ {"id": 1, "type": "point", "x": 0, "y": 0, "fixed": true},
//                 {"id": 4, "type": "line", "p1": 2, "p2": 3, "construction": true}, ... ],
//   "constraints": [ {"id": 9, "type": "distance", "refs": [4], "value": 40, "label": [0, 8]}, ... ]
// }

namespace core
{

inline constexpr int kFileVersion = 1;

std::string toJson(const Sketch& sketch);

// Throws SketchError with a readable message for malformed or unsupported files.
Sketch fromJson(const std::string& text);

}  // namespace core
