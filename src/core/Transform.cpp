#include "Transform.h"

#include "Gradient.h"

#include <algorithm>
#include <cmath>
#include <numbers>
#include <set>

namespace core
{

double Affine::scale() const
{
    return std::sqrt(std::abs(det()));
}

Affine Affine::translation(double dx, double dy)
{
    return {1.0, 0.0, 0.0, 1.0, dx, dy};
}

Affine Affine::rotation(Vec2 about, double degrees)
{
    const double r = degrees * std::numbers::pi / 180.0;
    const double cs = std::cos(r), sn = std::sin(r);
    return {cs, -sn, sn, cs, about.x - cs * about.x + sn * about.y, about.y - sn * about.x - cs * about.y};
}

Affine Affine::scaling(Vec2 about, double factor)
{
    return {factor, 0.0, 0.0, factor, about.x * (1.0 - factor), about.y * (1.0 - factor)};
}

Affine Affine::mirror(Vec2 p, Vec2 q)
{
    double ux = q.x - p.x, uy = q.y - p.y;
    const double len = std::hypot(ux, uy);
    if (len == 0.0) {
        throw SketchError("mirror line has no length");
    }
    ux /= len;
    uy /= len;
    // Reflection matrix 2uu^T - I, about p.
    const double a = 2 * ux * ux - 1, b = 2 * ux * uy, d = 2 * uy * uy - 1;
    return {a, b, b, d, p.x - a * p.x - b * p.y, p.y - b * p.x - d * p.y};
}

Affine Affine::then(const Affine& n) const
{
    return {n.a * a + n.b * c,           n.a * b + n.b * d,           n.c * a + n.d * c,
            n.c * b + n.d * d,           n.a * tx + n.b * ty + n.tx, n.c * tx + n.d * ty + n.ty};
}

namespace xform
{

namespace
{
Vec2 at(const Sketch& s, Id point)
{
    const Point& p = s.point(point);
    return {p.x, p.y};
}

bool isLength(ConstraintType type)
{
    switch (type) {
        case ConstraintType::Distance:
        case ConstraintType::HorizontalDistance:
        case ConstraintType::VerticalDistance:
        case ConstraintType::Radius:
        case ConstraintType::Diameter: return true;
        default: return false;
    }
}

// How a map treats horizontal/vertical: keeps them, swaps them, or neither (a general rotation).
enum class AxisMap
{
    Keep,
    Swap,
    Lose,
};

AxisMap axisMap(const Affine& t)
{
    const double s = t.scale();
    if (s == 0.0) {
        return AxisMap::Lose;
    }
    constexpr double eps = 1e-9;
    if (std::abs(t.b) / s < eps && std::abs(t.c) / s < eps) {
        return AxisMap::Keep;
    }
    if (std::abs(t.a) / s < eps && std::abs(t.d) / s < eps) {
        return AxisMap::Swap;
    }
    return AxisMap::Lose;
}

bool isAxial(ConstraintType type)
{
    return type == ConstraintType::Horizontal || type == ConstraintType::Vertical
        || type == ConstraintType::HorizontalDistance || type == ConstraintType::VerticalDistance;
}

ConstraintType swapped(ConstraintType type)
{
    switch (type) {
        case ConstraintType::Horizontal: return ConstraintType::Vertical;
        case ConstraintType::Vertical: return ConstraintType::Horizontal;
        case ConstraintType::HorizontalDistance: return ConstraintType::VerticalDistance;
        case ConstraintType::VerticalDistance: return ConstraintType::HorizontalDistance;
        default: return type;
    }
}
}  // namespace

std::vector<Id> textHolders(const Sketch& s, Id text)
{
    const Text& t = s.text(text);
    std::vector<Id> out;
    if (t.path != kNoId) {
        out.push_back(t.path);
        out.insert(out.end(), t.pathMore.begin(), t.pathMore.end());
        return out;
    }
    out = textPoints(s, t);  // (the anchor; a box's corners)
    if (t.inBox()) {
        for (const auto& [id, e] : s.entities()) {
            const auto* l = std::get_if<Line>(&e.geom);
            if (l && e.construction && std::ranges::find(out, l->p1) != out.end()
                && std::ranges::find(out, l->p2) != out.end()) {
                out.push_back(id);
            }
        }
    }
    return out;
}

std::vector<Id> expand(const Sketch& s, const std::vector<Id>& selection)
{
    std::set<Id> out;
    std::vector<Id> all = selection;
    for (Id id : selection) {
        if (s.hasText(id)) {
            for (Id h : textHolders(s, id)) {
                all.push_back(h);  // (a text: what it hangs on)
            }
        }
    }
    for (Id id : all) {
        if (!s.hasEntity(id) || s.isAxis(id) || s.isHiddenReference(id)) {
            continue;
        }
        out.insert(id);
        for (Id p : s.definingPoints(id)) {
            out.insert(p);
        }
    }
    return {out.begin(), out.end()};
}

namespace
{
// A group's settings for a copy of it scaled (and maybe mirrored): its sizes scale, and its points'
// places across its anchor line flip in a mirror image. Other constraints keep theirs.
std::vector<double> groupParams(const Sketch& s, const Constraint& c, double scale, bool mirrored)
{
    if (c.type != ConstraintType::Group) {
        return c.params;
    }
    std::vector<double> p = c.params;
    const size_t points = static_cast<size_t>(std::ranges::count_if(
        c.refs, [&](Id r) { return s.hasEntity(r) && s.entity(r).kind() == EntityKind::Point; }));
    // (anchor distance, then (along, across) per further point, then radii)
    p[0] *= scale;
    for (size_t i = 2; i < points; ++i) {
        if (mirrored) {
            p[2 + 2 * (i - 2)] = -p[2 + 2 * (i - 2)];
        }
    }
    for (size_t i = 1 + 2 * (points - 2); i < p.size(); ++i) {
        p[i] *= scale;
    }
    return p;
}}  // namespace

namespace
{
// Mirrored, the lines and splines that selected texts run along: made running the other way (as
// arcs are anyway), so the text along each, still read left to right, is the mirror image's.
std::set<Id> runBackwards(const Sketch& s, const std::vector<Id>& selection, const Affine& t)
{
    std::set<Id> out;
    if (!t.mirrors()) {
        return out;
    }
    for (Id id : selection) {
        if (!s.hasText(id) || s.text(id).path == kNoId) {
            continue;
        }
        const Text& text = s.text(id);
        std::vector<Id> along{text.path};
        along.insert(along.end(), text.pathMore.begin(), text.pathMore.end());
        for (Id c : along) {
            if (s.hasEntity(c) && (s.entity(c).kind() == EntityKind::Line || s.entity(c).kind() == EntityKind::Spline)) {
                out.insert(c);
            }
        }
    }
    return out;
}

// A spline the other way: its points in reverse (a piece's ends swapped).
Spline reversed(Spline sp)
{
    std::ranges::reverse(sp.points);
    std::swap(sp.start, sp.end);
    return sp;
}
}  // namespace

Text copyText(const Text& source, const std::map<Id, Id>& map, const Affine& t)
{
    Text x = source;
    x.anchor = map.at(source.anchor);
    if (x.path != kNoId) {
        x.path = map.at(source.path);
        for (Id& more : x.pathMore) {
            more = map.at(more);
        }
    }
    if (x.inBox()) {
        x.boxSide = map.at(source.boxSide);
        x.boxCorner = map.at(source.boxCorner);
    }
    x.copyOf = x.copyFeature = kNoId;
    x.copyInstance = 0;
    x.height *= t.scale();
    x.minThickness *= t.scale();
    // Which way its baseline runs now (a box's and a path's own come from them).
    const double a = source.angle * std::numbers::pi / 180.0;
    const Vec2 run = t.applyLinear({std::cos(a), std::sin(a)});
    const bool onPoint = x.path == kNoId && !x.inBox();
    if (!t.mirrors()) {
        if (onPoint) {
            x.angle = std::atan2(run.y, run.x) * 180.0 / std::numbers::pi;
        }
    }
    else {
        // Readable: a mirror image read the other way round. It runs back along the mirrored
        // baseline, so it starts from the other end: left and right alignment swap.
        if (onPoint) {
            x.angle = std::atan2(-run.y, -run.x) * 180.0 / std::numbers::pi;
        }
        else if (x.inBox()) {
            std::swap(x.anchor, x.boxSide);  // (its bottom side the other way)
        }
        else {
            // (Its curves were made running the other way: the chain's order reverses too.)
            std::vector<Id> along{x.path};
            along.insert(along.end(), x.pathMore.begin(), x.pathMore.end());
            std::ranges::reverse(along);
            x.path = along.front();
            x.pathMore.assign(along.begin() + 1, along.end());
        }
        if (x.align == TextAlign::Left) {
            x.align = TextAlign::Right;
        }
        else if (x.align == TextAlign::Right) {
            x.align = TextAlign::Left;
        }
    }
    x.layoutKey = 0;  // (made afresh: see textshapes::update)
    return x;
}

std::map<Id, Id> copy(const Sketch& from, Sketch& to, const std::vector<Id>& selection, const Affine& t,
                      bool constraints)
{
    const std::vector<Id> ids = expand(from, selection);
    std::map<Id, Id> map;
    const double scale = t.scale();
    const std::set<Id> backwards = runBackwards(from, selection, t);

    for (Id id : ids) {
        if (from.entity(id).kind() == EntityKind::Point) {
            const Vec2 p = t.apply(at(from, id));
            const bool construction = from.entity(id).construction;
            map[id] = to.addPoint(p.x, p.y);
            to.entity(map[id]).construction = construction;
        }
    }
    for (Id id : ids) {
        const Entity e = from.entity(id);  // a copy: `to` may be `from`
        Id made = kNoId;
        if (const auto* l = std::get_if<Line>(&e.geom)) {
            made = backwards.contains(id) ? to.addLine(map.at(l->p2), map.at(l->p1)) : to.addLine(map.at(l->p1), map.at(l->p2));
        }
        else if (const auto* c = std::get_if<Circle>(&e.geom)) {
            made = to.addCircle(map.at(c->center), c->radius * scale);
        }
        else if (const auto* a = std::get_if<Arc>(&e.geom)) {
            made = t.mirrors() ? to.addArc(map.at(a->center), map.at(a->end), map.at(a->start))
                               : to.addArc(map.at(a->center), map.at(a->start), map.at(a->end));
        }
        else if (const auto* el = std::get_if<Ellipse>(&e.geom)) {
            made = to.addEllipse(map.at(el->center), map.at(el->major), map.at(el->minor));
        }
        else if (const auto* ea = std::get_if<EllipseArc>(&e.geom)) {
            // (mirrored, counter-clockwise runs the other way: the ends swap, as for arcs)
            made = t.mirrors() ? to.addEllipseArc(map.at(ea->center), map.at(ea->major), map.at(ea->minor),
                                                  map.at(ea->end), map.at(ea->start))
                               : to.addEllipseArc(map.at(ea->center), map.at(ea->major), map.at(ea->minor),
                                                  map.at(ea->start), map.at(ea->end));
        }
        else if (const auto* sp = std::get_if<Spline>(&e.geom)) {
            Spline copied = *sp;
            for (Id& p : copied.points) {
                p = map.at(p);
            }
            if (copied.isPiece()) {
                copied.start = map.at(sp->start);
                copied.end = map.at(sp->end);
            }
            made = to.addSpline(backwards.contains(id) ? reversed(copied) : copied);
        }
        if (made != kNoId) {
            to.entity(made).construction = e.construction;
            map[id] = made;
            if (const StrokeStyle* style = from.stroke(id)) {
                to.setStroke(made, StrokeStyle(*style));  // copies look the same
            }
        }
    }

    // Gradients use the copied geometry where it was copied too (else the original's, within a
    // sketch; none when pasted from elsewhere).
    const bool sameSketch = &from == &to;
    for (Id id : ids) {
        const auto made = map.find(id);
        if (made == map.end() || !to.stroke(made->second) || !to.stroke(made->second)->gradient.active()) {
            continue;
        }
        StrokeStyle style = *to.stroke(made->second);
        gradient::remap(style.gradient, map, sameSketch);
        to.setStroke(made->second, style);
    }

    // Fills whose whole outline was copied come too.
    std::vector<Fill> fills;
    for (const auto& [id, f] : from.fills()) {
        if (!f.boundary.empty()
            && std::ranges::all_of(f.boundary, [&](Id c) { return map.contains(c); })) {
            fills.push_back(f);
        }
    }
    for (Fill f : fills) {
        const Id was = f.id;
        for (Id& c : f.boundary) {
            c = map.at(c);
        }
        const Vec2 seed = t.apply({f.seed.x, f.seed.y});
        f.seed = Point{seed.x, seed.y};
        gradient::remap(f.gradient, map, sameSketch);
        map[was] = to.addFill(f);
    }

    if (constraints) {
        const AxisMap axes = axisMap(t);
        std::vector<Constraint> internal;
        for (const auto& [id, c] : from.constraints()) {
            if (!isFeature(c.type)
                && std::ranges::all_of(c.refs, [&](Id r) { return map.contains(r); })) {
                internal.push_back(c);
            }
        }
        for (const Constraint& c : internal) {
            ConstraintType type = c.type;
            if (isAxial(type)) {
                if (axes == AxisMap::Lose) {
                    continue;
                }
                if (axes == AxisMap::Swap) {
                    type = swapped(type);
                }
            }
            std::vector<Id> refs;
            for (Id r : c.refs) {
                refs.push_back(map.at(r));
            }
            const double value = isLength(type) ? c.value * scale : c.value;
            const Id made = to.addConstraint(type, std::move(refs), value, groupParams(from, c, scale, t.mirrors()));
            if (!c.expression.empty() && sameSketch && std::abs(scale - 1.0) < 1e-12) {
                to.setConstraintExpression(made, c.expression);  // (same parameters, same size: still true)
            }
            const Vec2 label = t.applyLinear({c.labelX, c.labelY});
            to.setConstraintLabel(made, label.x, label.y);
            to.setConstraintFlip(made, c.flip);
            map[c.id] = made;
        }
    }

    // Texts selected: on the copies of what they hang on, turned as it turns. Mirrored, still
    // readable: running the other way, aligned to the other end (see copyText).
    for (Id id : selection) {
        if (!from.hasText(id) || map.contains(id)) {
            continue;
        }
        Text x = copyText(from.text(id), map, t);
        if (x.path != kNoId) {
            x.anchor = to.definingPoints(x.path).front();  // (its curve's first point: it may run the other way)
        }
        map[id] = to.addText(x);
    }
    return map;
}

void apply(Sketch& s, const std::vector<Id>& selection, const Affine& t)
{
    if (t.mirrors()) {
        throw SketchError("can't mirror in place");
    }
    const std::vector<Id> ids = expand(s, selection);
    // Fixed geometry (and the origin) stays put; the solve afterwards fits the rest around it.
    std::set<Id> pinned;
    for (Id id : ids) {
        const Entity& e = s.entity(id);
        if (e.fixed || s.isReference(id)) {
            pinned.insert(id);
            for (Id p : s.definingPoints(id)) {
                pinned.insert(p);
            }
        }
    }
    const std::set<Id> moving(ids.begin(), ids.end());
    const double scale = t.scale();
    for (Id id : ids) {
        if (pinned.contains(id)) {
            continue;
        }
        Entity& e = s.entity(id);
        if (auto* p = std::get_if<Point>(&e.geom)) {
            const Vec2 q = t.apply({p->x, p->y});
            *p = Point{q.x, q.y};
        }
        else if (auto* c = std::get_if<Circle>(&e.geom)) {
            c->radius *= scale;
        }
    }
    std::vector<Id> scaled;
    for (const auto& [id, c] : s.constraints()) {
        if (isLength(c.type) && std::ranges::all_of(c.refs, [&](Id r) { return moving.contains(r); })) {
            scaled.push_back(id);
        }
    }
    // Groups moved whole keep their shape at the new size.
    std::vector<std::pair<Id, std::vector<double>>> regrouped;
    for (const auto& [id, c] : s.constraints()) {
        if (c.type == ConstraintType::Group && std::ranges::all_of(c.refs, [&](Id r) { return moving.contains(r); })) {
            regrouped.push_back({id, groupParams(s, c, scale, false)});
        }
    }
    for (auto& [id, params] : regrouped) {
        s.setConstraintParams(id, std::move(params));
    }
    for (Id id : scaled) {
        const Constraint& c = s.constraint(id);
        const Vec2 label = t.applyLinear({c.labelX, c.labelY});
        s.setConstraintExpression(id, {});  // (a new size: no longer what its expression says)
        s.setConstraintValue(id, c.value * scale);
        s.setConstraintLabel(id, label.x, label.y);
    }
}

Sketch extract(const Sketch& from, const std::vector<Id>& selection)
{
    Sketch out;
    copy(from, out, selection, Affine{});
    return out;
}

Vec2 anchor(const Sketch& fragment)
{
    double x0 = 1e300, y0 = 1e300, x1 = -1e300, y1 = -1e300;
    for (const auto& [id, e] : fragment.entities()) {
        if (const auto* p = std::get_if<Point>(&e.geom)) {
            x0 = std::min(x0, p->x);
            y0 = std::min(y0, p->y);
            x1 = std::max(x1, p->x);
            y1 = std::max(y1, p->y);
        }
    }
    return x0 > x1 ? Vec2{} : Vec2{(x0 + x1) / 2, (y0 + y1) / 2};
}

std::map<Id, Id> paste(Sketch& to, const Sketch& fragment, Vec2 at)
{
    std::vector<Id> all;
    for (const auto& [id, e] : fragment.entities()) {
        all.push_back(id);
    }
    const Vec2 from = anchor(fragment);
    return copy(fragment, to, all, Affine::translation(at.x - from.x, at.y - from.y));
}

std::map<Id, Id> mirror(Sketch& s, const std::vector<Id>& selection, Id line)
{
    const Line axis = s.line(line);  // throws if it isn't a line
    const Vec2 a = at(s, axis.p1), b = at(s, axis.p2);
    const Affine t = Affine::mirror(a, b);
    const double len = std::hypot(b.x - a.x, b.y - a.y);

    const std::set<Id> backwards = runBackwards(s, selection, t);
    std::vector<Id> ids;
    for (Id id : expand(s, selection)) {
        if (id != line) {
            ids.push_back(id);
        }
    }
    if (ids.empty()) {
        throw SketchError("nothing to mirror");
    }

    // Distance from the mirror line, to share points that lie on it.
    auto onLine = [&](Id p) {
        const Vec2 q = at(s, p);
        const double cross = ((b.x - a.x) * (q.y - a.y) - (b.y - a.y) * (q.x - a.x)) / len;
        return std::abs(cross) < 1e-9 * std::max(1.0, std::hypot(q.x, q.y));
    };

    std::map<Id, Id> map;
    std::vector<std::pair<Id, Id>> pairs;  // (original, mirrored) points to tie together
    for (Id id : ids) {
        if (s.entity(id).kind() != EntityKind::Point) {
            continue;
        }
        if (onLine(id)) {
            map[id] = id;
            continue;
        }
        const Vec2 q = t.apply(at(s, id));
        const bool construction = s.entity(id).construction;
        map[id] = s.addPoint(q.x, q.y);
        s.entity(map[id]).construction = construction;
        pairs.emplace_back(id, map[id]);
    }

    std::vector<std::pair<Id, Id>> circles;
    for (Id id : ids) {
        const Entity e = s.entity(id);
        Id made = kNoId;
        if (const auto* l = std::get_if<Line>(&e.geom)) {
            if (map.at(l->p1) == l->p1 && map.at(l->p2) == l->p2) {
                continue;  // lies on the mirror line: its own mirror image
            }
            made = backwards.contains(id) ? s.addLine(map.at(l->p2), map.at(l->p1)) : s.addLine(map.at(l->p1), map.at(l->p2));
        }
        else if (const auto* c = std::get_if<Circle>(&e.geom)) {
            if (map.at(c->center) == c->center) {
                continue;
            }
            made = s.addCircle(map.at(c->center), c->radius);
            circles.emplace_back(id, made);
        }
        else if (const auto* arc = std::get_if<Arc>(&e.geom)) {
            if (map.at(arc->center) == arc->center && map.at(arc->start) == arc->start
                && map.at(arc->end) == arc->end) {
                continue;
            }
            made = s.addArc(map.at(arc->center), map.at(arc->end), map.at(arc->start));
        }
        else if (std::get_if<Ellipse>(&e.geom) || std::get_if<EllipseArc>(&e.geom) || std::get_if<Spline>(&e.geom)) {
            const std::vector<Id> own = s.definingPoints(id);
            if (std::ranges::all_of(own, [&](Id p) { return map.at(p) == p; })) {
                continue;  // lies on the mirror line
            }
            if (const auto* el = std::get_if<Ellipse>(&e.geom)) {
                made = s.addEllipse(map.at(el->center), map.at(el->major), map.at(el->minor));
            }
            else if (const auto* ea = std::get_if<EllipseArc>(&e.geom)) {
                made = s.addEllipseArc(map.at(ea->center), map.at(ea->major), map.at(ea->minor), map.at(ea->end),
                                       map.at(ea->start));
            }
            else {
                Spline copied = std::get<Spline>(e.geom);
                for (Id& p : copied.points) {
                    p = map.at(p);
                }
                if (copied.isPiece()) {
                    copied.start = map.at(copied.start);
                    copied.end = map.at(copied.end);
                }
                made = s.addSpline(backwards.contains(id) ? reversed(copied) : copied);
            }
        }
        if (made != kNoId) {
            s.entity(made).construction = e.construction;
            s.copyStroke(id, made);
            map[id] = made;
        }
    }

    for (const auto& [p, q] : pairs) {
        s.addConstraint(ConstraintType::Symmetric, {p, q, line});
    }
    for (const auto& [c, copy] : circles) {
        s.addConstraint(ConstraintType::Equal, {c, copy});
    }
    // Texts selected: mirrored onto their holders' images, still readable (see copyText). (A
    // text on a curve lying on the mirror line has no image to go on: left out.)
    for (Id id : selection) {
        if (!s.hasText(id)) {
            continue;
        }
        const std::vector<Id> holders = textHolders(s, id);
        if (std::ranges::all_of(holders, [&](Id h) { return map.contains(h); })) {
            Text x = copyText(s.text(id), map, t);
            if (x.path != kNoId) {
                x.anchor = s.definingPoints(x.path).front();  // (its curve's first point: it runs the other way)
            }
            map[id] = s.addText(x);
        }
    }
    std::erase_if(map, [](const auto& kv) { return kv.first == kv.second; });
    return map;
}

}  // namespace xform
}  // namespace core
