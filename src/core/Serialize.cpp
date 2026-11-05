#include "Serialize.h"

#include <algorithm>
#include <nlohmann/json.hpp>

#include <string>

namespace core
{

using nlohmann::json;

namespace
{
constexpr const char* kFormat = "drawing_app.sketch";

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
    doc["entities"] = std::move(entities);
    doc["constraints"] = std::move(constraints);
    if (!sketch.strokes().empty()) {
        json strokes = json::array();
        for (const auto& [curve, style] : sketch.strokes()) {
            json width = style.none() ? json("none") : json(style.width);
            strokes.push_back(
                {{"curve", curve}, {"color", style.color.hex()}, {"width", std::move(width)}, {"dash", toString(style.dash)}});
        }
        doc["strokes"] = std::move(strokes);
    }
    if (!sketch.fills().empty()) {
        json fills = json::array();
        for (const auto& [id, f] : sketch.fills()) {
            fills.push_back({{"id", id},
                             {"color", f.color.hex()},
                             {"seed", {f.seed.x, f.seed.y}},
                             {"boundary", f.boundary},
                             {"layer", f.layer}});
        }
        doc["fills"] = std::move(fills);
    }
    if (!sketch.texts().empty()) {
        json texts = json::array();
        for (const auto& [id, t] : sketch.texts()) {
            json shape = json::array();
            for (const auto& contour : t.shape) {
                json points = json::array();
                for (const Point& p : contour) {
                    points.push_back(p.x);
                    points.push_back(p.y);
                }
                shape.push_back(std::move(points));  // flat [x0, y0, x1, y1, ...]
            }
            json j{{"id", id}, {"anchor", t.anchor}, {"text", t.text}, {"font", t.font}, {"height", t.height},
                   {"angle", t.angle}, {"outline", toString(t.outline)}, {"shape", std::move(shape)}};
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
            if (t.path != kNoId) {
                j["path"] = t.path;
            }
            if (t.inBox()) {
                j["box"] = {t.boxSide, t.boxCorner};
            }
            if (t.sublayer != kNoId) {
                j["sublayer"] = t.sublayer;
            }
            if (t.hasColor) {
                j["color"] = t.color.hex();
            }
            texts.push_back(std::move(j));
        }
        doc["texts"] = std::move(texts);
    }
    json sublayers = json::array();
    for (const Sublayer& l : sketch.sublayers()) {
        json j{{"id", l.id}, {"name", l.name}, {"layer", l.layer}};
        if (!l.visible) {
            j["visible"] = false;
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
    if (history && !history->empty()) {
        doc["history"] = *history;
    }
    return compact ? doc.dump() : doc.dump(2) + "\n";
}

Sketch fromJson(const std::string& text, std::vector<std::string>* history)
{
    try {
        const json doc = json::parse(text);
        if (doc.value("format", std::string()) != kFormat) {
            throw SketchError("not a drawing_app sketch file");
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
                                     j.value("layer", Id{1})});
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
            sketch.setStroke(j.at("curve").get<Id>(), style);
        }
        for (const json& j : doc.value("fills", json::array())) {
            Fill f;
            f.id = j.at("id").get<Id>();
            const std::string color = j.value("color", std::string("#000000"));
            if (!Color::fromHex(color, f.color)) {
                throw SketchError("bad fill colour '" + color + "'");
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
            t.path = j.value("path", Id{kNoId});
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
            for (const json& contour : j.value("shape", json::array())) {
                const std::vector<double> flat = contour.get<std::vector<double>>();
                std::vector<Point> points;
                for (size_t i = 0; i + 1 < flat.size(); i += 2) {
                    points.push_back({flat[i], flat[i + 1]});
                }
                t.shape.push_back(std::move(points));
            }
            sketch.insertText(t);
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
        for (Id id : homeless) {
            sketch.setSublayerOf(id, first);
        }
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
