#pragma once

#include "Features.h"
#include "Sketch.h"

#include <map>

#include <string>
#include <vector>

// The drawing's parameters: named values ("width" = "120", "hole" = "width / 10") that dimensions'
// expressions use ("width / 2 + 3"). Parameters may use other parameters; dimensions use
// parameters only. Names are matched ignoring case, as the expression's own names are.
namespace core::parameters
{

// Whether `name` can name a parameter: letters, digits and _, starting with a letter or _, and not
// one of the expression's own names (pi, mm, sqrt, ...). If not, *why says why.
bool validName(const std::string& name, std::string* why = nullptr);

// The parameter of that name (ignoring case), or -1.
int find(const Sketch& sketch, const std::string& name);

// Works out every parameter, in the order they use each other, then every dimension with an
// expression, setting their values. Throws SketchError saying what's wrong: a name it doesn't
// know, parameters that use each other in a loop, or an expression that doesn't come out (or
// comes out as a value the dimension can't have).
void update(Sketch& sketch);

// What `expression` comes to with the drawing's parameters as they are. Throws SketchError.
double valueOf(const Sketch& sketch, const std::string& expression);

// A pattern's settings with those given as expressions (Constraint::settings) worked out; a count
// that doesn't come out whole is an error. Throws SketchError.
void applySettings(const Sketch& sketch, const std::map<std::string, std::string>& settings,
                   feature::LinearPatternParams& params);
void applySettings(const Sketch& sketch, const std::map<std::string, std::string>& settings,
                   feature::CircularPatternParams& params);

// What uses a parameter: other parameters (by name), and dimensions, patterns and offsets.
struct Users
{
    std::vector<std::string> parameters;
    std::vector<Id> dimensions;  // (and patterns and offsets)
    bool empty() const { return parameters.empty() && dimensions.empty(); }
};
Users usersOf(const Sketch& sketch, const std::string& name);

// `expression` with the name `from` (whole, ignoring case) written as `to` instead.
std::string renamed(const std::string& expression, const std::string& from, const std::string& to);

}  // namespace core::parameters
