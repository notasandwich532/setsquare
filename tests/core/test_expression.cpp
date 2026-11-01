// Arithmetic typed into number inputs.

#include <core/Expression.h>
#include <core/Sketch.h>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <numbers>
#include <string>

using core::evaluate;
using Catch::Approx;

TEST_CASE("Numbers and sums, the usual way round", "[expression]")
{
    CHECK(evaluate("42") == 42.0);
    CHECK(evaluate(" 2.5 ") == 2.5);
    CHECK(evaluate(".5") == 0.5);
    CHECK(evaluate("1e3") == 1000.0);
    CHECK(evaluate("2.5E-1") == 0.25);
    CHECK(evaluate("20/3") == Approx(20.0 / 3.0));
    CHECK(evaluate("(100 - 2*15) / 2") == 35.0);
    CHECK(evaluate("2 + 3 * 4") == 14.0);
    CHECK(evaluate("2 - 3 - 4") == -5.0);       // left to right
    CHECK(evaluate("2^3^2") == 512.0);          // power: right to left
    CHECK(evaluate("-2^2") == -4.0);            // minus applies after the power
    CHECK(evaluate("2^-1") == 0.5);
    CHECK(evaluate("--3") == 3.0);
    CHECK(evaluate("7 % 3") == 1.0);
    CHECK(evaluate("((1))") == 1.0);
}

TEST_CASE("Constants, units, functions; trig in degrees", "[expression]")
{
    CHECK(evaluate("pi") == Approx(std::numbers::pi));
    CHECK(evaluate("2pi") == Approx(2 * std::numbers::pi));  // side by side multiplies
    CHECK(evaluate("3(4 + 1)") == 15.0);
    CHECK(evaluate("(1 + 1)(2 + 3)") == 10.0);
    CHECK(evaluate("10 cm") == 100.0);
    CHECK(evaluate("1in + 3mm") == Approx(28.4));
    CHECK(evaluate("90°") == 90.0);
    CHECK(evaluate("45 deg") == 45.0);
    CHECK(evaluate("sqrt(2) * 10") == Approx(std::sqrt(2.0) * 10));
    CHECK(evaluate("sin(30)") == Approx(0.5));
    CHECK(evaluate("cos(60)") == Approx(0.5));
    CHECK(evaluate("atan(1)") == Approx(45.0));
    CHECK(evaluate("atan2(1, 1)") == Approx(45.0));
    CHECK(evaluate("hypot(3, 4)") == 5.0);
    CHECK(evaluate("max(1, 7, 3) - min(4, 2)") == 5.0);
    CHECK(evaluate("round(2.6) + floor(1.9) + ceil(0.1)") == 5.0);
    CHECK(evaluate("log(1000) + ln(e)") == Approx(4.0));
    CHECK(evaluate("SQRT(16)") == 4.0);  // any case
}

TEST_CASE("What doesn't read says why", "[expression]")
{
    auto why = [](const std::string& text) {
        std::string error;
        double v = 123.0;
        CHECK_FALSE(core::tryEvaluate(text, v, &error));
        CHECK(v == 123.0);  // untouched
        return error;
    };
    CHECK(why("").find("type a number") != std::string::npos);
    CHECK(why("2+").find("missing at the end") != std::string::npos);
    CHECK(why("(2+3").find("bracket") != std::string::npos);
    CHECK(why("2)").find("')'") != std::string::npos);
    CHECK(why("4/0").find("divide by zero") != std::string::npos);
    CHECK(why("sqrt(-1)").find("number") != std::string::npos);
    CHECK(why("width*2").find("'width'") != std::string::npos);
    CHECK(why("hypot(3)").find("takes 2") != std::string::npos);
    CHECK(why("1..2").find("isn't a number") != std::string::npos);
    CHECK_THROWS_AS(evaluate("2 $ 3"), core::SketchError);
}
