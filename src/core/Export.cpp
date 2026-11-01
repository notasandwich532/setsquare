#include "Export.h"

#include "Geometry.h"
#include "Regions.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <numbers>
#include <sstream>
#include <vector>

namespace core::exporter
{

namespace
{
constexpr double kPi = std::numbers::pi;
constexpr double kHairline = 0.05;  // mm: what a 0-width (hairline) stroke is written as

// Up to 4 decimals (0.1 micron), without trailing zeros.
std::string num(double v)
{
    char buf[64];
    std::snprintf(buf, sizeof buf, "%.4f", v);
    std::string s = buf;
    if (s.find('.') != std::string::npos) {
        while (s.back() == '0') {
            s.pop_back();
        }
        if (s.back() == '.') {
            s.pop_back();
        }
    }
    return s == "-0" ? "0" : s;
}

std::string rgb(const Color& c)
{
    char buf[8];
    std::snprintf(buf, sizeof buf, "#%02x%02x%02x", c.r, c.g, c.b);
    return buf;
}

struct Item
{
    Id curve = kNoId;
    StrokeStyle style;
};

// Dash patterns in multiples of the line width, as the canvas draws them.
std::vector<double> dashPattern(Dash dash)
{
    switch (dash) {
        case Dash::Dashed: return {5, 3};
        case Dash::Dotted: return {1, 2};
        case Dash::DashDot: return {8, 3, 1, 3};
        case Dash::DashDotDot: return {8, 3, 1, 3, 1, 3};
        case Dash::Solid: break;
    }
    return {};
}
}  // namespace

std::string toSvg(const Sketch& s, const SvgOptions& o)
{
    // What goes in, by layer.
    std::vector<Item> strokes, plain, construction;
    const StrokeStyle constructionStyle{{0x9a, 0xa0, 0xa6, 255}, 0.18, Dash::Dashed};
    for (const auto& [id, e] : s.entities()) {
        if (e.kind() == EntityKind::Point || s.isReference(id)) {
            continue;
        }
        if (e.construction) {
            if (o.construction) {
                construction.push_back({id, constructionStyle});
            }
        }
        else if (const StrokeStyle* style = s.stroke(id)) {
            strokes.push_back({id, *style});
        }
        else if (o.undecorated) {
            plain.push_back({id, o.plain});
        }
    }
    struct Area
    {
        Color color;
        regions::Region region;
    };
    std::vector<Area> fills;
    if (o.fills && !s.fills().empty()) {
        const regions::Arrangement arrangement(s, kPi / 720);  // quarter-degree steps on arcs
        for (const auto& [id, f] : s.fills()) {
            if (auto region = regions::resolve(arrangement, f)) {
                fills.push_back({f.color, std::move(*region)});
            }
        }
    }
    if (strokes.empty() && plain.empty() && construction.empty() && fills.empty()) {
        throw SketchError("there's nothing to export");
    }

    // The extent of it all (with the lines' thickness), in sketch coordinates.
    double x0 = 1e300, y0 = 1e300, x1 = -1e300, y1 = -1e300;
    auto include = [&](Vec2 p, double pad) {
        x0 = std::min(x0, p.x - pad);
        y0 = std::min(y0, p.y - pad);
        x1 = std::max(x1, p.x + pad);
        y1 = std::max(y1, p.y + pad);
    };
    for (const auto* list : {&strokes, &plain, &construction}) {
        for (const Item& item : *list) {
            const double pad = std::max(item.style.width, kHairline) / 2;
            for (const Vec2& p : sample(s, item.curve, 256)) {
                include(p, pad);
            }
        }
    }
    for (const Area& a : fills) {
        for (const Vec2& p : a.region.outer) {
            include(p, 0.0);
        }
    }
    const double m = std::max(0.0, o.margin);
    const double width = x1 - x0 + 2 * m, height = y1 - y0 + 2 * m;
    // Sketch y points up, SVG's down.
    auto X = [&](double x) { return num(x - x0 + m); };
    auto Y = [&](double y) { return num(y1 - y + m); };

    std::ostringstream out;
    out << "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
        << "<svg xmlns=\"http://www.w3.org/2000/svg\" "
           "xmlns:inkscape=\"http://www.inkscape.org/namespaces/inkscape\" version=\"1.1\"\n"
        << "     width=\"" << num(width) << "mm\" height=\"" << num(height) << "mm\" viewBox=\"0 0 "
        << num(width) << ' ' << num(height) << "\">\n"
        << "  <!-- Drawing App export: 1 unit = 1 mm -->\n";

    auto layer = [&](const char* id, const char* label) {
        out << "  <g id=\"" << id << "\" inkscape:groupmode=\"layer\" inkscape:label=\"" << label << "\">\n";
    };

    if (!fills.empty()) {
        layer("fills", "Fills");
        for (const Area& a : fills) {
            std::string d;
            auto loop = [&](const std::vector<Vec2>& pts) {
                for (size_t i = 0; i < pts.size(); ++i) {
                    d += (i == 0 ? "M" : "L") + X(pts[i].x) + ',' + Y(pts[i].y) + ' ';
                }
                d += "Z ";
            };
            loop(a.region.outer);
            for (const auto& hole : a.region.holes) {
                loop(hole);
            }
            d.pop_back();
            out << "    <path d=\"" << d << "\" fill=\"" << rgb(a.color) << '"';
            if (a.color.a != 255) {
                out << " fill-opacity=\"" << num(a.color.a / 255.0) << '"';
            }
            out << " fill-rule=\"evenodd\" stroke=\"none\"/>\n";
        }
        out << "  </g>\n";
    }

    auto writeCurve = [&](const Item& item) {
        const StrokeStyle& st = item.style;
        const double w = st.width > 0.0 ? st.width : kHairline;
        std::string style = " fill=\"none\" stroke=\"" + rgb(st.color) + '"';
        if (st.color.a != 255) {
            style += " stroke-opacity=\"" + num(st.color.a / 255.0) + '"';
        }
        style += " stroke-width=\"" + num(w) + '"';
        const std::vector<double> dashes = dashPattern(st.dash);
        if (dashes.empty()) {
            style += " stroke-linecap=\"round\" stroke-linejoin=\"round\"";
        }
        else {
            const double unit = std::max(w, 0.25);  // hairline dashes stay visible
            style += " stroke-linecap=\"butt\" stroke-dasharray=\"";
            for (size_t i = 0; i < dashes.size(); ++i) {
                style += (i ? "," : "") + num(dashes[i] * unit);
            }
            style += '"';
        }

        const Entity& e = s.entity(item.curve);
        if (const auto* l = std::get_if<Line>(&e.geom)) {
            const Point& a = s.point(l->p1);
            const Point& b = s.point(l->p2);
            out << "    <line x1=\"" << X(a.x) << "\" y1=\"" << Y(a.y) << "\" x2=\"" << X(b.x) << "\" y2=\""
                << Y(b.y) << '"' << style << "/>\n";
        }
        else if (const auto* c = std::get_if<Circle>(&e.geom)) {
            const Point& centre = s.point(c->center);
            out << "    <circle cx=\"" << X(centre.x) << "\" cy=\"" << Y(centre.y) << "\" r=\"" << num(c->radius)
                << '"' << style << "/>\n";
        }
        else {
            // Counter-clockwise with y up is clockwise with y down: SVG's sweep-flag 0.
            const ArcShape a = arcShape(s, item.curve);
            const double end = a.startAngle + a.sweep;
            out << "    <path d=\"M" << X(a.center.x + a.radius * std::cos(a.startAngle)) << ','
                << Y(a.center.y + a.radius * std::sin(a.startAngle)) << " A" << num(a.radius) << ','
                << num(a.radius) << " 0 " << (a.sweep > kPi ? 1 : 0) << ",0 "
                << X(a.center.x + a.radius * std::cos(end)) << ',' << Y(a.center.y + a.radius * std::sin(end))
                << '"' << style << "/>\n";
        }
    };

    if (!strokes.empty()) {
        layer("strokes", "Strokes");
        for (const Item& item : strokes) {
            writeCurve(item);
        }
        out << "  </g>\n";
    }
    if (!plain.empty()) {
        layer("geometry", "Geometry");
        for (const Item& item : plain) {
            writeCurve(item);
        }
        out << "  </g>\n";
    }
    if (!construction.empty()) {
        layer("construction", "Construction");
        for (const Item& item : construction) {
            writeCurve(item);
        }
        out << "  </g>\n";
    }
    out << "</svg>\n";
    return out.str();
}

}  // namespace core::exporter
