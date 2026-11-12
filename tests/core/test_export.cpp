// Exporting drawings.

#include <core/Builders.h>
#include <core/Export.h>
#include <core/Regions.h>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <map>
#include <string>
#include <vector>

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
    // Its layer an Inkscape layer; its sublayer a group in it, by name.
    CHECK(svg.find("inkscape:groupmode=\"layer\" inkscape:label=\"" + s.layers().front().name + '"') != std::string::npos);
    CHECK(svg.find("inkscape:label=\"" + s.sublayers().front().name + '"') != std::string::npos);
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

namespace
{
// A DXF file as its (group code, value) pairs; fails the test if it isn't pairs of lines.
std::vector<std::pair<int, std::string>> dxfPairs(const std::string& text)
{
    std::vector<std::string> lines;
    size_t start = 0;
    while (start < text.size()) {
        const size_t end = text.find("\r\n", start);
        REQUIRE(end != std::string::npos);  // every line ends CRLF
        lines.push_back(text.substr(start, end - start));
        start = end + 2;
    }
    REQUIRE(lines.size() % 2 == 0);
    std::vector<std::pair<int, std::string>> pairs;
    for (size_t i = 0; i < lines.size(); i += 2) {
        pairs.push_back({std::stoi(lines[i]), lines[i + 1]});
    }
    return pairs;
}

// The values of the group codes of the n-th entity of a type.
std::map<int, std::string> dxfEntity(const std::vector<std::pair<int, std::string>>& pairs, const std::string& type,
                                     size_t n = 0)
{
    for (size_t i = 0; i < pairs.size(); ++i) {
        if (pairs[i].first == 0 && pairs[i].second == type && n-- == 0) {
            std::map<int, std::string> out;
            for (size_t j = i + 1; j < pairs.size() && pairs[j].first != 0; ++j) {
                out[pairs[j].first] = pairs[j].second;
            }
            return out;
        }
    }
    return {};
}
}  // namespace

TEST_CASE("DXF holds the exact geometry in millimetres", "[export][dxf]")
{
    Sketch s;
    rect(s, 0, 0, 20, 10);
    s.addCircle(30, 5, 4);
    s.addArc(50, 0, 6, 30, 120);
    const auto pairs = dxfPairs(exporter::toDxf(s));
    CHECK(pairs.front() == std::pair<int, std::string>{0, "SECTION"});
    CHECK(pairs.back() == std::pair<int, std::string>{0, "EOF"});
    CHECK(std::ranges::find(pairs, std::pair<int, std::string>{1, "AC1018"}) != pairs.end());
    CHECK(std::ranges::count(pairs, std::pair<int, std::string>{0, "LINE"}) == 4);

    const auto circle = dxfEntity(pairs, "CIRCLE");
    CHECK(circle.at(10) == "30");
    CHECK(circle.at(40) == "4");
    CHECK(circle.at(8) == "GEOMETRY");
    const auto arc = dxfEntity(pairs, "ARC");
    CHECK(arc.at(10) == "50");
    CHECK(arc.at(40) == "6");
    CHECK(arc.at(50) == "30");   // counter-clockwise from 30 degrees
    CHECK(arc.at(51) == "120");  // to 120
}

TEST_CASE("DXF strokes keep a colour and line style; construction is a switched-off layer", "[export][dxf]")
{
    Sketch s;
    const Id l = s.addLine(0, 0, 10, 0);
    const Id helper = s.addLine(0, 0, 0, 10);
    s.entity(helper).construction = true;
    StrokeStyle style;
    Color::fromHex("#ff0000", style.color);
    style.dash = Dash::DashDot;
    s.setStroke(l, style);

    exporter::DxfOptions o;
    o.construction = true;
    const auto pairs = dxfPairs(exporter::toDxf(s, o));
    const auto stroked = dxfEntity(pairs, "LINE", 0);
    CHECK(stroked.at(8) == "STROKES");
    CHECK(stroked.at(62) == "1");  // red
    CHECK(stroked.at(6) == "DASHDOT");
    CHECK(dxfEntity(pairs, "LINE", 1).at(8) == "CONSTRUCTION");
    // The CONSTRUCTION layer is off (negative colour).
    bool found = false;
    for (size_t n = 0; n < 10; ++n) {
        const auto layer = dxfEntity(pairs, "LAYER", n);
        if (layer.contains(2) && layer.at(2) == "CONSTRUCTION") {
            CHECK(layer.at(62) == "-8");
            found = true;
        }
    }
    CHECK(found);
    CHECK(exporter::nearestAci({0, 0, 255, 255}) == 5);
    CHECK(exporter::nearestAci({0, 0, 0, 255}) == 7);  // black shows as 7 (black on white)
    CHECK(exporter::nearestAci({255, 128, 0, 255}) != 7);

    o.construction = false;
    o.styles = false;
    const auto plain = dxfPairs(exporter::toDxf(s, o));
    CHECK(std::ranges::count(plain, std::pair<int, std::string>{0, "LINE"}) == 1);
    CHECK(dxfEntity(plain, "LINE").at(8) == "GEOMETRY");
}

TEST_CASE("SVG fills are outlined by the curves they're in, exactly; a gradient used twice is written once", "[export][svg]")
{
    Sketch s;
    // A disc with a square hole, and a slot-like area of lines and arcs.
    const Id disc = s.addCircle(0, 0, 10);
    build::Rectangle hole = build::rectangle(s, s.addPoint(-3, -3), s.addPoint(3, 3));
    Fill f;
    f.color = {255, 0, 0, 255};
    f.seed = {7, 0};
    Gradient g;
    g.kind = GradientKind::Linear;
    g.start = s.addPoint(-10, 0);
    g.end = s.addPoint(10, 0);
    g.stops = {{0, {255, 0, 0, 255}}, {1, {0, 0, 255, 255}}};
    f.gradient = g;
    s.addFill(f);
    StrokeStyle st;
    st.width = 0.5;
    st.gradient = g;
    s.setStroke(disc, st);
    regions::refreshFills(s);

    const std::string svg = exporter::toSvg(s, {});
    const size_t fills = svg.find("-fills\"");
    REQUIRE(fills != std::string::npos);
    const size_t d = svg.find(" d=\"", fills);
    const std::string path = svg.substr(d, svg.find('"', d + 4) - d);
    auto count = [&](char c) { return std::count(path.begin(), path.end(), c); };
    CHECK(count('A') == 3);  // the circle, in thirds
    CHECK(count('L') == 4);  // the square's sides
    CHECK(count('Z') == 2);
    (void)hole;
    // The gradient (the fill's and the outline's): one definition.
    CHECK(svg.find("<linearGradient") != std::string::npos);
    CHECK(svg.find("<linearGradient", svg.find("<linearGradient") + 1) == std::string::npos);
}
