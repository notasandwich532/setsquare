#include "Serialize.h"

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
            strokes.push_back(
                {{"curve", curve}, {"color", style.color.hex()}, {"width", style.width}, {"dash", toString(style.dash)}});
        }
        doc["strokes"] = std::move(strokes);
    }
    if (!sketch.fills().empty()) {
        json fills = json::array();
        for (const auto& [id, f] : sketch.fills()) {
            fills.push_back(
                {{"id", id}, {"color", f.color.hex()}, {"seed", {f.seed.x, f.seed.y}}, {"boundary", f.boundary}});
        }
        doc["fills"] = std::move(fills);
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
        if (doc.value("format", std::string()) != kFormat) {
            throw SketchError("not a drawing_app sketch file");
        }
        const int version = doc.value("version", 0);
        if (version < 1 || version > kFileVersion) {
            throw SketchError("unsupported file version " + std::to_string(version) + " (this build reads up to "
                              + std::to_string(kFileVersion) + ")");
        }

        Sketch sketch;
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
            style.width = j.value("width", style.width);
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
            sketch.insertFill(f);
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
