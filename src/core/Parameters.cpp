#include "Parameters.h"

#include "Expression.h"
#include "Features.h"

#include <cmath>
#include <cstdio>

#include <algorithm>
#include <cctype>
#include <optional>

namespace core::parameters
{

namespace
{
// A number as people write it (2.5, not 2.500000).
std::string number(double v)
{
    char text[32];
    std::snprintf(text, sizeof text, "%g", v);
    return text;
}

bool sameName(const std::string& a, const std::string& b)
{
    return std::ranges::equal(a, b, [](unsigned char x, unsigned char y) { return std::tolower(x) == std::tolower(y); });
}

bool nameStart(char c)
{
    return std::isalpha(static_cast<unsigned char>(c)) || c == '_';
}
bool namePart(char c)
{
    return std::isalnum(static_cast<unsigned char>(c)) || c == '_';
}
}  // namespace

bool validName(const std::string& name, std::string* why)
{
    auto no = [&](const std::string& reason) {
        if (why) {
            *why = reason;
        }
        return false;
    };
    if (name.empty()) {
        return no("a parameter needs a name");
    }
    if (!nameStart(name.front()) || !std::ranges::all_of(name, namePart)) {
        return no("'" + name + "': a name is letters, digits and _, starting with a letter");
    }
    if (isBuiltInName(name)) {
        return no("'" + name + "' already means something in expressions (a constant, unit or function)");
    }
    return true;
}

int find(const Sketch& sketch, const std::string& name)
{
    const auto& all = sketch.parameters();
    for (size_t i = 0; i < all.size(); ++i) {
        if (sameName(all[i].name, name)) {
            return static_cast<int>(i);
        }
    }
    return -1;
}

void update(Sketch& sketch)
{
    std::vector<Parameter> all = sketch.parameters();
    enum class State { Todo, Working, Done };
    std::vector<State> state(all.size(), State::Todo);
    std::vector<std::string> path;  // (the parameters being worked out, for saying where a loop is)

    std::function<double(size_t)> valueOf = [&](size_t i) -> double {
        if (state[i] == State::Done) {
            return all[i].value;
        }
        if (state[i] == State::Working) {
            std::string loop;
            const auto from = std::ranges::find(path, all[i].name);
            for (auto it = from; it != path.end(); ++it) {
                loop += *it + " \xE2\x86\x92 ";  // →
            }
            throw SketchError("parameters use each other in a loop: " + loop + all[i].name);
        }
        state[i] = State::Working;
        path.push_back(all[i].name);
        try {
            all[i].value = evaluate(all[i].expression, [&](const std::string& name) -> std::optional<double> {
                const int j = find(sketch, name);
                if (j < 0) {
                    return std::nullopt;
                }
                return valueOf(static_cast<size_t>(j));
            });
        }
        catch (const SketchError& e) {
            const std::string what = e.what();
            // (A loop's own message says where it is; others say which parameter.)
            throw SketchError(what.starts_with("parameters use") ? what : all[i].name + ": " + what);
        }
        path.pop_back();
        state[i] = State::Done;
        return all[i].value;
    };
    for (size_t i = 0; i < all.size(); ++i) {
        valueOf(i);
    }
    sketch.setParameters(all);

    // The dimensions: parameters only.
    auto lookup = [&](const std::string& name) -> std::optional<double> {
        const int j = find(sketch, name);
        return j < 0 ? std::nullopt : std::optional<double>(all[static_cast<size_t>(j)].value);
    };
    std::vector<std::pair<Id, std::string>> driven;
    for (const auto& [id, c] : sketch.constraints()) {
        if (!c.expression.empty()) {
            driven.emplace_back(id, c.expression);
        }
    }
    for (const auto& [id, expression] : driven) {
        try {
            sketch.setConstraintValue(id, evaluate(expression, lookup));
        }
        catch (const SketchError& e) {
            throw SketchError("dimension d" + std::to_string(id) + " (" + expression + "): " + e.what());
        }
    }

    // Patterns' and offsets' settings. (A pattern given a new count is made again, with a new id.)
    std::vector<Id> features;
    for (const auto& [id, c] : sketch.constraints()) {
        if (!c.settings.empty()) {
            features.push_back(id);
        }
    }
    for (Id id : features) {
        const Constraint c = sketch.constraint(id);
        const std::string what = c.type == ConstraintType::Offset ? "offset" : "pattern";
        auto value = [&](const char* key, double& field) {
            const auto it = c.settings.find(key);
            if (it == c.settings.end()) {
                return;
            }
            try {
                field = evaluate(it->second, lookup);
            }
            catch (const SketchError& e) {
                throw SketchError(what + " k" + std::to_string(id) + " (" + key + " = "
                                  + it->second + "): " + e.what());
            }
        };
        auto whole = [&](const char* key, int& field) {
            double v = field;
            value(key, v);
            if (std::abs(v - std::round(v)) > 1e-9) {
                throw SketchError(what + " k" + std::to_string(id) + " (" + key + " = "
                                  + c.settings.at(key) + "): a count has to come out whole, not " + number(v));
            }
            field = static_cast<int>(std::lround(v));
        };
        if (c.type == ConstraintType::LinearPattern) {
            feature::LinearPatternParams p = feature::linearParams(c), was = p;
            whole("count", p.count);
            value("spacing", p.spacing);
            value("angle", p.angle);
            whole("count2", p.count2);
            value("spacing2", p.spacing2);
            if (p.count != was.count || p.spacing != was.spacing || p.angle != was.angle || p.count2 != was.count2
                || p.spacing2 != was.spacing2) {
                feature::setLinearPattern(sketch, id, p);
            }
        }
        else if (c.type == ConstraintType::CircularPattern) {
            feature::CircularPatternParams p = feature::circularParams(c), was = p;
            whole("count", p.count);
            value("angle", p.angle);
            if (p.count != was.count || p.angle != was.angle) {
                feature::setCircularPattern(sketch, id, p);
            }
        }
        else if (c.type == ConstraintType::Offset) {
            double d = c.value;
            value("distance", d);
            if (d != c.value) {
                sketch.setConstraintValue(id, d);
            }
        }
    }
}

double valueOf(const Sketch& sketch, const std::string& expression)
{
    return evaluate(expression, [&](const std::string& name) -> std::optional<double> {
        const int at = find(sketch, name);
        return at < 0 ? std::nullopt : std::optional<double>(sketch.parameters()[static_cast<size_t>(at)].value);
    });
}

namespace
{
void setting(const Sketch& s, const std::map<std::string, std::string>& settings, const char* key, double& field)
{
    if (const auto it = settings.find(key); it != settings.end()) {
        try {
            field = valueOf(s, it->second);
        }
        catch (const SketchError& e) {
            throw SketchError(std::string(key) + " (" + it->second + "): " + e.what());
        }
    }
}
void wholeSetting(const Sketch& s, const std::map<std::string, std::string>& settings, const char* key, int& field)
{
    double v = field;
    setting(s, settings, key, v);
    if (std::abs(v - std::round(v)) > 1e-9) {
        throw SketchError(std::string(key) + " (" + settings.at(key) + "): a count has to come out whole, not " + number(v));
    }
    field = static_cast<int>(std::lround(v));
}
}  // namespace

void applySettings(const Sketch& s, const std::map<std::string, std::string>& settings, feature::LinearPatternParams& p)
{
    wholeSetting(s, settings, "count", p.count);
    setting(s, settings, "spacing", p.spacing);
    setting(s, settings, "angle", p.angle);
    wholeSetting(s, settings, "count2", p.count2);
    setting(s, settings, "spacing2", p.spacing2);
}

void applySettings(const Sketch& s, const std::map<std::string, std::string>& settings, feature::CircularPatternParams& p)
{
    wholeSetting(s, settings, "count", p.count);
    setting(s, settings, "angle", p.angle);
}

Users usersOf(const Sketch& sketch, const std::string& name)
{
    Users out;
    auto uses = [&](const std::string& expression) {
        return std::ranges::any_of(namesIn(expression), [&](const std::string& n) { return sameName(n, name); });
    };
    for (const Parameter& p : sketch.parameters()) {
        if (!sameName(p.name, name) && uses(p.expression)) {
            out.parameters.push_back(p.name);
        }
    }
    for (const auto& [id, c] : sketch.constraints()) {
        const bool setting = std::ranges::any_of(c.settings, [&](const auto& kv) { return uses(kv.second); });
        if ((!c.expression.empty() && uses(c.expression)) || setting) {
            out.dimensions.push_back(id);
        }
    }
    return out;
}

std::string renamed(const std::string& expression, const std::string& from, const std::string& to)
{
    std::string out;
    size_t i = 0;
    const size_t n = expression.size();
    while (i < n) {
        const char c = expression[i];
        if (std::isdigit(static_cast<unsigned char>(c)) || c == '.') {
            // A number, with any exponent (so the e of 1e3 isn't taken for a name).
            const size_t start = i;
            while (i < n && (std::isdigit(static_cast<unsigned char>(expression[i])) || expression[i] == '.')) {
                ++i;
            }
            if (i < n && (expression[i] == 'e' || expression[i] == 'E')) {
                size_t q = i + 1;
                if (q < n && (expression[q] == '+' || expression[q] == '-')) {
                    ++q;
                }
                if (q < n && std::isdigit(static_cast<unsigned char>(expression[q]))) {
                    i = q;
                    while (i < n && std::isdigit(static_cast<unsigned char>(expression[i]))) {
                        ++i;
                    }
                }
            }
            out += expression.substr(start, i - start);
            continue;
        }
        if (nameStart(c)) {
            const size_t start = i;
            while (i < n && namePart(expression[i])) {
                ++i;
            }
            const std::string name = expression.substr(start, i - start);
            size_t after = i;
            while (after < n && std::isspace(static_cast<unsigned char>(expression[after]))) {
                ++after;
            }
            const bool function = after < n && expression[after] == '(';
            out += !function && sameName(name, from) ? to : name;
            continue;
        }
        out += c;
        ++i;
    }
    return out;
}

}  // namespace core::parameters
