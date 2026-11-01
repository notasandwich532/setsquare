#pragma once

#include <string>

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
// No variables or other names: anything else is an error, saying where.
namespace core
{

// The value of `text`. Throws SketchError (with a short reason) if it doesn't read, or comes out
// as no number at all (dividing by zero, sqrt(-1), ...).
double evaluate(const std::string& text);

// As evaluate, but false instead of throwing (and `value` untouched).
bool tryEvaluate(const std::string& text, double& value, std::string* error = nullptr);

}  // namespace core
