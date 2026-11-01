#include "Expression.h"

#include "Sketch.h"  // SketchError

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <functional>
#include <numbers>
#include <string_view>
#include <vector>

namespace core
{

namespace
{
constexpr double kDeg = std::numbers::pi / 180.0;

// Recursive descent over the text:
//   sum     := product (('+' | '-') product)*
//   product := unary (('*' | '/' | '%') unary | <juxtaposed> power)*
//   unary   := ('+' | '-') unary | power
//   power   := atom ('^' unary)?
//   atom    := number | name | name '(' sum (',' sum)* ')' | '(' sum ')'
class Parser
{
public:
    explicit Parser(std::string_view text)
        : m_text(text)
    {}

    double parse()
    {
        skipSpace();
        if (atEnd()) {
            fail("type a number or a sum");
        }
        const double v = sum();
        skipSpace();
        if (!atEnd()) {
            fail(std::string("didn't expect '") + m_text[m_pos] + "'");
        }
        return v;
    }

private:
    [[noreturn]] void fail(const std::string& why) const { throw SketchError(why); }

    bool atEnd() const { return m_pos >= m_text.size(); }
    char peek() const { return atEnd() ? '\0' : m_text[m_pos]; }
    void skipSpace()
    {
        while (!atEnd() && std::isspace(static_cast<unsigned char>(m_text[m_pos]))) {
            ++m_pos;
        }
    }
    bool accept(char c)
    {
        skipSpace();
        if (peek() == c) {
            ++m_pos;
            return true;
        }
        return false;
    }
    bool atDegreeSign() const { return m_text.substr(m_pos).starts_with("\xC2\xB0"); }  // UTF-8 °
    // Something that can follow a value to multiply it: a bracket, a name or a number.
    bool atJuxtaposed()
    {
        skipSpace();
        const char c = peek();
        return c == '(' || std::isalpha(static_cast<unsigned char>(c)) || std::isdigit(static_cast<unsigned char>(c))
            || c == '.' || atDegreeSign();
    }

    double sum()
    {
        double v = product();
        while (true) {
            if (accept('+')) {
                v += product();
            }
            else if (accept('-')) {
                v -= product();
            }
            else {
                return v;
            }
        }
    }

    double product()
    {
        double v = unary();
        while (true) {
            if (accept('*')) {
                v *= unary();
            }
            else if (accept('/')) {
                const double d = unary();
                if (d == 0.0) {
                    fail("can't divide by zero");
                }
                v /= d;
            }
            else if (accept('%')) {
                const double d = unary();
                if (d == 0.0) {
                    fail("can't divide by zero");
                }
                v = std::fmod(v, d);
            }
            else if (atJuxtaposed()) {
                v *= power();  // 2pi, 3(1 + 2), 10 cm
            }
            else {
                return v;
            }
        }
    }

    double unary()
    {
        if (accept('-')) {
            return -unary();
        }
        if (accept('+')) {
            return unary();
        }
        return power();
    }

    double power()
    {
        const double base = atom();
        if (accept('^')) {
            return std::pow(base, unary());
        }
        return base;
    }

    double atom()
    {
        skipSpace();
        if (accept('(')) {
            const double v = sum();
            if (!accept(')')) {
                fail("a bracket isn't closed");
            }
            return v;
        }
        if (atDegreeSign()) {
            m_pos += 2;
            return 1.0;
        }
        const char c = peek();
        if (std::isdigit(static_cast<unsigned char>(c)) || c == '.') {
            return number();
        }
        if (std::isalpha(static_cast<unsigned char>(c))) {
            return named();
        }
        if (atEnd()) {
            fail("it stops short: a number is missing at the end");
        }
        fail(std::string("didn't expect '") + c + "'");
    }

