#pragma once

#include <functional>
#include <optional>
#include <string>
#include <vector>

// Arithmetic typed into number inputs: "20/3", "(100 - 2*15) / 2", "sqrt(2) * 10", "2^3",
// "1in + 3mm". Lengths are in mm and angles in degrees, as everywhere in the app.
//
//   + - * / ^ (power, right to left)  % (remainder)  brackets  unary minus
//   a number followed by a bracket, a name or a unit multiplies: 2pi, 3(4 + 1), 10 cm
//   pi  tau  e
//   units (multiply into mm / degrees): mm  cm  m  in  ft  deg  °
//   sqrt abs round floor ceil exp ln log (base 10)  sin cos tan asin acos atan (degrees)
//   atan2(y, x)  hypot(a, b)  min(a, b, ...)  max(a, b, ...)
//
// Other names are an error, saying where, unless a lookup knows them (the drawing's parameters).
namespace core
{

// The value of `text`. Throws SketchError (with a short reason) if it doesn't read, or comes out
// as no number at all (dividing by zero, sqrt(-1), ...).
double evaluate(const std::string& text);

// As evaluate, but false instead of throwing (and `value` untouched).
bool tryEvaluate(const std::string& text, double& value, std::string* error = nullptr);

// As evaluate, with names that aren't constants, units or functions looked up: `lookup` gives a
// name's value, or nullopt if it doesn't know it (an error then, as without one).
using NameLookup = std::function<std::optional<double>(const std::string& name)>;
double evaluate(const std::string& text, const NameLookup& lookup);

// The names in `text` that would be looked up (not constants, units or functions), in order,
// each once, as written. Doesn't check the rest of it.
std::vector<std::string> namesIn(const std::string& text);

// Whether `name` is one of evaluate's own (a constant, unit or function), compared as it does:
// ignoring case.
bool isBuiltInName(const std::string& name);

}  // namespace core
