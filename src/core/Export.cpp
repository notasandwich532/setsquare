#include "Export.h"

#include "Geometry.h"
#include "Regions.h"

#include <algorithm>
#include <array>
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
    // Texts: their letters in world coordinates, filled.
    struct TextArea
    {
        Color color;
        std::vector<std::vector<Vec2>> contours;
    };
    std::vector<TextArea> texts;
    for (const auto& [id, t] : s.texts()) {
        if (t.shape.empty() || !s.hasEntity(t.anchor)) {
            continue;
        }
        TextArea area{t.hasColor ? t.color : o.plain.color, {}};
        for (const auto& contour : t.shape) {
            std::vector<Vec2> pts;
            for (const Point& p : contour) {
                const Point w = textToWorld(s.point(t.anchor), t.angle, p);
                pts.push_back({w.x, w.y});
            }
            area.contours.push_back(std::move(pts));
        }
        texts.push_back(std::move(area));
    }
    if (strokes.empty() && plain.empty() && construction.empty() && fills.empty() && texts.empty()) {
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
    for (const TextArea& t : texts) {
        for (const auto& contour : t.contours) {
            for (const Vec2& p : contour) {
                include(p, 0.0);
            }
        }
    }
    double m = std::max(0.0, o.margin);
    if (o.canvas) {
        // A fixed frame, in its own units.
        m = 0.0;
        x0 = (*o.canvas)[0];
        y0 = (*o.canvas)[1];
        x1 = (*o.canvas)[2];
        y1 = (*o.canvas)[3];
    }
    const double width = x1 - x0 + 2 * m, height = y1 - y0 + 2 * m;
    // Sketch y points up, SVG's down.
    auto X = [&](double x) { return num(x - x0 + m); };
    auto Y = [&](double y) { return num(y1 - y + m); };

    std::ostringstream out;
    const std::string unit = o.canvas ? "" : "mm";
    out << "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
        << "<svg xmlns=\"http://www.w3.org/2000/svg\" "
           "xmlns:inkscape=\"http://www.inkscape.org/namespaces/inkscape\" version=\"1.1\"\n"
        << "     width=\"" << num(width) << unit << "\" height=\"" << num(height) << unit << "\" viewBox=\"0 0 "
        << num(width) << ' ' << num(height) << "\">\n"
        << (o.canvas ? "  <!-- Drawing App icon -->\n" : "  <!-- Drawing App export: 1 unit = 1 mm -->\n");

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

    if (!texts.empty()) {
        layer("text", "Text");
        for (const TextArea& t : texts) {
            std::string d;
            for (const auto& contour : t.contours) {
                for (size_t i = 0; i < contour.size(); ++i) {
                    d += (i == 0 ? "M" : "L") + X(contour[i].x) + ',' + Y(contour[i].y) + ' ';
                }
                d += "Z ";
            }
            if (d.empty()) {
                continue;
            }
            d.pop_back();
            out << "    <path d=\"" << d << "\" fill=\"" << rgb(t.color) << '"';
            if (t.color.a != 255) {
                out << " fill-opacity=\"" << num(t.color.a / 255.0) << '"';
            }
            out << " fill-rule=\"nonzero\" stroke=\"none\"/>\n";
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

// ---------------------------------------------------------------------------------------------
// DXF
// ---------------------------------------------------------------------------------------------

namespace
{
// The AutoCAD colour index palette (index -> RGB).
std::array<Color, 256> aciPalette()
{
    std::array<Color, 256> p{};
    const Color basic[10] = {{0, 0, 0, 255},       {255, 0, 0, 255},     {255, 255, 0, 255}, {0, 255, 0, 255},
                             {0, 255, 255, 255},   {0, 0, 255, 255},     {255, 0, 255, 255}, {255, 255, 255, 255},
                             {128, 128, 128, 255}, {192, 192, 192, 255}};
    for (int i = 0; i < 10; ++i) {
        p[static_cast<size_t>(i)] = basic[i];
    }
    // 10-249: 24 hues in 15 degree steps; in each, five brightnesses, full then half saturation.
    const double values[5] = {1.0, 0.8, 0.6, 0.5, 0.3};
    for (int i = 10; i < 250; ++i) {
        const double h = (i / 10 - 1) * 15.0;
        const double v = values[(i % 10) / 2];
        const double sat = i % 2 == 0 ? 1.0 : 0.5;
        const double c = v * sat;
        const double x = c * (1 - std::abs(std::fmod(h / 60.0, 2.0) - 1));
        const double m = v - c;
        double r = 0, g = 0, b = 0;
        if (h < 60) {
            r = c, g = x;
        }
        else if (h < 120) {
            r = x, g = c;
        }
        else if (h < 180) {
            g = c, b = x;
        }
        else if (h < 240) {
            g = x, b = c;
        }
        else if (h < 300) {
            r = x, b = c;
        }
        else {
            r = c, b = x;
        }
        auto byte = [](double u) { return static_cast<std::uint8_t>(std::lround(u * 255)); };
        p[static_cast<size_t>(i)] = Color{byte(r + m), byte(g + m), byte(b + m), 255};
    }
    const std::uint8_t greys[6] = {51, 80, 105, 130, 190, 255};
    for (int i = 0; i < 6; ++i) {
        p[static_cast<size_t>(250 + i)] = Color{greys[i], greys[i], greys[i], 255};
    }
    return p;
}

// R12 linetypes for the dash styles (pattern lengths in mm, for lines around 0.5 mm wide).
struct LineType
{
    const char* name;
    const char* description;
    std::vector<double> pattern;  // dashes positive, gaps negative
};
const LineType kLineTypes[] = {
    {"CONTINUOUS", "Solid line", {}},
    {"DASHED", "Dashed __ __ __", {2.5, -1.5}},
    {"DOT", "Dotted . . . .", {0.0, -1.0}},
    {"DASHDOT", "Dash dot __ . __ .", {4.0, -1.5, 0.0, -1.5}},
    {"DIVIDE", "Dash dot dot __ . . __", {4.0, -1.5, 0.0, -1.5, 0.0, -1.5}},
};

const char* lineTypeFor(Dash dash)
{
    switch (dash) {
        case Dash::Solid: return "CONTINUOUS";
        case Dash::Dashed: return "DASHED";
        case Dash::Dotted: return "DOT";
        case Dash::DashDot: return "DASHDOT";
        case Dash::DashDotDot: return "DIVIDE";
    }
    return "CONTINUOUS";
}

// DXF is lines of group code, then value.
class DxfWriter
{
public:
    void pair(int code, const std::string& value) { m_out << code << "\r\n" << value << "\r\n"; }
    void pair(int code, int value) { pair(code, std::to_string(value)); }
    void real(int code, double value) { pair(code, num(value)); }
    std::string str() const { return m_out.str(); }

private:
    std::ostringstream m_out;
};
}  // namespace

int nearestAci(const Color& c)
{
    // Black and white both come out as 7, which CAD shows contrasting with its background.
    const int hi = std::max({c.r, c.g, c.b}), lo = std::min({c.r, c.g, c.b});
    if (hi < 40 || lo > 230) {
        return 7;
    }
    static const std::array<Color, 256> palette = aciPalette();
    int best = 1;
    long bestDistance = -1;
    for (int i = 1; i < 256; ++i) {
        if (i == 7) {
            continue;
        }
        const Color& p = palette[static_cast<size_t>(i)];
        const long dr = c.r - p.r, dg = c.g - p.g, db = c.b - p.b;
        const long d = dr * dr + dg * dg + db * db;
        if (bestDistance < 0 || d < bestDistance) {
            bestDistance = d;
            best = i;
        }
    }
    return best;
}

std::string toDxf(const Sketch& s, const DxfOptions& o)
{
    struct Item
    {
        Id curve = kNoId;
        const char* layer = "GEOMETRY";
        const char* lineType = nullptr;  // null: the layer's
        int color = 256;                 // 256: the layer's
    };
    std::vector<Item> items;
    for (const auto& [id, e] : s.entities()) {
        if (e.kind() == EntityKind::Point || s.isReference(id)) {
            continue;
        }
        if (e.construction) {
            if (o.construction) {
                items.push_back({id, "CONSTRUCTION"});
            }
        }
        else if (const StrokeStyle* style = s.stroke(id); style && o.styles) {
            items.push_back({id, "STROKES", lineTypeFor(style->dash), nearestAci(style->color)});
        }
        else {
            items.push_back({id, "GEOMETRY"});
        }
    }
    // Texts: each contour of their letters as a closed polyline.
    struct TextOutline
    {
        int color;
        std::vector<Point> points;
    };
    std::vector<TextOutline> textOutlines;
    for (const auto& [id, t] : s.texts()) {
        if (!s.hasEntity(t.anchor)) {
            continue;
        }
        const int color = t.hasColor && o.styles ? nearestAci(t.color) : 256;
        for (const auto& contour : t.shape) {
            TextOutline out{color, {}};
            for (const Point& p : contour) {
                out.points.push_back(textToWorld(s.point(t.anchor), t.angle, p));
            }
            if (out.points.size() >= 3) {
                textOutlines.push_back(std::move(out));
            }
        }
    }
    if (items.empty() && textOutlines.empty()) {
        throw SketchError("there's nothing to export");
    }

    double x0 = 1e300, y0 = 1e300, x1 = -1e300, y1 = -1e300;
    for (const Item& item : items) {
        for (const Vec2& p : sample(s, item.curve, 256)) {
            x0 = std::min(x0, p.x);
            y0 = std::min(y0, p.y);
            x1 = std::max(x1, p.x);
            y1 = std::max(y1, p.y);
        }
    }
    for (const TextOutline& t : textOutlines) {
        for (const Point& p : t.points) {
            x0 = std::min(x0, p.x);
            y0 = std::min(y0, p.y);
            x1 = std::max(x1, p.x);
            y1 = std::max(y1, p.y);
        }
    }

    DxfWriter w;
    // Header: R12, millimetres.
    w.pair(0, "SECTION");
    w.pair(2, "HEADER");
    w.pair(9, "$ACADVER");
    w.pair(1, "AC1009");
    w.pair(9, "$INSUNITS");
    w.pair(70, 4);  // millimetres
    w.pair(9, "$MEASUREMENT");
    w.pair(70, 1);  // metric
    w.pair(9, "$EXTMIN");
    w.real(10, x0);
    w.real(20, y0);
    w.real(30, 0.0);
    w.pair(9, "$EXTMAX");
    w.real(10, x1);
    w.real(20, y1);
    w.real(30, 0.0);
    w.pair(9, "$LTSCALE");
    w.real(40, 1.0);
    w.pair(0, "ENDSEC");

    // Tables: linetypes, then layers.
    w.pair(0, "SECTION");
    w.pair(2, "TABLES");
    w.pair(0, "TABLE");
    w.pair(2, "LTYPE");
    w.pair(70, static_cast<int>(std::size(kLineTypes)));
    for (const LineType& lt : kLineTypes) {
        double total = 0.0;
        for (double d : lt.pattern) {
            total += std::abs(d);
        }
        w.pair(0, "LTYPE");
        w.pair(2, lt.name);
        w.pair(70, 0);
        w.pair(3, lt.description);
        w.pair(72, 65);  // 'A'
        w.pair(73, static_cast<int>(lt.pattern.size()));
        w.real(40, total);
        for (double d : lt.pattern) {
            w.real(49, d);
        }
    }
    w.pair(0, "ENDTAB");
    struct Layer
    {
        const char* name;
        int color;  // negative: switched off
        const char* lineType;
    };
    const Layer layers[] = {{"0", 7, "CONTINUOUS"},
                            {"GEOMETRY", 7, "CONTINUOUS"},
                            {"STROKES", 7, "CONTINUOUS"},
                            {"CONSTRUCTION", -8, "DASHED"},
                            {"TEXT", 7, "CONTINUOUS"}};
    w.pair(0, "TABLE");
    w.pair(2, "LAYER");
    w.pair(70, static_cast<int>(std::size(layers)));
    for (const Layer& l : layers) {
        w.pair(0, "LAYER");
        w.pair(2, l.name);
        w.pair(70, 0);
        w.pair(62, l.color);
        w.pair(6, l.lineType);
    }
    w.pair(0, "ENDTAB");
    w.pair(0, "ENDSEC");

    // The geometry, exactly: DXF is y up and arcs run counter-clockwise, as here.
    w.pair(0, "SECTION");
    w.pair(2, "ENTITIES");
    for (const Item& item : items) {
        const Entity& e = s.entity(item.curve);
        auto common = [&](const char* type) {
            w.pair(0, type);
            w.pair(8, item.layer);
            if (item.lineType) {
                w.pair(6, item.lineType);
            }
            if (item.color != 256) {
                w.pair(62, item.color);
            }
        };
        if (const auto* l = std::get_if<Line>(&e.geom)) {
            const Point& a = s.point(l->p1);
            const Point& b = s.point(l->p2);
            common("LINE");
            w.real(10, a.x);
            w.real(20, a.y);
            w.real(30, 0.0);
            w.real(11, b.x);
            w.real(21, b.y);
            w.real(31, 0.0);
        }
        else if (const auto* c = std::get_if<Circle>(&e.geom)) {
            const Point& centre = s.point(c->center);
            common("CIRCLE");
            w.real(10, centre.x);
            w.real(20, centre.y);
            w.real(30, 0.0);
            w.real(40, c->radius);
        }
        else {
            const ArcShape a = arcShape(s, item.curve);
            auto degrees = [](double r) {
                double d = std::fmod(r * 180.0 / kPi, 360.0);
                return d < 0 ? d + 360.0 : d;
            };
            common("ARC");
            w.real(10, a.center.x);
            w.real(20, a.center.y);
            w.real(30, 0.0);
            w.real(40, a.radius);
            w.real(50, degrees(a.startAngle));
            w.real(51, degrees(a.startAngle + a.sweep));
        }
    }
    for (const TextOutline& t : textOutlines) {
        w.pair(0, "POLYLINE");
        w.pair(8, "TEXT");
        if (t.color != 256) {
            w.pair(62, t.color);
        }
        w.pair(66, 1);  // vertices follow
        w.real(10, 0.0);
        w.real(20, 0.0);
        w.real(30, 0.0);
        w.pair(70, 1);  // closed
        for (const Point& p : t.points) {
            w.pair(0, "VERTEX");
            w.pair(8, "TEXT");
            w.real(10, p.x);
            w.real(20, p.y);
            w.real(30, 0.0);
        }
        w.pair(0, "SEQEND");
        w.pair(8, "TEXT");
    }
    w.pair(0, "ENDSEC");
    w.pair(0, "EOF");
    return w.str();
}

}  // namespace core::exporter
