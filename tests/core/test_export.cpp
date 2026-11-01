// Exporting drawings.

#include <core/Builders.h>
#include <core/Export.h>
#include <core/Regions.h>

#include <catch2/catch_test_macros.hpp>

#include <string>

using namespace core;

namespace
{
size_t occurrences(const std::string& text, const std::string& what)
{
    size_t n = 0;
    for (size_t at = text.find(what); at != std::string::npos; at = text.find(what, at + 1)) {
        ++n;
    }
    return n;
}

build::Rectangle rect(Sketch& s, double x0, double y0, double x1, double y1)
{
    const Id a = s.addPoint(x0, y0);
    return build::rectangle(s, a, s.addPoint(x1, y1));
}
}  // namespace

TEST_CASE("SVG is in millimetres, the right way up", "[export][svg]")
{
    Sketch s;
    rect(s, 0, 0, 20, 10);
    exporter::SvgOptions o;
    o.margin = 0;
    const std::string svg = exporter::toSvg(s, o);
    // 20 x 10 plus half the default 0.25 mm line all round.
    CHECK(svg.find("width=\"20.25mm\" height=\"10.25mm\" viewBox=\"0 0 20.25 10.25\"") != std::string::npos);
    CHECK(occurrences(svg, "<line ") == 4);
    // The bottom side (y = 0) is at the bottom of the picture.
    CHECK(svg.find("<line x1=\"0.125\" y1=\"10.125\" x2=\"20.125\" y2=\"10.125\"") != std::string::npos);
    CHECK(svg.find("inkscape:label=\"Geometry\"") != std::string::npos);
}

TEST_CASE("SVG strokes keep their colour, width and dashes; arcs and circles stay exact", "[export][svg]")
{
    Sketch s;
    const Id arc = s.addArc(0, 0, 5, 0, 90);
    const Id circle = s.addCircle(20, 0, 3);
    StrokeStyle red;
    Color::fromHex("#ed1c2480", red.color);
    red.width = 0.5;
    red.dash = Dash::Dashed;
    s.setStroke(arc, red);
    const std::string svg = exporter::toSvg(s);
    CHECK(svg.find("A5,5 0 0,0 ") != std::string::npos);  // a quarter, counter-clockwise
    CHECK(svg.find("stroke=\"#ed1c24\" stroke-opacity=\"0.502\" stroke-width=\"0.5\"") != std::string::npos);
    CHECK(svg.find("stroke-dasharray=\"2.5,1.5\"") != std::string::npos);
    CHECK(svg.find("<circle ") != std::string::npos);
    CHECK(svg.find("r=\"3\"") != std::string::npos);
    (void)circle;
}

TEST_CASE("SVG fills keep their holes; construction is left out unless asked for", "[export][svg]")
{
    Sketch s;
    rect(s, 0, 0, 40, 30);
    s.addCircle(20, 15, 5);
    const Id helper = s.addLine(0, 0, 40, 30);
    s.entity(helper).construction = true;
    Fill f;
    Color::fromHex("#ffc90e", f.color);
    f.seed = Point{3, 3};
    s.addFill(f);
    regions::refreshFills(s);

    std::string svg = exporter::toSvg(s);
    CHECK(occurrences(svg, "fill-rule=\"evenodd\"") == 1);
    CHECK(occurrences(svg, " Z") == 2);  // the outline and the hole
    CHECK(svg.find("Construction") == std::string::npos);
    CHECK(occurrences(svg, "<line ") == 4);

    exporter::SvgOptions o;
    o.construction = true;
    o.fills = false;
    o.undecorated = false;
    svg = exporter::toSvg(s, o);
    CHECK(svg.find("Construction") != std::string::npos);
    CHECK(occurrences(svg, "<line ") == 1);
    CHECK(svg.find("fill-rule") == std::string::npos);
}

TEST_CASE("An empty drawing can't be exported", "[export][svg]")
{
    Sketch s;
    s.ensureOrigin();
    CHECK_THROWS_AS(exporter::toSvg(s), SketchError);
}
