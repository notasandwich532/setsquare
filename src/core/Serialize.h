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
// The recorder's version: what each recorded command makes. History refers to what was made by
// its number (l26), so a history recorded when commands made something else no longer lines up.
// Bump it whenever a command's recording would replay differently.
constexpr int kRecorderVersion = 1;

// `recorder`: the version the history was recorded by (saved with it; see kRecorderVersion).
std::string toJson(const Sketch& sketch, bool compact = false, const std::vector<std::string>* history = nullptr,
                   int recorder = kRecorderVersion);

// Throws SketchError with a readable message for malformed or unsupported files. The stored
// command history, if any, goes to *history.
// ecorder: set to the version its history was recorded by (0: before versions were saved).
Sketch fromJson(const std::string& text, std::vector<std::string>* history = nullptr, int* recorder = nullptr);

}  // namespace core
