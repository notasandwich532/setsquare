#pragma once

#include "Sketch.h"

#include <string>
#include <vector>

// Sketch <-> JSON (.setsq files). The format is versioned; ids are kept so that files diff well
// and scripts can refer to things by id.
//
// {
//   "format": "setsquare.sketch", "version": 1, "origin": 1, "axes": [4, 5],
//   "entities": [ {"id": 1, "type": "point", "x": 0, "y": 0, "fixed": true},
//                 {"id": 4, "type": "line", "p1": 2, "p2": 3, "construction": true}, ... ],
//   "constraints": [ {"id": 9, "type": "distance", "refs": [4], "value": 40, "label": [0, 8]}, ... ]
// }

namespace core
{

inline constexpr int kFileVersion = 1;

// `history`, if given, is stored too: the commands (Python) that built the sketch.
std::string toJson(const Sketch& sketch, bool compact = false, const std::vector<std::string>* history = nullptr);

// Throws SketchError with a readable message for malformed or unsupported files. The stored
// command history, if any, goes to *history.
Sketch fromJson(const std::string& text, std::vector<std::string>* history = nullptr);

}  // namespace core