    double number()
    {
        const size_t start = m_pos;
        while (std::isdigit(static_cast<unsigned char>(peek())) || peek() == '.') {
            ++m_pos;
        }
        // An exponent (1e3, 2.5E-2), but not the constant e on its own (2e = 2 * e).
        if ((peek() == 'e' || peek() == 'E') && m_pos + 1 < m_text.size()) {
            size_t q = m_pos + 1;
            if (m_text[q] == '+' || m_text[q] == '-') {
                ++q;
            }
            if (q < m_text.size() && std::isdigit(static_cast<unsigned char>(m_text[q]))) {
                m_pos = q;
                while (std::isdigit(static_cast<unsigned char>(peek()))) {
                    ++m_pos;
                }
            }
        }
        const std::string digits(m_text.substr(start, m_pos - start));
        char* end = nullptr;
        const double v = std::strtod(digits.c_str(), &end);
        if (end != digits.c_str() + digits.size()) {
            fail("'" + digits + "' isn't a number");
        }
        return v;
    }

    double named()
    {
        const size_t start = m_pos;
        while (std::isalnum(static_cast<unsigned char>(peek())) || peek() == '_') {
            ++m_pos;
        }
        std::string name(m_text.substr(start, m_pos - start));
        std::ranges::transform(name, name.begin(), [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });

        struct Constant
        {
            const char* name;
            double value;
        };
        static const Constant constants[] = {
            {"pi", std::numbers::pi}, {"tau", 2 * std::numbers::pi}, {"e", std::numbers::e},
            {"mm", 1.0}, {"cm", 10.0}, {"m", 1000.0}, {"in", 25.4}, {"ft", 304.8}, {"deg", 1.0},
        };
        for (const Constant& k : constants) {
            if (name == k.name) {
                return k.value;
            }
        }

        skipSpace();
        if (peek() != '(') {
            fail("don't know '" + name + "'");
        }
        ++m_pos;
        std::vector<double> args;
        skipSpace();
        if (peek() != ')') {
            args.push_back(sum());
            while (accept(',')) {
                args.push_back(sum());
            }
        }
        if (!accept(')')) {
            fail("a bracket isn't closed");
        }
        return call(name, args);
    }

    double call(const std::string& name, const std::vector<double>& a)
    {
        auto need = [&](size_t n) {
            if (a.size() != n) {
                fail(name + " takes " + std::to_string(n) + (n == 1 ? " number" : " numbers"));
            }
        };
        using F = std::function<double(double)>;
        static const std::pair<const char*, F> one[] = {
            {"sqrt", [](double x) { return std::sqrt(x); }},
            {"abs", [](double x) { return std::abs(x); }},
            {"round", [](double x) { return std::round(x); }},
            {"floor", [](double x) { return std::floor(x); }},
            {"ceil", [](double x) { return std::ceil(x); }},
            {"exp", [](double x) { return std::exp(x); }},
            {"ln", [](double x) { return std::log(x); }},
            {"log", [](double x) { return std::log10(x); }},
            {"sin", [](double x) { return std::sin(x * kDeg); }},
            {"cos", [](double x) { return std::cos(x * kDeg); }},
            {"tan", [](double x) { return std::tan(x * kDeg); }},
            {"asin", [](double x) { return std::asin(x) / kDeg; }},
            {"acos", [](double x) { return std::acos(x) / kDeg; }},
            {"atan", [](double x) { return std::atan(x) / kDeg; }},
        };
        for (const auto& [n, f] : one) {
            if (name == n) {
                need(1);
                return f(a[0]);
            }
        }
        if (name == "atan2") {
            need(2);
            return std::atan2(a[0], a[1]) / kDeg;
        }
        if (name == "hypot") {
            need(2);
            return std::hypot(a[0], a[1]);
        }
        if (name == "min" || name == "max") {
            if (a.empty()) {
                fail(name + " needs at least one number");
            }
            return name == "min" ? *std::ranges::min_element(a) : *std::ranges::max_element(a);
        }
        fail("don't know '" + name + "'");
    }

    std::string_view m_text;
    size_t m_pos = 0;
};
}  // namespace

double evaluate(const std::string& text)
{
    const double v = Parser(text).parse();
    if (!std::isfinite(v)) {
        throw SketchError("that doesn't come out as a number");
    }
    return v;
}

bool tryEvaluate(const std::string& text, double& value, std::string* error)
{
    try {
        value = evaluate(text);
        return true;
    }
    catch (const SketchError& e) {
        if (error) {
            *error = e.what();
        }
        return false;
    }
}

}  // namespace core
