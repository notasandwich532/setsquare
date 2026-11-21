// Parameters: named values worked out in order, dimensions following them, names renamed inside
// expressions, loops and unknown names refused, saved and loaded, copied and scaled.

#include <core/Builders.h>
#include <core/Expression.h>
#include <core/Parameters.h>
#include <core/Serialize.h>
#include <core/Transform.h>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

using namespace core;
using Catch::Approx;

namespace
{
std::string failure(const std::function<void()>& f)
{
    try {
        f();
    }
    catch (const SketchError& e) {
        return e.what();
    }
    return "(no error)";
}
}  // namespace

TEST_CASE("Expressions: names looked up, listed, and renamed", "[parameters]")
{
    auto lookup = [](const std::string& n) -> std::optional<double> {
        if (n == "width") return 120.0;
        if (n == "Gap") return 4.0;
        return std::nullopt;
    };
    CHECK(evaluate("width / 2 + Gap", lookup) == Approx(64.0));
    CHECK(evaluate("2width", lookup) == Approx(240.0));               // (juxtaposed, as 2pi)
    CHECK(evaluate("sqrt(width * 0 + 16) mm", lookup) == Approx(4.0));  // functions and units still theirs
    CHECK(failure([&] { evaluate("width + hieght", lookup); }) == "don't know 'hieght'");
    CHECK(failure([] { evaluate("width"); }) == "don't know 'width'");  // (without a lookup, as before)

    CHECK(namesIn("width / 2 + max(gap, 1e3) * pi + 2in") == std::vector<std::string>{"width", "gap"});
    CHECK(namesIn("12.5 + 3") .empty());
    CHECK(isBuiltInName("PI"));
    CHECK(isBuiltInName("sqrt"));
    CHECK(!isBuiltInName("width"));

    CHECK(parameters::renamed("width/2 + Width*widths + max(width, 1e3)", "width", "w") == "w/2 + w*widths + max(w, 1e3)");
    CHECK(parameters::renamed("min(1, 2) + 2e5", "min", "x") == "min(1, 2) + 2e5");  // (a function's not a name)
}

TEST_CASE("Parameters: names that can be, worked out in order, loops and unknowns refused", "[parameters]")
{
    std::string why;
    CHECK(parameters::validName("width_2", &why));
    CHECK(!parameters::validName("2wide", &why));
    CHECK(!parameters::validName("in", &why));
    CHECK(why.find("already means") != std::string::npos);
    CHECK(!parameters::validName("a b", &why));

    Sketch s;
    s.setParameters({{"hole", "width / 10"}, {"width", "120"}, {"Gap", "hole + 1"}});  // (any order)
    parameters::update(s);
    CHECK(s.parameters()[0].value == Approx(12.0));
    CHECK(s.parameters()[2].value == Approx(13.0));
    CHECK(parameters::find(s, "WIDTH") == 1);

    s.setParameters({{"a", "b + 1"}, {"b", "c * 2"}, {"c", "a"}});
    CHECK(failure([&] { parameters::update(s); }).find("loop: a \xE2\x86\x92 b \xE2\x86\x92 c \xE2\x86\x92 a") != std::string::npos);
    s.setParameters({{"a", "nope + 1"}});
    CHECK(failure([&] { parameters::update(s); }) == "a: don't know 'nope'");
}

TEST_CASE("Parameters: dimensions follow them; saved and loaded; copies keep them, scaling doesn't", "[parameters]")
{
    Sketch s;
    const build::Rectangle r = build::rectangle(s, s.addPoint(0, 0), s.addPoint(100, 50));
    const Id w = s.addConstraint(ConstraintType::Distance, {r.sides[0]}, 100);
    s.setParameters({{"width", "120"}});
    s.setConstraintExpression(w, "width / 2");
    parameters::update(s);
    CHECK(s.constraint(w).value == Approx(60.0));
    const parameters::Users users = parameters::usersOf(s, "Width");
    CHECK(users.dimensions == std::vector<Id>{w});
    CHECK(users.parameters.empty());

    s.setParameters({{"width", "-20"}});
    CHECK(failure([&] { parameters::update(s); }).find("d" + std::to_string(w) + " (width / 2)") != std::string::npos);
    s.setParameters({{"width", "200"}});
    parameters::update(s);

    const Sketch loaded = fromJson(toJson(s));
    REQUIRE(loaded.parameters().size() == 1);
    CHECK(loaded.parameters()[0].expression == "200");
    CHECK(loaded.parameters()[0].value == Approx(200.0));  // (worked out on loading)
    CHECK(loaded.constraint(w).expression == "width / 2");
    CHECK(loaded.constraint(w).value == Approx(100.0));

    const std::vector<Id> items(r.sides.begin(), r.sides.end());
    const std::map<Id, Id> copied = xform::copy(s, s, items, Affine::translation(0, 200));
    REQUIRE(copied.contains(w));
    CHECK(s.constraint(copied.at(w)).expression == "width / 2");
    xform::apply(s, items, Affine::scaling({0, 0}, 2.0));
    CHECK(s.constraint(w).expression.empty());  // (scaled: a value of its own now)
    CHECK(s.constraint(w).value == Approx(200.0));
}