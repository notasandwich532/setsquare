#include "Serialize.h"

#include <algorithm>
#include <charconv>
#include <nlohmann/json.hpp>

#include <string>

namespace core
{

using nlohmann::json;

namespace
{
constexpr const char* kFormat = "setsquare.sketch";
constexpr const char* kOldFormat = "drawing_app.sketch";  // (files saved before the app was named Setsquare)

// {"type": "linear", "spread": "pad", "stops": [[0, "#ff0000"], [1, "#0000ff80"]], "start": id, "end": id}
// or {"type": "radial", ..., "shape": id, "focal": id (optional)}: the sketch geometry placing it.
const char kBase64[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

std::string toBase64(const std::string& bytes)
{
    std::string out;
    out.reserve((bytes.size() + 2) / 3 * 4);
    size_t i = 0;
    for (; i + 2 < bytes.size(); i += 3) {
        const unsigned n = (static_cast<unsigned char>(bytes[i]) << 16) | (static_cast<unsigned char>(bytes[i + 1]) << 8)
                           | static_cast<unsigned char>(bytes[i + 2]);
        out += kBase64[(n >> 18) & 63];
        out += kBase64[(n >> 12) & 63];
        out += kBase64[(n >> 6) & 63];
        out += kBase64[n & 63];
    }
    if (i < bytes.size()) {
        unsigned n = static_cast<unsigned char>(bytes[i]) << 16;
        if (i + 1 < bytes.size()) {
            n |= static_cast<unsigned char>(bytes[i + 1]) << 8;
        }
        out += kBase64[(n >> 18) & 63];
        out += kBase64[(n >> 12) & 63];
        out += i + 1 < bytes.size() ? kBase64[(n >> 6) & 63] : '=';
        out += '=';
    }
    return out;
}

std::string fromBase64(const std::string& text)
{
    int value[256];
    std::fill(std::begin(value), std::end(value), -1);
    for (int i = 0; i < 64; ++i) {
        value[static_cast<unsigned char>(kBase64[i])] = i;
    }
    std::string out;
    out.reserve(text.size() / 4 * 3);
    unsigned n = 0;
    int bits = 0;
    for (char c : text) {
        const int v = value[static_cast<unsigned char>(c)];
        if (v < 0) {
            continue;  // padding, line breaks
        }
        n = (n << 6) | static_cast<unsigned>(v);
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            out += static_cast<char>((n >> bits) & 0xff);
        }
    }
    return out;
}

json gradientToJson(const Gradient& g)
{
    json stops = json::array();
    for (const GradientStop& s : g.stops) {
        stops.push_back({s.offset, s.color.hex()});
    }
    json j{{"type", toString(g.kind)}, {"spread", toString(g.spread)}, {"stops", std::move(stops)}};
    if (g.kind == GradientKind::Linear) {
        j["start"] = g.start;
        j["end"] = g.end;
    }
    else {
        j["shape"] = g.shape;
        if (g.focal != kNoId) {
            j["focal"] = g.focal;
        }
    }
    return j;
}

Gradient gradientFromJson(const json& j)
{
    Gradient g;
    if (!gradientKindFromString(j.value("type", std::string("linear")), g.kind)
        || !gradientSpreadFromString(j.value("spread", std::string("pad")), g.spread)) {
        throw SketchError("unknown gradient type or spread");
    }
    for (const json& s : j.value("stops", json::array())) {
        GradientStop stop;
        stop.offset = s.at(0).get<double>();
        if (!Color::fromHex(s.at(1).get<std::string>(), stop.color)) {
            throw SketchError("bad gradient colour '" + s.at(1).get<std::string>() + "'");
        }
        g.stops.push_back(stop);
    }
    g.sortStops();
    auto id = [&](const char* key) {
        return j.contains(key) && j.at(key).is_number_integer() ? j.at(key).get<Id>() : kNoId;
    };
    g.start = id("start");
    g.end = id("end");
    g.shape = id("shape");
    g.focal = id("focal");
    return g;
}

// A text's letters as SVG-like path data: "M x y L x y Q x y x y C x y x y x y Z M ...", in mm to
// a micron (they look the same, in a small part of the file points would take); a curve that's a
// quadratic (as TrueType letters' are) as one, a point shorter.
std::string lettersToPath(const Text& t)
{
    std::string out;
    auto num = [&](double v) {
        char buf[64];
        const auto end = std::to_chars(buf, buf + sizeof buf, v, std::chars_format::fixed, 3).ptr;  // (any locale)
        std::string s(buf, end);
        while (s.back() == '0') {
            s.pop_back();
        }
        if (s.back() == '.') {
            s.pop_back();
        }
        if (s == "-0") {
            s = "0";
        }
        out += s;
    };
    auto point = [&](const Point& p) {
        num(p.x);
        out += ' ';
        num(p.y);
    };
    auto contour = [&](const TextContour& c) {
        if (c.points.empty()) {
            return;
        }
        if (!out.empty()) {
            out += ' ';
        }
        out += "M ";
        point(c.points.front());
        size_t i = 1;
        for (char op : c.ops) {
            const size_t n = op == 'C' ? 3 : 1;
            if (i + n > c.points.size()) {
                break;
            }
            Point control;
            if (op == 'C' && quadraticOf(c.points[i - 1], c.points[i], c.points[i + 1], c.points[i + 2], control)) {
                out += " Q ";
                point(control);
                out += ' ';
                point(c.points[i + 2]);
                i += n;
                continue;
            }
            out += ' ';
            out += op;
            for (size_t k = 0; k < n; ++k) {
                out += ' ';
                point(c.points[i + k]);
            }
            i += n;
        }
        out += " Z";
    };
    if (!t.letters.empty()) {
        for (const TextContour& c : t.letters) {
            contour(c);
        }
    }
    else {
        for (const auto& poly : t.shape) {  // (made without the curves)
            TextContour c{poly, std::string(poly.empty() ? 0 : poly.size() - 1, 'L')};
            contour(c);
        }
    }
    return out;
}

std::vector<TextContour> lettersFromPath(const std::string& d)
{
    std::vector<TextContour> out;
    size_t at = 0;
    auto next = [&](std::string& word) {
        while (at < d.size() && d[at] == ' ') {
            ++at;
        }
        const size_t from = at;
        while (at < d.size() && d[at] != ' ') {
            ++at;
        }
        word.assign(d, from, at - from);
        return !word.empty();
    };
    std::string word;
    auto number = [&]() {
        double v = 0;
        if (!next(word) || std::from_chars(word.data(), word.data() + word.size(), v).ec != std::errc{}) {
            throw SketchError("text letters: expected a number");
        }
        return v;
    };
    while (next(word)) {
        if (word == "M") {
            out.push_back({});
            const double x = number();
            out.back().points.push_back({x, number()});
        }
        else if ((word == "L" || word == "C") && !out.empty()) {
            const char op = word[0];
            const int n = op == 'C' ? 3 : 1;
            for (int k = 0; k < n; ++k) {
                const double x = number();
                out.back().points.push_back({x, number()});
            }
            out.back().ops += op;
        }
        else if (word == "Q" && !out.empty()) {
            // (kept as the cubic it is: control points two thirds of the way to its one, from each end)
            const Point from = out.back().points.back();
            const double qx = number(), qy = number(), x = number(), y = number();
            out.back().points.push_back({from.x + 2.0 / 3 * (qx - from.x), from.y + 2.0 / 3 * (qy - from.y)});
            out.back().points.push_back({x + 2.0 / 3 * (qx - x), y + 2.0 / 3 * (qy - y)});
            out.back().points.push_back({x, y});
            out.back().ops += 'C';
        }
        else if (word != "Z") {
            throw SketchError("text letters: unexpected '" + word + "'");
        }
    }
    return out;
}

// A hatch: its pattern (each family as [angle, x, y, dx, dy, [dashes]]), turned and scaled.
json hatchToJson(const Hatch& h)
{
    json lines = json::array();
    for (const HatchLine& l : h.lines) {
        lines.push_back({l.angle, l.x, l.y, l.dx, l.dy, l.dashes});
    }
    return {{"pattern", h.pattern}, {"lines", std::move(lines)}, {"angle", h.angle}, {"scale", h.scale}, {"width", h.width}};
}

Hatch hatchFromJson(const json& j)
{
    Hatch h;
    h.pattern = j.value("pattern", std::string());
    h.angle = j.value("angle", 0.0);
    h.scale = j.value("scale", 1.0);
    h.width = j.value("width", 0.0);
    for (const json& l : j.value("lines", json::array())) {
        HatchLine line;
        line.angle = l.at(0).get<double>();
        line.x = l.at(1).get<double>();
        line.y = l.at(2).get<double>();
        line.dx = l.at(3).get<double>();
        line.dy = l.at(4).get<double>();
        line.dashes = l.size() > 5 ? l.at(5).get<std::vector<double>>() : std::vector<double>{};
        h.lines.push_back(std::move(line));
    }
    if (!(h.scale > 0)) {
        throw SketchError("a hatch's scale must be more than 0");
    }
    return h;
}
json entityToJson(const Entity& e)
{
    json j{{"id", e.id}, {"type", toString(e.kind())}};
    std::visit(
        [&](const auto& g) {
            using T = std::decay_t<decltype(g)>;
            if constexpr (std::is_same_v<T, Point>) {
                j["x"] = g.x;
                j["y"] = g.y;
            }
            else if constexpr (std::is_same_v<T, Line>) {
                j["p1"] = g.p1;
                j["p2"] = g.p2;
            }
            else if constexpr (std::is_same_v<T, Circle>) {
                j["center"] = g.center;
                j["radius"] = g.radius;
            }
            else if constexpr (std::is_same_v<T, Arc>) {
                j["center"] = g.center;
                j["start"] = g.start;
                j["end"] = g.end;
            }
            else if constexpr (std::is_same_v<T, Ellipse>) {
                j["center"] = g.center;
                j["major"] = g.major;
                j["minor"] = g.minor;
            }
            else if constexpr (std::is_same_v<T, EllipseArc>) {
                j["center"] = g.center;
                j["major"] = g.major;
                j["minor"] = g.minor;
                j["start"] = g.start;
                j["end"] = g.end;
            }
            else if constexpr (std::is_same_v<T, Spline>) {
                j["points"] = g.points;
                j["kind"] = toString(g.kind);
                if (g.closed) {
                    j["closed"] = true;
                }
                if (g.isPiece()) {
                    j["start"] = g.start;
                    j["end"] = g.end;
                }
            }
        },
        e.geom);
    if (e.construction) {
        j["construction"] = true;
    }
    if (e.fixed) {
        j["fixed"] = true;
    }
    if (e.sublayer != kNoId) {
        j["sublayer"] = e.sublayer;
    }
    if (e.projectedFrom != kNoId) {
        j["projected_from"] = e.projectedFrom;
    }
    return j;
}

Entity entityFromJson(const json& j)
{
    Entity e;
    e.id = j.at("id").get<Id>();
    const std::string type = j.at("type").get<std::string>();
    if (type == "point") {
        e.geom = Point{j.at("x").get<double>(), j.at("y").get<double>()};
    }
    else if (type == "line") {
        e.geom = Line{j.at("p1").get<Id>(), j.at("p2").get<Id>()};
    }
    else if (type == "circle") {
        e.geom = Circle{j.at("center").get<Id>(), j.at("radius").get<double>()};
    }
    else if (type == "arc") {
        e.geom = Arc{j.at("center").get<Id>(), j.at("start").get<Id>(), j.at("end").get<Id>()};
    }
    else if (type == "ellipse") {
        e.geom = Ellipse{j.at("center").get<Id>(), j.at("major").get<Id>(), j.at("minor").get<Id>()};
    }
    else if (type == "ellipse_arc") {
        e.geom = EllipseArc{j.at("center").get<Id>(), j.at("major").get<Id>(), j.at("minor").get<Id>(),
                            j.at("start").get<Id>(), j.at("end").get<Id>()};
    }
    else if (type == "spline") {
        Spline sp;
        sp.points = j.at("points").get<std::vector<Id>>();
        if (!splineKindFromString(j.value("kind", std::string("fit")), sp.kind)) {
            throw SketchError("unknown spline kind '" + j.value("kind", std::string()) + "'");
        }
        sp.closed = j.value("closed", false);
        sp.start = j.value("start", Id{kNoId});
        sp.end = j.value("end", Id{kNoId});
        e.geom = std::move(sp);
    }
    else {
        throw SketchError("unknown entity type '" + type + "'");
    }
    e.construction = j.value("construction", false);
    e.fixed = j.value("fixed", false);
    e.sublayer = j.value("sublayer", Id{kNoId});
    e.projectedFrom = j.value("projected_from", Id{kNoId});
    return e;
}

json constraintToJson(const Constraint& c)
{
    json j{{"id", c.id}, {"type", toString(c.type)}, {"refs", c.refs}};
    if (!c.params.empty()) {
        j["params"] = c.params;
    }
    if (hasValue(c.type)) {
        j["value"] = c.value;
        if (c.labelX != 0.0 || c.labelY != 0.0) {
            j["label"] = {c.labelX, c.labelY};
        }
        if (c.flip != 0) {
            j["reverse"] = {(c.flip & 1) != 0, (c.flip & 2) != 0};
        }
    }
    if (c.sublayer != kNoId) {
        j["sublayer"] = c.sublayer;
    }
    return j;
}

Constraint constraintFromJson(const json& j)
{
    Constraint c;
    c.id = j.at("id").get<Id>();
    const std::string type = j.at("type").get<std::string>();
    if (!constraintTypeFromString(type, c.type)) {
        throw SketchError("unknown constraint type '" + type + "'");
    }
    c.refs = j.at("refs").get<std::vector<Id>>();
    c.value = j.value("value", 0.0);
    c.params = j.value("params", std::vector<double>{});
    if (j.contains("label")) {
        const auto& label = j.at("label");
        c.labelX = label.at(0).get<double>();
        c.labelY = label.at(1).get<double>();
    }
    if (j.contains("reverse")) {
        const auto& r = j.at("reverse");
        c.flip = static_cast<std::uint8_t>((r.at(0).get<bool>() ? 1 : 0) | (r.at(1).get<bool>() ? 2 : 0));
    }
    c.sublayer = j.value("sublayer", Id{kNoId});
    return c;
}
}  // namespace

std::string toJson(const Sketch& sketch, bool compact, const std::vector<std::string>* history)
{
    json entities = json::array();
    for (const auto& [id, e] : sketch.entities()) {
        entities.push_back(entityToJson(e));
    }
    json constraints = json::array();
    for (const auto& [id, c] : sketch.constraints()) {
        constraints.push_back(constraintToJson(c));
    }
    json doc{{"format", kFormat}, {"version", kFileVersion}};
    if (sketch.origin() != kNoId) {
        doc["origin"] = sketch.origin();
    }
    if (sketch.xAxis() != kNoId) {
        doc["axes"] = {sketch.xAxis(), sketch.yAxis()};
    }
    if (sketch.pageCorners()[0] != kNoId) {
        doc["page_frame"] = {{"corners", sketch.pageCorners()}, {"edges", sketch.pageEdges()}};
    }
    doc["entities"] = std::move(entities);
    doc["constraints"] = std::move(constraints);
    if (!sketch.strokes().empty()) {
        json strokes = json::array();
        for (const auto& [curve, style] : sketch.strokes()) {
            json width = style.none() ? json("none") : json(style.width);
            json j{{"curve", curve}, {"color", style.color.hex()}, {"width", std::move(width)}, {"dash", toString(style.dash)}};
            if (style.gradient.active()) {
                j["gradient"] = gradientToJson(style.gradient);
            }
            strokes.push_back(std::move(j));
        }
        doc["strokes"] = std::move(strokes);
    }
    if (!sketch.fills().empty()) {
        json fills = json::array();
        for (const auto& [id, f] : sketch.fills()) {
            json j{{"id", id},
                   {"color", f.color.hex()},
                   {"seed", {f.seed.x, f.seed.y}},
                   {"boundary", f.boundary},
                   {"layer", f.layer}};
            if (f.gradient.active()) {
                j["gradient"] = gradientToJson(f.gradient);
            }
            if (f.hatch.active()) {
                j["hatch"] = hatchToJson(f.hatch);
            }
            fills.push_back(std::move(j));
        }
        doc["fills"] = std::move(fills);
    }
    if (!sketch.texts().empty()) {
        json texts = json::array();
        for (const auto& [id, t] : sketch.texts()) {
            json j{{"id", id}, {"anchor", t.anchor}, {"text", t.text}, {"font", t.font}, {"height", t.height},
                   {"angle", t.angle}, {"outline", toString(t.outline)}, {"letters", lettersToPath(t)}};
            if (t.bold) {
                j["bold"] = true;
            }
            if (t.italic) {
                j["italic"] = true;
            }
            if (t.minThickness > 0.0) {
                j["min_thickness"] = t.minThickness;
            }
            if (t.spacing != 0.0) {
                j["spacing"] = t.spacing;
            }
            if (t.align != TextAlign::Left) {
                j["align"] = toString(t.align);
            }
            if (t.valign != TextVAlign::Baseline) {
                j["valign"] = toString(t.valign);
            }
            if (t.flipH) {
                j["flip_h"] = true;
            }
            if (t.flipV) {
                j["flip_v"] = true;
            }
            if (t.wrap) {
                j["wrap"] = true;
            }
            if (t.grow) {
                j["grow"] = true;
            }
            if (t.path != kNoId) {
                j["path"] = t.path;
                if (!t.pathMore.empty()) {
                    j["pathMore"] = t.pathMore;
                }
            }
            if (t.inBox()) {
                j["box"] = {t.boxSide, t.boxCorner};
            }
            if (t.sublayer != kNoId) {
                j["sublayer"] = t.sublayer;
            }
            if (t.hasColor) {
                j["color"] = t.color.hex();
                if (t.gradient.active()) {
                    j["gradient"] = gradientToJson(t.gradient);
                }
            }
            if (!t.runs.empty()) {
                // (rounded to a micron: they're for other programs' text, not the letters)
                auto um = [](double v) { return std::round(v * 1000.0) / 1000.0; };
                json runs = json::array();
                for (const TextRun& r : t.runs) {
                    json g = json::array();
                    for (double x : r.glyphs) {
                        g.push_back(um(x));
                    }
                    json jr{{"text", r.text}, {"at", {um(r.at.x), um(r.at.y)}}, {"size", um(r.size)}, {"glyphs", std::move(g)}};
                    if (r.angle != 0.0) {
                        jr["angle"] = std::round(r.angle * 1e4) / 1e4;
                    }
                    if (r.tracking != 0.0) {
                        jr["tracking"] = um(r.tracking);
                    }
                    runs.push_back(std::move(jr));
                }
                j["runs"] = std::move(runs);
            }
            texts.push_back(std::move(j));
        }
        doc["texts"] = std::move(texts);
    }
    if (!sketch.drawnDimensions().empty()) {
        json dims = json::array();
        for (const auto& [id, d] : sketch.drawnDimensions()) {
            json j{{"id", id},
                   {"kind", toString(d.kind)},
                   {"refs", d.refs},
                   {"label", {d.labelX, d.labelY}},
                   {"text", d.text},
                   {"font", d.font},
                   {"text_height", d.textHeight},
                   {"color", d.style.color.hex()},
                   {"width", d.style.width},
                   {"sublayer", d.sublayer}};
            if (d.decimals >= 0) {
                j["decimals"] = d.decimals;
            }
            if (d.flip != 0) {
                j["flip"] = d.flip;
            }
            if (d.kind == DrawnDimensionKind::Leader) {
                j["along"] = d.along;
                j["tip"] = {d.tip.x, d.tip.y};
            }
            dims.push_back(std::move(j));
        }
        doc["drawn_dimensions"] = std::move(dims);
    }
    if (sketch.dimensionDecimals() != 2) {
        doc["dimension_decimals"] = sketch.dimensionDecimals();
    }
    json sublayers = json::array();
    for (const Sublayer& l : sketch.sublayers()) {
        json j{{"id", l.id}, {"name", l.name}, {"layer", l.layer}};
        if (!l.visible) {
            j["visible"] = false;
        }
        if (l.annotations) {
            j["annotations"] = true;
        }
        sublayers.push_back(std::move(j));
    }
    doc["sublayers"] = std::move(sublayers);
    json layers = json::array();
    for (const Layer& l : sketch.layers()) {
        json j{{"id", l.id}, {"name", l.name}, {"active_sublayer", l.activeSublayer}};
        if (!l.visible) {
            j["visible"] = false;
        }
        layers.push_back(std::move(j));
    }
    doc["layers"] = std::move(layers);
    doc["active_sublayer"] = sketch.activeSublayer();
    if (!sketch.images().empty()) {
        json images = json::array();
        for (const auto& [id, im] : sketch.images()) {
            json j{{"id", id},
                   {"layer", im.layer},
                   {"source", im.source},
                   {"pixels", {im.pixelWidth, im.pixelHeight}},
                   {"center", {im.x, im.y}},
                   {"width", im.width},
                   {"angle", im.angle},
                   {"opacity", im.opacity},
                   {"data", im.data ? toBase64(*im.data) : std::string()}};
            if (im.flipH) {
                j["flip_h"] = true;
            }
            if (im.flipV) {
                j["flip_v"] = true;
            }
            if (!im.visible) {
                j["visible"] = false;
            }
            images.push_back(std::move(j));
        }
        doc["images"] = std::move(images);
    }
    if (const auto& page = sketch.page()) {
        json p{{"size", page->size},
               {"width", page->width},
               {"height", page->height},
               {"origin", toString(page->origin)}};
        if (page->hasColor) {
            p["color"] = page->color.hex();
        }
        doc["page"] = std::move(p);
    }
    if (history && !history->empty()) {
        doc["history"] = *history;
    }
    return compact ? doc.dump() : doc.dump(2) + "\n";
}

Sketch fromJson(const std::string& text, std::vector<std::string>* history)
{
    try {
        const json doc = json::parse(text);
        const std::string format = doc.value("format", std::string());
        if (format != kFormat && format != kOldFormat) {
            throw SketchError("not a Setsquare sketch file");
        }
        const int version = doc.value("version", 0);
        if (version < 1 || version > kFileVersion) {
            throw SketchError("unsupported file version " + std::to_string(version) + " (this build reads up to "
                              + std::to_string(kFileVersion) + ")");
        }

        Sketch sketch;
        if (doc.contains("sublayers")) {
            std::vector<Sublayer> sublayers;
            for (const json& j : doc.at("sublayers")) {
                sublayers.push_back({j.at("id").get<Id>(), j.value("name", std::string()), j.value("visible", true),
                                     j.value("layer", Id{1}), j.value("annotations", false)});
            }
            sketch.setSublayers(std::move(sublayers), doc.value("active_sublayer", Id{kNoId}));
        }
        if (doc.contains("layers")) {
            std::vector<Layer> layers;
            for (const json& j : doc.at("layers")) {
                layers.push_back({j.at("id").get<Id>(), j.value("name", std::string()), j.value("visible", true),
                                  j.value("active_sublayer", Id{kNoId})});
            }
            sketch.setLayers(std::move(layers));
        }
        for (const json& j : doc.value("images", json::array())) {
            Image im;
            im.id = j.at("id").get<Id>();
            im.layer = j.value("layer", Id{kNoId});
            if (!sketch.hasLayer(im.layer)) {
                im.layer = sketch.layers().back().id;
            }
            im.source = j.value("source", std::string());
            im.pixelWidth = std::max(1, j.at("pixels").at(0).get<int>());
            im.pixelHeight = std::max(1, j.at("pixels").at(1).get<int>());
            im.x = j.at("center").at(0).get<double>();
            im.y = j.at("center").at(1).get<double>();
            im.width = j.value("width", im.width);
            im.angle = j.value("angle", 0.0);
            im.opacity = j.value("opacity", im.opacity);
            im.flipH = j.value("flip_h", false);
            im.flipV = j.value("flip_v", false);
            im.visible = j.value("visible", true);
            im.data = std::make_shared<const std::string>(fromBase64(j.value("data", std::string())));
            sketch.insertImage(im);
        }
        std::optional<Page> loadedPage;
        if (doc.contains("page")) {
            const json& j = doc.at("page");
            Page page;
            page.size = j.value("size", std::string("custom"));
            page.width = j.value("width", page.width);
            page.height = j.value("height", page.height);
            if (!pageOriginFromString(j.value("origin", std::string("bottom_left")), page.origin)) {
                throw SketchError("unknown page origin '" + j.value("origin", std::string()) + "'");
            }
            if (j.contains("color")) {
                page.hasColor = Color::fromHex(j.at("color").get<std::string>(), page.color);
            }
            loadedPage = page;  // (set once the geometry's in: its frame is some of it)
        }
        // Points first, so curves can refer to them regardless of order in the file.
        std::vector<Entity> curves;
        for (const json& j : doc.at("entities")) {
            Entity e = entityFromJson(j);
            if (e.kind() == EntityKind::Point) {
                sketch.insertEntity(e);
            }
            else {
                curves.push_back(std::move(e));
            }
        }
        for (const Entity& e : curves) {
            sketch.insertEntity(e);
        }
        if (doc.contains("origin")) {
            sketch.setOrigin(doc.at("origin").get<Id>());
            if (doc.contains("axes")) {
                const auto& axes = doc.at("axes");
                sketch.setAxes(axes.at(0).get<Id>(), axes.at(1).get<Id>());
            }
        }
        if (doc.contains("page_frame")) {
            const json& f = doc.at("page_frame");
            sketch.setPageFrame(f.at("corners").get<std::array<Id, 4>>(), f.at("edges").get<std::array<Id, 4>>());
        }
        for (const json& j : doc.at("constraints")) {
            sketch.insertConstraint(constraintFromJson(j));
        }
        for (const json& j : doc.value("strokes", json::array())) {
            StrokeStyle style;
            const std::string color = j.value("color", std::string("#000000"));
            if (!Color::fromHex(color, style.color)) {
                throw SketchError("bad stroke colour '" + color + "'");
            }
            if (j.contains("width") && j.at("width").is_string()) {
                if (j.at("width").get<std::string>() != "none") {
                    throw SketchError("bad stroke width '" + j.at("width").get<std::string>() + "'");
                }
                style.width = StrokeStyle::kNone;
            }
            else {
                style.width = std::max(0.0, j.value("width", style.width));
            }
            const std::string dash = j.value("dash", std::string("solid"));
            if (!dashFromString(dash, style.dash)) {
                throw SketchError("unknown line style '" + dash + "'");
            }
            if (j.contains("gradient")) {
                style.gradient = gradientFromJson(j.at("gradient"));
            }
            sketch.setStroke(j.at("curve").get<Id>(), style);
        }
        for (const json& j : doc.value("fills", json::array())) {
            Fill f;
            f.id = j.at("id").get<Id>();
            const std::string color = j.value("color", std::string("#000000"));
            if (!Color::fromHex(color, f.color)) {
                throw SketchError("bad fill colour '" + color + "'");
            }
            if (j.contains("gradient")) {
                f.gradient = gradientFromJson(j.at("gradient"));
            }
            if (j.contains("hatch")) {
                f.hatch = hatchFromJson(j.at("hatch"));
            }
            const auto& seed = j.at("seed");
            f.seed = Point{seed.at(0).get<double>(), seed.at(1).get<double>()};
            f.boundary = j.value("boundary", std::vector<Id>{});
            f.layer = j.value("layer", Id{kNoId});
            if (!sketch.hasLayer(f.layer)) {
                f.layer = sketch.layers().back().id;  // (from before layers: the only one)
            }
            sketch.insertFill(f);
        }
        for (const json& j : doc.value("texts", json::array())) {
            Text t;
            t.id = j.at("id").get<Id>();
            t.anchor = j.at("anchor").get<Id>();
            t.text = j.value("text", std::string());
            t.font = j.value("font", std::string());
            t.bold = j.value("bold", false);
            t.italic = j.value("italic", false);
            t.height = j.value("height", 10.0);
            t.angle = j.value("angle", 0.0);
            t.minThickness = j.value("min_thickness", 0.0);
            t.spacing = j.value("spacing", 0.0);
            t.flipH = j.value("flip_h", false);
            t.flipV = j.value("flip_v", false);
            t.wrap = j.value("wrap", false);
            t.grow = j.value("grow", false);
            t.path = j.value("path", Id{kNoId});
            t.pathMore = j.value("pathMore", std::vector<Id>{});
            t.sublayer = j.value("sublayer", Id{kNoId});
            if (j.contains("box")) {
                t.boxSide = j.at("box").at(0).get<Id>();
                t.boxCorner = j.at("box").at(1).get<Id>();
            }
            if (!textAlignFromString(j.value("align", std::string("left")), t.align)
                || !textVAlignFromString(j.value("valign", std::string("baseline")), t.valign)) {
                throw SketchError("unknown text alignment");
            }
            if (!textOutlineFromString(j.value("outline", std::string("as_designed")), t.outline)) {
                throw SketchError("unknown text outline '" + j.value("outline", std::string()) + "'");
            }
            if (j.contains("color")) {
                t.hasColor = Color::fromHex(j.at("color").get<std::string>(), t.color);
            }
            if (t.hasColor && j.contains("gradient")) {
                t.gradient = gradientFromJson(j.at("gradient"));
            }
            if (j.contains("letters")) {
                t.letters = lettersFromPath(j.at("letters").get<std::string>());
            }
            else {
                // (files from before: the letters as points, a flat [x0, y0, x1, y1, ...] each)
                for (const json& contour : j.value("shape", json::array())) {
                    const std::vector<double> flat = contour.get<std::vector<double>>();
                    TextContour c;
                    for (size_t i = 0; i + 1 < flat.size(); i += 2) {
                        c.points.push_back({flat[i], flat[i + 1]});
                        if (i > 0) {
                            c.ops += 'L';
                        }
                    }
                    t.letters.push_back(std::move(c));
                }
            }
            t.shape = flattenLetters(t.letters, letterTolerance(t.height));
            for (const json& jr : j.value("runs", json::array())) {
                TextRun r;
                r.text = jr.value("text", std::string());
                const std::vector<double> at = jr.value("at", std::vector<double>{0.0, 0.0});
                if (at.size() == 2) {
                    r.at = {at[0], at[1]};
                }
                r.angle = jr.value("angle", 0.0);
                r.size = jr.value("size", 0.0);
                r.tracking = jr.value("tracking", 0.0);
                r.glyphs = jr.value("glyphs", std::vector<double>{});
                t.runs.push_back(std::move(r));
            }
            sketch.insertText(t);
        }
        sketch.setDimensionDecimals(doc.value("dimension_decimals", 2));
        for (const json& j : doc.value("drawn_dimensions", json::array())) {
            DrawnDimension d;
            d.id = j.at("id").get<Id>();
            if (!drawnDimensionKindFromString(j.value("kind", std::string()), d.kind)) {
                throw SketchError("unknown dimension kind '" + j.value("kind", std::string()) + "'");
            }
            d.refs = j.value("refs", std::vector<Id>{});
            if (j.contains("label")) {
                d.labelX = j.at("label").at(0).get<double>();
                d.labelY = j.at("label").at(1).get<double>();
            }
            d.text = j.value("text", std::string("<>"));
            d.font = j.value("font", std::string());
            d.textHeight = j.value("text_height", d.textHeight);
            d.decimals = j.value("decimals", -1);
            d.flip = j.value("flip", std::uint8_t{0});
            // ("along_y", from when an ordinate showed one of x and y: they show both now)
            d.along = j.value("along", 0.0);
            if (j.contains("tip")) {
                d.tip = {j.at("tip").at(0).get<double>(), j.at("tip").at(1).get<double>()};
            }
            const std::string color = j.value("color", std::string("#ffffff"));
            if (!Color::fromHex(color, d.style.color)) {
                throw SketchError("bad dimension colour '" + color + "'");
            }
            d.style.width = std::max(0.0, j.value("width", d.style.width));
            d.sublayer = j.value("sublayer", Id{kNoId});
            sketch.insertDrawnDimension(d);
        }
        // Items from before sublayers (or in one that's gone) go in the first one.
        const Id first = sketch.sublayers().front().id;
        std::vector<Id> homeless;
        for (const auto& [id, e] : sketch.entities()) {
            if (!sketch.isReference(id) && !sketch.hasSublayer(e.sublayer)) {
                homeless.push_back(id);
            }
        }
        for (const auto& [id, c] : sketch.constraints()) {
            if (!sketch.hasSublayer(c.sublayer)) {
                homeless.push_back(id);
            }
        }
        for (const auto& [id, t] : sketch.texts()) {
            if (!sketch.hasSublayer(t.sublayer)) {
                homeless.push_back(id);
            }
        }
        for (const auto& [id, d] : sketch.drawnDimensions()) {
            if (!sketch.hasSublayer(d.sublayer)) {
                homeless.push_back(id);
            }
        }
        for (Id id : homeless) {
            sketch.setSublayerOf(id, first);
        }
        // Last, so a frame made for a page from before frames takes ids nothing else has.
        sketch.setPage(loadedPage);
        if (history) {
            *history = doc.value("history", std::vector<std::string>{});
        }
        return sketch;
    }
    catch (const json::exception& e) {
        throw SketchError(std::string("malformed sketch file: ") + e.what());
    }
}

}  // namespace core
