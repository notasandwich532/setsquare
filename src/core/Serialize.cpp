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
    if (isDimension(c.type)) {
        j["value"] = c.value;
        if (c.labelX != 0.0 || c.labelY != 0.0) {
            j["label"] = {c.labelX, c.labelY};
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
    if (j.contains("label")) {
        const auto& label = j.at("label");
        c.labelX = label.at(0).get<double>();
        c.labelY = label.at(1).get<double>();
    }
    return c;
}
}  // namespace

std::string toJson(const Sketch& sketch)
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
    doc["entities"] = std::move(entities);
    doc["constraints"] = std::move(constraints);
    return doc.dump(2) + "\n";
}

Sketch fromJson(const std::string& text)
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
        }
        for (const json& j : doc.at("constraints")) {
            sketch.insertConstraint(constraintFromJson(j));
        }
        return sketch;
    }
    catch (const json::exception& e) {
        throw SketchError(std::string("malformed sketch file: ") + e.what());
    }
}

}  // namespace core
