#include "Groups.h"

#include <algorithm>
#include <cmath>

namespace core::group
{

Members members(const Sketch& sketch, const Constraint& g)
{
    Members m;
    for (Id r : g.refs) {
        if (!sketch.hasEntity(r)) {
            continue;
        }
        (sketch.entity(r).kind() == EntityKind::Point ? m.points : m.circles).push_back(r);
    }
    return m;
}

Id create(Sketch& sketch, const std::vector<Id>& given)
{
    // A group in the items (its symbol) brings its members.
    std::vector<Id> items;
    std::vector<Id> absorbed;  // groups this one takes in
    for (Id id : given) {
        if (sketch.hasConstraint(id) && sketch.constraint(id).type == ConstraintType::Group) {
            absorbed.push_back(id);
            const auto& refs = sketch.constraint(id).refs;
            items.insert(items.end(), refs.begin(), refs.end());
        }
        else {
            items.push_back(id);
        }
    }
    std::vector<Id> points, circles;
    auto addPoint = [&](Id p) {
        if (std::ranges::find(points, p) == points.end()) {
            points.push_back(p);
        }
    };
    // Groups don't chain: any group sharing a point or circle with this one joins it (and so on).
    auto expand = [&] {
        for (bool grew = true; grew;) {
            grew = false;
            for (const auto& [id, c] : sketch.constraints()) {
                if (c.type != ConstraintType::Group || std::ranges::find(absorbed, id) != absorbed.end()) {
                    continue;
                }
                const bool shares = std::ranges::any_of(c.refs, [&](Id r) {
                    return std::ranges::find(points, r) != points.end() || std::ranges::find(circles, r) != circles.end();
                });
                if (shares) {
                    absorbed.push_back(id);
                    for (Id r : c.refs) {
                        if (sketch.entity(r).kind() == EntityKind::Point) {
                            addPoint(r);
                        }
                        else if (std::ranges::find(circles, r) == circles.end()) {
                            circles.push_back(r);
                        }
                    }
                    grew = true;
                }
            }
        }
    };
    for (Id id : items) {
        if (!sketch.hasEntity(id) || sketch.isReference(id)) {
            continue;  // (the origin and axes are where they are: not part of a group)
        }
        for (Id p : sketch.definingPoints(id)) {
            if (!sketch.isReference(p)) {
                addPoint(p);
            }
        }
        if (sketch.entity(id).kind() == EntityKind::Circle && std::ranges::find(circles, id) == circles.end()) {
            circles.push_back(id);
        }
    }
    expand();
    if (points.size() < 2) {
        throw SketchError("a group needs at least two things (or a curve) to hold together");
    }
    // The anchors: the first point, and the one farthest from it (the steadiest frame).
    auto at = [&](Id p) { return sketch.point(p); };
    size_t far = 1;
    double best = -1.0;
    for (size_t i = 1; i < points.size(); ++i) {
        const double d = std::hypot(at(points[i]).x - at(points[0]).x, at(points[i]).y - at(points[0]).y);
        if (d > best) {
            best = d;
            far = i;
        }
    }
    if (!(best > 1e-9)) {
        throw SketchError("a group's members must be in more than one place");
    }
    std::swap(points[1], points[far]);

    const Point a0 = at(points[0]), a1 = at(points[1]);
    const double ux = a1.x - a0.x, uy = a1.y - a0.y, len2 = ux * ux + uy * uy;
    std::vector<double> params{std::sqrt(len2)};
    for (size_t i = 2; i < points.size(); ++i) {
        const double dx = at(points[i]).x - a0.x, dy = at(points[i]).y - a0.y;
        params.push_back((dx * ux + dy * uy) / len2);  // along u
        params.push_back((ux * dy - uy * dx) / len2);  // across it
    }
    for (Id c : circles) {
        params.push_back(sketch.circle(c).radius);
    }
    std::vector<Id> refs = points;
    refs.insert(refs.end(), circles.begin(), circles.end());
    for (Id g : absorbed) {
        sketch.remove(g);  // (now part of this one)
    }
    return sketch.addConstraint(ConstraintType::Group, std::move(refs), 0.0, std::move(params));
}

bool covers(const Sketch& sketch, const Members& m, Id entity)
{
    if (!sketch.hasEntity(entity) || sketch.isReference(entity)) {
        return false;
    }
    if (sketch.entity(entity).kind() == EntityKind::Circle
        && std::ranges::find(m.circles, entity) == m.circles.end()) {
        return false;
    }
    for (Id p : sketch.definingPoints(entity)) {
        if (std::ranges::find(m.points, p) == m.points.end()) {
            return false;
        }
    }
    return true;
}

Id holder(const Sketch& sketch, Id constraint)
{
    if (!sketch.hasConstraint(constraint)) {
        return kNoId;
    }
    const Constraint& c = sketch.constraint(constraint);
    if (c.type == ConstraintType::Group || c.refs.empty()) {
        return kNoId;
    }
    for (const auto& [id, g] : sketch.constraints()) {
        if (g.type != ConstraintType::Group) {
            continue;
        }
        const Members m = members(sketch, g);
        if (std::ranges::all_of(c.refs, [&](Id r) { return covers(sketch, m, r); })) {
            return id;
        }
    }
    return kNoId;
}

bool setsOrientation(ConstraintType type)
{
    return type == ConstraintType::Horizontal || type == ConstraintType::Vertical
        || type == ConstraintType::HorizontalDistance || type == ConstraintType::VerticalDistance;
}

}  // namespace core::group
