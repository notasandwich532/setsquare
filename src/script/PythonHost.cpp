// The embedded interpreter and the `setsquare` Python module.
//
// The module lives in this file (not a separate one) because PYBIND11_EMBEDDED_MODULE registers
// itself from a static initialiser, and a linker would drop an otherwise-unreferenced object
// file from a static library.

#include "PythonHost.h"

#include "Document.h"
#include "Editor.h"
#include "Images.h"
#include "TextShapes.h"

#include <core/Annotations.h>
#include <core/Parameters.h>
#include <core/Curves.h>
#include <numbers>
#include <core/Dimensions.h>
#include <core/Export.h>
#include <core/Features.h>
#include <core/Gradient.h>
#include <core/Hatch.h>
#include <core/Regions.h>
#include <core/Measure.h>
#include <core/Modify.h>
#include <core/Serialize.h>
#include <core/Solver.h>
#include <core/Transform.h>

// Qt's `slots` macro collides with a struct member in Python's object.h.
#pragma push_macro("slots")
#undef slots
#include <pybind11/embed.h>
#include <pybind11/operators.h>
#include <pybind11/stl.h>
#pragma pop_macro("slots")

#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>

#include <cstdio>
#include <memory>

namespace py = pybind11;
using core::ConstraintType;
using core::EntityKind;
using core::Id;
using core::kNoId;

namespace script
{
namespace
{
Document* g_doc = nullptr;
UiHooks g_ui;
std::function<void(const QString&, bool)> g_output;
bool g_initialised = false;

// Python objects owned by the host; released before the interpreter shuts down.
struct HostObjects
{
    py::object helpers;  // the _setsquare_host helper module
    py::dict consoleGlobals;
    py::object consoleLocals;
};
std::unique_ptr<HostObjects> g_objects;

Document& doc()
{
    if (!g_doc) {
        throw std::runtime_error("no document is open");
    }
    return *g_doc;
}

const core::Sketch& sketch()
{
    return doc().sketch();
}

// One Python API call = one undo step, and not echoed by the recorder (it's already code).
class Call
{
public:
    Call()
        : m_doc(doc())
        , m_silent(m_doc)
    {
        m_doc.beginTransaction();
    }
    ~Call() { m_doc.endTransaction(); }
    Document& target() { return m_doc; }  // (not doc(): that would shadow the free function above)

private:
    Document& m_doc;
    Document::SilentRecording m_silent;
};

// ---------------------------------------------------------------------------------------------
// Handles: Python objects naming a sketch item by id. Reading through them always sees the
// current sketch; using one whose item was deleted raises ValueError.
// ---------------------------------------------------------------------------------------------

struct Item
{
    Id id = kNoId;
};
struct PointH : Item
{};
struct LineH : Item
{};
struct CircleH : Item
{};
struct ArcH : Item
{};
struct EllipseH : Item
{};
struct EllipseArcH : Item
{};
struct SplineH : Item
{};
struct ConstraintH : Item
{};
struct FillH : Item
{};
struct TextH : Item
{};
struct ImageH : Item
{};
struct AnnotationH : Item
{};
// A sublayer, by its own id (sublayers have their own id space).
struct SublayerH
{
    Id id = kNoId;
};
// A layer, likewise.
struct LayerH
{
    Id id = kNoId;
};

const core::Entity& entity(Id id)
{
    if (!sketch().hasEntity(id)) {
        throw py::value_error("item " + std::to_string(id) + " no longer exists");
    }
    return sketch().entity(id);
}

const core::Constraint& constraintOf(Id id)
{
    if (!sketch().hasConstraint(id)) {
        throw py::value_error("constraint " + std::to_string(id) + " no longer exists");
    }
    return sketch().constraint(id);
}

// Whether `id` is anything in the sketch (geometry, a constraint, fill, text, image, annotation).
bool isItem(Id id)
{
    const core::Sketch& s = sketch();
    return s.hasEntity(id) || s.hasConstraint(id) || s.hasFill(id) || s.hasText(id) || s.hasImage(id)
        || s.hasDrawnDimension(id);
}

py::object wrap(Id id)
{
    const core::Sketch& s = sketch();
    if (s.hasDrawnDimension(id)) {
        return py::cast(AnnotationH{{id}});
    }
    if (s.hasConstraint(id)) {
        return py::cast(ConstraintH{{id}});
    }
    if (s.hasFill(id)) {
        return py::cast(FillH{{id}});
    }
    if (s.hasText(id)) {
        return py::cast(TextH{{id}});
    }
    if (s.hasImage(id)) {
        return py::cast(ImageH{{id}});
    }
    switch (entity(id).kind()) {
        case EntityKind::Point: return py::cast(PointH{{id}});
        case EntityKind::Line: return py::cast(LineH{{id}});
        case EntityKind::Circle: return py::cast(CircleH{{id}});
        case EntityKind::Arc: return py::cast(ArcH{{id}});
        case EntityKind::Ellipse: return py::cast(EllipseH{{id}});
        case EntityKind::EllipseArc: return py::cast(EllipseArcH{{id}});
        case EntityKind::Spline: return py::cast(SplineH{{id}});
    }
    return py::none();
}

py::list wrapAll(const std::vector<Id>& ids)
{
    py::list out;
    for (Id id : ids) {
        out.append(wrap(id));
    }
    return out;
}

Id idOf(py::handle h)
{
    if (py::isinstance<Item>(h)) {
        return h.cast<Item&>().id;
    }
    if (py::isinstance<py::int_>(h)) {
        return h.cast<Id>();
    }
    throw py::type_error("expected a sketch item (point, line, circle, arc or constraint)");
}

std::vector<Id> idsOf(const py::args& args)
{
    std::vector<Id> ids;
    for (py::handle h : args) {
        // Accept s.delete(a, b) as well as s.delete([a, b]).
        if (py::isinstance<py::list>(h) || py::isinstance<py::tuple>(h)) {
            for (py::handle inner : h) {
                ids.push_back(idOf(inner));
            }
        }
        else {
            ids.push_back(idOf(h));
        }
    }
    return ids;
}

// One item or a list/tuple of them.
std::vector<Id> itemsOf(py::handle h)
{
    if (py::isinstance<py::sequence>(h) && !py::isinstance<py::str>(h)) {
        std::vector<Id> ids;
        for (py::handle inner : h) {
            ids.push_back(idOf(inner));
        }
        return ids;
    }
    return {idOf(h)};
}

// What each of `ids` became (skipping those with no counterpart).
std::vector<Id> mapped(const std::vector<Id>& ids, const std::map<Id, Id>& map)
{
    std::vector<Id> out;
    for (Id id : ids) {
        if (auto it = map.find(id); it != map.end()) {
            out.push_back(it->second);
        }
    }
    return out;
}

// A colour: "#rrggbb", "#rrggbbaa" or (r, g, b[, a]) with 0-255 components.
core::Color colorArg(py::handle h)
{
    core::Color c;
    if (py::isinstance<py::str>(h)) {
        const std::string text = h.cast<std::string>();
        if (!core::Color::fromHex(text, c)) {
            throw py::value_error("not a colour: '" + text + "' (use '#rrggbb')");
        }
        return c;
    }
    if (py::isinstance<py::sequence>(h)) {
        const auto seq = h.cast<py::sequence>();
        if (seq.size() == 3 || seq.size() == 4) {
            auto part = [&](size_t i) { return static_cast<std::uint8_t>(std::clamp(seq[i].cast<int>(), 0, 255)); };
            return {part(0), part(1), part(2), seq.size() == 4 ? part(3) : std::uint8_t(255)};
        }
    }
    throw py::type_error("a colour is '#rrggbb' or an (r, g, b[, a]) tuple");
}

// A colour argument that may also be a gradient (setsquare.linear / setsquare.radial).
struct PaintArg
{
    core::Color color;
    core::Gradient gradient;  // active when one was given
};

// A pattern's setting given from Python: a number (a plain value: no longer worked out), or an
// expression using the drawing's parameters (kept in `settings`; `field` is what it comes to, for
// now: the Editor works it out again). None: as it is.
template<typename T>
void setting(Editor::Settings& settings, const char* key, const py::object& given, T& field)
{
    if (given.is_none()) {
        return;
    }
    if (py::isinstance<py::str>(given)) {
        settings[key] = given.cast<std::string>();
        return;
    }
    field = given.cast<T>();
    settings.erase(key);
}
PaintArg paintArg(py::handle h)
{
    PaintArg p;
    if (py::isinstance<core::Gradient>(h)) {
        p.gradient = h.cast<core::Gradient>();
        if (!p.gradient.active()) {
            throw py::value_error("a gradient needs at least one colour");
        }
        p.color = p.gradient.average();
        return p;
    }
    p.color = colorArg(h);
    return p;
}

// Gradient stops: colours spread evenly, or (offset, colour) pairs.
std::vector<core::GradientStop> stopsArg(py::handle h)
{
    if (!py::isinstance<py::sequence>(h) || py::isinstance<py::str>(h)) {
        throw py::type_error("stops: a list of colours, or of (offset, colour) pairs");
    }
    const auto seq = h.cast<py::sequence>();
    std::vector<core::GradientStop> stops;
    for (size_t i = 0; i < seq.size(); ++i) {
        const py::handle item = seq[i];
        core::GradientStop stop;
        if (py::isinstance<py::sequence>(item) && !py::isinstance<py::str>(item) && py::len(item) == 2) {
            const auto pairSeq = item.cast<py::sequence>();
            stop.offset = pairSeq[0].cast<double>();
            stop.color = colorArg(pairSeq[1]);
        }
        else {
            stop.offset = seq.size() > 1 ? double(i) / double(seq.size() - 1) : 0.0;
            stop.color = colorArg(item);
        }
        if (stop.offset < 0.0 || stop.offset > 1.0) {
            throw py::value_error("stop offsets run from 0 to 1");
        }
        stops.push_back(stop);
    }
    if (stops.empty()) {
        throw py::value_error("a gradient needs at least one colour");
    }
    return stops;
}

core::GradientSpread spreadArg(const std::string& name)
{
    core::GradientSpread s;
    if (!core::gradientSpreadFromString(name, s)) {
        throw py::value_error("spread: 'pad', 'reflect' or 'repeat'");
    }
    return s;
}

std::pair<double, double> pairOf(py::handle h, const char* what)
{
    if (py::isinstance<py::sequence>(h) && !py::isinstance<py::str>(h)) {
        const auto seq = h.cast<py::sequence>();
        if (seq.size() == 2) {
            return {seq[0].cast<double>(), seq[1].cast<double>()};
        }
    }
    throw py::type_error(std::string(what) + " must be an (x, y) pair");
}

// A point argument: an existing point, or an (x, y) pair that becomes a new point.
Id pointArg(Editor& ed, py::handle h)
{
    if (py::isinstance<PointH>(h)) {
        return h.cast<PointH&>().id;
    }
    if (py::isinstance<Item>(h)) {
        throw py::type_error("expected a point or an (x, y) pair");
    }
    const auto [x, y] = pairOf(h, "a point");
    return ed.point(x, y);
}

// A sublayer argument: a Sublayer handle, or a name.
Id sublayerArg(py::handle h)
{
    if (py::isinstance<SublayerH>(h)) {
        const Id id = h.cast<SublayerH&>().id;
        if (!sketch().hasSublayer(id)) {
            throw py::value_error("that sublayer no longer exists");
        }
        return id;
    }
    // A name: the active layer's sublayer called that, else another layer's; "Layer/Sublayer"
    // to say which layer.
    const std::string name = h.cast<std::string>();
    const core::Sketch& s = sketch();
    for (const core::Sublayer& l : s.sublayers()) {
        if (l.layer == s.activeLayer() && l.name == name) {
            return l.id;
        }
    }
    for (const core::Sublayer& l : s.sublayers()) {
        if (l.name == name || s.layer(l.layer).name + "/" + l.name == name) {
            return l.id;
        }
    }
    throw py::value_error("no sublayer called '" + name + "'");
}

// A layer argument: a Layer handle, or a name.
Id layerArg(py::handle h)
{
    if (py::isinstance<LayerH>(h)) {
        const Id id = h.cast<LayerH&>().id;
        if (!sketch().hasLayer(id)) {
            throw py::value_error("that layer no longer exists");
        }
        return id;
    }
    const std::string name = h.cast<std::string>();
    for (const core::Layer& l : sketch().layers()) {
        if (l.name == name) {
            return l.id;
        }
    }
    throw py::value_error("no layer called '" + name + "'");
}

template<typename F>
Id editReturningId(const char* label, F&& f)
{
    Call call;
    Id out = kNoId;
    call.target().edit(QString::fromLatin1(label), [&](Editor& ed) { out = f(ed); });
    return out;
}

// For shapes: f returns the new curves, which come back as a list of handles.
template<typename F>
py::list editCurves(const char* label, F&& f)
{
    std::vector<Id> ids;
    {
        Call call;
        call.target().edit(QString::fromLatin1(label), [&](Editor& ed) { ids = f(ed); });
    }
    return wrapAll(ids);
}

core::Vec2 vecArg(py::handle h);

// Which side of each curve to keep for a fillet/chamfer: given as near=(pos_a, pos_b), or, when
// the curves share an end, worked out (their other ends).
std::pair<core::Vec2, core::Vec2> cornerNear(Id a, Id b, const py::object& near)
{
    if (!near.is_none()) {
        const auto seq = near.cast<py::sequence>();
        if (seq.size() != 2) {
            throw py::type_error("near must be (position_on_a, position_on_b)");
        }
        return {vecArg(seq[0]), vecArg(seq[1])};
    }
    for (Id p : sketch().definingPoints(a)) {
        Id ca, cb;
        core::Vec2 na, nb;
        if (core::modify::cornerAt(sketch(), p, ca, na, cb, nb) && ((ca == a && cb == b) || (ca == b && cb == a))) {
            return ca == a ? std::pair{na, nb} : std::pair{nb, na};
        }
    }
    throw py::value_error("these curves don't share a corner; say which sides to keep with near=(pos_a, pos_b)");
}

// A position argument: an (x, y) pair, or a Point's current position.
core::Vec2 vecArg(py::handle h)
{
    if (py::isinstance<PointH>(h)) {
        const Id id = h.cast<PointH&>().id;
        entity(id);
        const core::Point& p = sketch().point(id);
        return {p.x, p.y};
    }
    const auto [x, y] = pairOf(h, "a position");
    return {x, y};
}

// s.divide's spacing: count=n, or ratio="3:2:1" / [3, 2, 1].
core::feature::DivideParams divideArgs(const py::object& count, const py::object& ratio, bool reversed)
{
    if (count.is_none() == ratio.is_none()) {
        throw py::value_error("give count= or ratio= (one of them)");
    }
    core::feature::DivideParams p;
    if (!count.is_none()) {
        p = core::feature::evenly(count.cast<int>());
    }
    else {
        std::string text;
        if (py::isinstance<py::str>(ratio)) {
            text = ratio.cast<std::string>();
        }
        else {
            for (py::handle part : ratio) {
                text += std::to_string(part.cast<double>()) + ":";
            }
        }
        p.parts = core::feature::parseRatio(text);
        p.byRatio = true;
    }
    p.reversed = reversed;
    return p;
}

// Adds a (checked) constraint or dimension; refuses over-constraining like the UI does.
py::object addConstraint(ConstraintType type, std::vector<Id> refs, py::object value, py::object label,
                         std::uint8_t flip = 0)
{
    double v = 0.0;
    if (core::isDimension(type) || type == ConstraintType::Ratio) {
        if (value.is_none()) {
            // Measure in the normalised order (point before line).
            std::vector<Id> measured = refs;
            if (type == ConstraintType::Distance && measured.size() == 2
                && sketch().hasEntity(measured[0]) && sketch().entity(measured[0]).kind() == EntityKind::Line) {
                std::swap(measured[0], measured[1]);
            }
            std::string why;
            if (!sketch().canAddConstraint(type, measured, &why)) {
                throw py::value_error(why);
            }
            v = core::measure(sketch(), type, measured, flip);
        }
        else {
            v = value.cast<double>();
        }
    }
    double lx = 0.0, ly = 0.0;
    if (!label.is_none()) {
        std::tie(lx, ly) = pairOf(label, "label");
    }
    Call call;
    QString error;
    const Id id = call.target().addConstraint(type, std::move(refs), v, &error, lx, ly, flip);
    if (id == kNoId) {
        throw py::value_error(error.toStdString());
    }
    return wrap(id);
}

std::string itemName(Id id)
{
    return Editor::name(sketch(), id).toStdString();
}

std::string fmt(double v)
{
    return Editor::number(v).toStdString();
}

// ---------------------------------------------------------------------------------------------
// Sketch / Document / App facades (stateless: they act on whatever document is current)
// ---------------------------------------------------------------------------------------------

struct SketchH
{};
struct DocumentH
{};
struct AppH
{};
struct Transaction
{};

UiHooks& ui()
{
    if (!g_ui.fit) {
        throw std::runtime_error("no user interface (running headless)");
    }
    return g_ui;
}

void dragPoints(const std::vector<Id>& points, double dx, double dy)
{
    Call call;
    call.target().beginDrag(points);
    call.target().dragTo(dx, dy);
    call.target().endDrag();
}

// s.annotate / s.edit_annotation's settings, onto `d` (None: as it is). `at` last: where the label
// goes depends on the rest.
void annotationSettings(core::DrawnDimension& d, const py::object& at, const py::object& text,
                        const py::object& decimals, const py::object& textHeight, const py::object& width,
                        const py::object& color, const py::object& font, const py::object& origin,
                        const py::object& along, const py::object& tip, const py::object& flip)
{
    if (!origin.is_none()) {
        if (d.kind != core::DrawnDimensionKind::Ordinate || d.refs.empty()) {
            throw py::value_error("origin= is an ordinate's (the point it's measured from)");
        }
        d.refs.resize(1);
        d.refs.push_back(idOf(origin));
    }
    if (!text.is_none()) {
        d.text = text.cast<std::string>();
    }
    if (!decimals.is_none()) {
        d.decimals = decimals.cast<int>();
    }
    if (!textHeight.is_none()) {
        d.textHeight = textHeight.cast<double>();
    }
    if (!width.is_none()) {
        d.style.width = width.cast<double>();
    }
    if (!color.is_none()) {
        d.style.color = colorArg(color);
    }
    if (!font.is_none()) {
        d.font = font.cast<std::string>();
    }
    if (!along.is_none()) {
        d.along = along.cast<double>();
    }
    if (!tip.is_none()) {
        const core::Vec2 t = vecArg(tip);
        d.tip = {t.x, t.y};
    }
    if (!flip.is_none()) {
        d.flip = static_cast<std::uint8_t>(flip.cast<int>());
    }
    if (!at.is_none()) {
        for (Id r : d.refs) {
            entity(r);  // (gone: say so, rather than lay out nothing)
        }
        const core::Vec2 off = core::annotation::labelOffsetFor(sketch(), d, vecArg(at));
        d.labelX = off.x;
        d.labelY = off.y;
    }
}

}  // namespace
}  // namespace script

// =============================================================================================
// The module
// =============================================================================================

PYBIND11_EMBEDDED_MODULE(setsquare, m)
{
    using namespace script;
    m.doc() = "Scripting API for Setsquare. Everything the UI does can be done from here.";

    py::register_exception_translator([](std::exception_ptr p) {
        try {
            if (p) {
                std::rethrow_exception(p);
            }
        }
        catch (const core::SketchError& e) {
            PyErr_SetString(PyExc_ValueError, e.what());
        }
    });

    // --- gradients ---------------------------------------------------------------------------

    auto stopsOut = [](const core::Gradient& g) {
        py::list out;
        for (const core::GradientStop& s : g.stops) {
            out.append(py::make_tuple(s.offset, s.color.hex()));
        }
        return out;
    };
    py::class_<core::Gradient>(m, "Gradient",
                               "A gradient, for any colour of a fill, stroke or text: made by setsquare.linear or "
                               "setsquare.radial, placed by sketch geometry (it follows it).")
        .def_property_readonly("type", [](const core::Gradient& g) { return std::string(core::toString(g.kind)); },
                               "'linear' or 'radial'.")
        .def_property_readonly("stops", stopsOut, "[(offset, '#rrggbb'), ...]")
        .def_property_readonly("spread", [](const core::Gradient& g) { return std::string(core::toString(g.spread)); },
                               "Past the ends: 'pad', 'reflect' or 'repeat'.")
        .def_property_readonly("start", [](const core::Gradient& g) { return g.start ? wrap(g.start) : py::none(); },
                               "Linear: the point where it starts.")
        .def_property_readonly("end", [](const core::Gradient& g) { return g.end ? wrap(g.end) : py::none(); },
                               "Linear: the point where it ends.")
        .def_property_readonly("shape", [](const core::Gradient& g) { return g.shape ? wrap(g.shape) : py::none(); },
                               "Radial: the circle, arc, ellipse or elliptical arc where its last colour is.")
        .def_property_readonly("focal", [](const core::Gradient& g) { return g.focal ? wrap(g.focal) : py::none(); },
                               "Radial: the point where its first colour is (None: the shape's centre).")
        .def("color_at", [](const core::Gradient& g, double offset) { return g.colorAt(offset).hex(); },
             py::arg("offset"), "The colour at an offset along it.")
        .def(py::self == py::self)
        .def("__repr__", [](const core::Gradient& g) { return Editor::gradientCall(sketch(), g).toStdString(); });

    // --- hatches -----------------------------------------------------------------------------

    py::class_<core::Hatch>(m, "Hatch",
                            "A hatch for a fill: lines in a pattern instead of a solid colour (made by setsquare.hatch).")
        .def_property_readonly("pattern", [](const core::Hatch& h) { return h.pattern; }, "Its pattern's name.")
        .def_property_readonly("lines", [](const core::Hatch& h) {
            py::list out;
            for (const core::HatchLine& l : h.lines) {
                out.append(py::make_tuple(l.angle, l.x, l.y, l.dx, l.dy, l.dashes));
            }
            return out;
        }, "The pattern, at scale 1: [(angle, x, y, dx, dy, [dashes]), ...] (see setsquare.hatch).")
        .def_property_readonly("angle", [](const core::Hatch& h) { return h.angle; }, "Degrees, the pattern turned.")
        .def_property_readonly("scale", [](const core::Hatch& h) { return h.scale; })
        .def_property_readonly("width", [](const core::Hatch& h) { return h.width; }, "Its lines' width, mm (0: hairlines).")
        .def(py::self == py::self)
        .def("__repr__", [](const core::Hatch& h) { return Editor::hatchCall(h).toStdString(); });
    m.def("hatch", [](const std::string& pattern, py::object lines, double angle, double scale, double width) {
        core::Hatch h;
        if (lines.is_none()) {
            const auto known = core::hatch::builtin(pattern, angle, scale, width);
            if (!known) {
                throw py::value_error("no pattern called '" + pattern + "' (see setsquare.hatch_patterns()); give lines= for one of your own");
            }
            h = *known;
        }
        else {
            h.pattern = pattern;
            h.angle = angle;
            h.scale = scale;
            h.width = width;
            for (py::handle item : lines.cast<py::sequence>()) {
                const auto seq = item.cast<py::sequence>();
                if (py::len(seq) < 5) {
                    throw py::value_error("a hatch line is (angle, x, y, dx, dy[, [dashes]])");
                }
                core::HatchLine l{seq[0].cast<double>(), seq[1].cast<double>(), seq[2].cast<double>(),
                                  seq[3].cast<double>(), seq[4].cast<double>(), {}};
                if (py::len(seq) > 5) {
                    l.dashes = seq[5].cast<std::vector<double>>();
                }
                if (std::abs(l.dy) < 1e-9) {
                    throw py::value_error("a hatch line's dy (the gap to the next line) can't be 0");
                }
                h.lines.push_back(std::move(l));
            }
            if (h.lines.empty()) {
                throw py::value_error("a hatch needs at least one line");
            }
        }
        if (!(scale > 0) || width < 0) {
            throw py::value_error("a hatch's scale must be more than 0, and its width 0 or more");
        }
        return h;
    }, py::arg("pattern"), py::arg("lines") = py::none(), py::arg("angle") = 0.0, py::arg("scale") = 1.0,
       py::arg("width") = 0.0,
       "A hatch for Sketch.fill(hatch=) or Fill.hatch: a built-in pattern by name (see hatch_patterns()), or one "
       "of your own named `pattern` with lines=[(angle, x, y, dx, dy, [dashes]), ...] as in AutoCAD .pat files "
       "(mm: lines at `angle` degrees, one through (x, y), each next shifted dx along and dy across; dashes "
       "positive, gaps negative, 0 a dot). angle turns it, scale sizes it, width is its lines' (mm; 0 hairlines).");
    m.def("hatch_patterns", [] {
        py::list out;
        for (const core::hatch::Pattern& p : core::hatch::builtins()) {
            out.append(py::make_tuple(p.name, p.description));
        }
        return out;
    }, "The built-in hatch patterns: [(name, description), ...].");

    // The geometry a gradient is placed by: items of the sketch.
    auto gradientPoint = [](py::handle h, const char* what) {
        const Id id = idOf(h);
        if (!core::gradient::canBePoint(sketch(), id)) {
            throw py::value_error(std::string(what) + ": a point of the sketch");
        }
        return id;
    };
    m.def("linear", [gradientPoint](py::handle stops, py::handle start, py::handle end, const std::string& spread) {
        core::Gradient g;
        g.kind = core::GradientKind::Linear;
        g.stops = stopsArg(stops);
        g.sortStops();
        g.start = gradientPoint(start, "start");
        g.end = gradientPoint(end, "end");
        if (g.start == g.end) {
            throw py::value_error("start and end must be different points");
        }
        g.spread = spreadArg(spread);
        return g;
    }, py::arg("stops"), py::arg("start"), py::arg("end"), py::arg("spread") = "pad",
       "A linear gradient for color= (fills, strokes, text), running from the point `start` to the point `end` "
       "(points of the sketch: it follows them as they move). stops: colours spread evenly, or (offset, colour) "
       "pairs with offsets 0..1. spread: past the ends, 'pad' (the end colours carry on), 'reflect' or 'repeat'.");
    m.def("radial", [gradientPoint](py::handle stops, py::handle shape, py::object focal, const std::string& spread) {
        core::Gradient g;
        g.kind = core::GradientKind::Radial;
        g.stops = stopsArg(stops);
        g.sortStops();
        g.shape = idOf(shape);
        if (!core::gradient::canBeShape(sketch(), g.shape)) {
            throw py::value_error("shape: a circle, arc, ellipse or elliptical arc of the sketch");
        }
        if (!focal.is_none()) {
            g.focal = gradientPoint(focal, "focal");
        }
        g.spread = spreadArg(spread);
        return g;
    }, py::arg("stops"), py::arg("shape"), py::arg("focal") = py::none(), py::arg("spread") = "pad",
       "A radial gradient for color= (fills, strokes, text): its first stop at the centre of `shape` (a circle, "
       "arc, ellipse or elliptical arc of the sketch; an ellipse makes it elliptical), its last on the shape, and "
       "it follows the shape as it moves and resizes. focal: a point to put the first colour at instead of the "
       "centre (a highlight to one side). spread: past the shape, 'pad', 'reflect' or 'repeat'.");
    // --- items -------------------------------------------------------------------------------

    py::class_<Item>(m, "Item", "Base of all sketch items.")
        .def_property_readonly("id", [](const Item& i) { return i.id; })
        .def_property_readonly("name", [](const Item& i) { return itemName(i.id); },
                               "The name used in recorded scripts, e.g. 'l7'.")
        .def_property_readonly("exists", [](const Item& i) { return isItem(i.id); })
        .def_property_readonly("sublayer", [](const Item& i) -> py::object {
            const Id l = sketch().sublayerOf(i.id);
            return l != kNoId ? py::cast(SublayerH{l}) : py::none();
        }, "The sublayer it's in (None for the origin, axes and fills).")
        .def_property_readonly("layer", [](const Item& i) -> py::object {
            const Id l = sketch().layerOf(i.id);
            return l != kNoId ? py::cast(LayerH{l}) : py::none();
        }, "The layer it's in (None for the origin and axes, which are in every layer).")
        .def_property_readonly("projected", [](const Item& i) { return sketch().isProjection(i.id); },
                               "Whether it's a projection of geometry from another layer.")
        .def_property_readonly("linked", [](const Item& i) { return sketch().isLinked(i.id); },
                               "A projection whose original is still there (and which it follows).")
        .def_property_readonly("source", [](const Item& i) -> py::object {
            if (!sketch().isLinked(i.id)) {
                return py::none();
            }
            return wrapAll({sketch().entity(i.id).projectedFrom})[0];
        }, "A projection's original (None if it isn't one, or the original is gone).")
        .def("__eq__", [](const Item& a, py::handle b) { return py::isinstance<Item>(b) && a.id == b.cast<Item&>().id; })
        .def("__hash__", [](const Item& i) { return py::hash(py::int_(i.id)); });

    py::class_<SublayerH>(m, "Sublayer",
                          "A part of the drawing edited on its own (see Sketch.sublayers). Set name/visible to change.")
        .def_property(
            "name", [](const SublayerH& l) { return sketch().sublayer(l.id).name; },
            [](const SublayerH& l, const std::string& name) {
                const Id id = sublayerArg(py::cast(l));
                editReturningId("Rename sublayer", [&](Editor& ed) {
                    ed.renameSublayer(id, QString::fromStdString(name));
                    return id;
                });
            })
        .def_property(
            "visible", [](const SublayerH& l) { return sketch().sublayer(l.id).visible; },
            [](const SublayerH& l, bool visible) {
                Call call;
                call.target().setSublayerVisible(sublayerArg(py::cast(l)), visible);
            })
        .def_property_readonly("active", [](const SublayerH& l) { return sketch().activeSublayer() == l.id; })
        .def_property_readonly("items", [](const SublayerH& l) {
            std::vector<Id> ids;
            for (const auto& [id, e] : sketch().entities()) {
                if (e.sublayer == l.id) {
                    ids.push_back(id);
                }
            }
            for (const auto& [id, c] : sketch().constraints()) {
                if (c.sublayer == l.id) {
                    ids.push_back(id);
                }
            }
            for (const auto& [id, t] : sketch().texts()) {
                if (t.sublayer == l.id) {
                    ids.push_back(id);
                }
            }
            for (const auto& [id, d] : sketch().drawnDimensions()) {
                if (d.sublayer == l.id) {
                    ids.push_back(id);
                }
            }
            return wrapAll(ids);
        }, "Its geometry, constraints, texts and annotations.")
        .def_property_readonly("annotations", [](const SublayerH& l) { return sketch().sublayer(l.id).annotations; },
                               "Whether it's its layer's own for annotations (where new ones go).")
        .def("__eq__", [](const SublayerH& a, py::handle b) {
            return py::isinstance<SublayerH>(b) && a.id == b.cast<SublayerH&>().id;
        })
        .def("__hash__", [](const SublayerH& l) { return py::hash(py::int_(l.id)); })
        .def("__repr__", [](const SublayerH& l) {
            if (!sketch().hasSublayer(l.id)) {
                return std::string("<Sublayer (deleted)>");
            }
            const core::Sublayer& s = sketch().sublayer(l.id);
            return "<Sublayer \"" + s.name + "\"" + (sketch().activeSublayer() == l.id ? " active" : "")
                + (s.visible ? "" : " hidden") + ">";
        })
        .def_property_readonly("layer", [](const SublayerH& l) { return LayerH{sketch().sublayer(l.id).layer}; });

    py::class_<LayerH>(m, "Layer",
                       "A sketch of its own, drawn over the layers after it in Sketch.layers (the first is on top). "
                       "Its geometry can't refer to other layers' except through projections (Sketch.project). "
                       "Set name/visible to change.")
        .def_property(
            "name", [](const LayerH& l) { return sketch().layer(l.id).name; },
            [](const LayerH& l, const std::string& name) {
                const Id id = layerArg(py::cast(l));
                editReturningId("Rename layer", [&](Editor& ed) {
                    ed.renameLayer(id, QString::fromStdString(name));
                    return id;
                });
            })
        .def_property(
            "visible", [](const LayerH& l) { return sketch().layer(l.id).visible; },
            [](const LayerH& l, bool visible) {
                Call call;
                call.target().setLayerVisible(layerArg(py::cast(l)), visible);
            })
        .def_property_readonly("active", [](const LayerH& l) { return sketch().activeLayer() == l.id; })
        .def_property_readonly("sublayers", [](const LayerH& l) {
            py::list out;
            for (Id id : sketch().sublayersOf(l.id)) {
                out.append(SublayerH{id});
            }
            return out;
        })
        .def_property_readonly("fills", [](const LayerH& l) {
            std::vector<Id> ids;
            for (const auto& [id, f] : sketch().fills()) {
                if (f.layer == l.id) {
                    ids.push_back(id);
                }
            }
            return wrapAll(ids);
        })
        .def("__eq__", [](const LayerH& a, py::handle b) { return py::isinstance<LayerH>(b) && a.id == b.cast<LayerH&>().id; })
        .def("__hash__", [](const LayerH& l) { return py::hash(py::int_(l.id)); })
        .def("__repr__", [](const LayerH& l) {
            if (!sketch().hasLayer(l.id)) {
                return std::string("<Layer (deleted)>");
            }
            const core::Layer& s = sketch().layer(l.id);
            return "<Layer \"" + s.name + "\"" + (sketch().activeLayer() == l.id ? " active" : "")
                + (s.visible ? "" : " hidden") + ">";
        });

    auto entityProps = [](auto& cls) {
        using T = typename std::remove_reference_t<decltype(cls)>::type;
        cls.def_property(
               "fixed", [](const T& i) { return entity(i.id).fixed; },
               [](const T& i, bool fixed) {
                   editReturningId("Fix", [&](Editor& ed) {
                       ed.setFixed(i.id, fixed);
                       return i.id;
                   });
               },
               "Locked in place (the origin is always fixed).")
            .def_property(
                "construction", [](const T& i) { return entity(i.id).construction; },
                [](const T& i, bool on) {
                    editReturningId("Construction", [&](Editor& ed) {
                        ed.setConstruction(i.id, on);
                        return i.id;
                    });
                },
                "Reference geometry, drawn dashed.")
            .def_property_readonly("fully_constrained", [](const T& i) {
                entity(i.id);
                return doc().solveResult().fullyConstrained.contains(i.id);
            })
            .def_property_readonly("stroke", [](const T& i) -> py::object {
                entity(i.id);
                const core::StrokeStyle* style = sketch().stroke(i.id);
                if (!style) {
                    return py::none();
                }
                py::dict d;
                d["color"] = style->color.hex();
                d["width"] = style->none() ? py::object(py::str("none")) : py::object(py::float_(style->width));
                d["dash"] = std::string(core::toString(style->dash));
                if (style->gradient.active()) {
                    d["gradient"] = style->gradient;
                }
                return d;
            }, "The curve's stroke as {'color', 'width', 'dash'}, or None (see Sketch.stroke); with a gradient "
               "colouring the line instead of color, also 'gradient' (a setsquare.Gradient).");
    };

    py::class_<PointH, Item> point(m, "Point");
    point.def_property_readonly("x", [](const PointH& p) { entity(p.id); return sketch().point(p.id).x; })
        .def_property_readonly("y", [](const PointH& p) { entity(p.id); return sketch().point(p.id).y; })
        .def_property_readonly("pos", [](const PointH& p) {
            entity(p.id);
            const core::Point& q = sketch().point(p.id);
            return py::make_tuple(q.x, q.y);
        })
        .def("move_to", [](const PointH& p, double x, double y) {
            entity(p.id);
            const core::Point q = sketch().point(p.id);
            dragPoints({p.id}, x - q.x, y - q.y);
        }, py::arg("x"), py::arg("y"), "Drag the point towards (x, y); constraints still hold.")
        .def("__repr__", [](const PointH& p) {
            if (!sketch().hasEntity(p.id)) {
                return "<Point " + std::to_string(p.id) + " (deleted)>";
            }
            const core::Point& q = sketch().point(p.id);
            return "<Point " + itemName(p.id) + " (" + fmt(q.x) + ", " + fmt(q.y) + ")>";
        });
    entityProps(point);

    py::class_<LineH, Item> line(m, "Line");
    line.def_property_readonly("p1", [](const LineH& l) { entity(l.id); return wrap(sketch().line(l.id).p1); })
        .def_property_readonly("p2", [](const LineH& l) { entity(l.id); return wrap(sketch().line(l.id).p2); })
        .def_property_readonly("length", [](const LineH& l) {
            entity(l.id);
            return core::measure(sketch(), ConstraintType::Distance, {l.id});
        })
        .def("__repr__", [](const LineH& l) {
            if (!sketch().hasEntity(l.id)) {
                return "<Line " + std::to_string(l.id) + " (deleted)>";
            }
            const core::Line& ln = sketch().line(l.id);
            const core::Point& a = sketch().point(ln.p1);
            const core::Point& b = sketch().point(ln.p2);
            return "<Line " + itemName(l.id) + " (" + fmt(a.x) + ", " + fmt(a.y) + ")-(" + fmt(b.x) + ", " + fmt(b.y)
                + ")>";
        });
    entityProps(line);

    py::class_<CircleH, Item> circle(m, "Circle");
    circle.def_property_readonly("center", [](const CircleH& c) { entity(c.id); return wrap(sketch().circle(c.id).center); })
        .def_property_readonly("radius", [](const CircleH& c) { entity(c.id); return sketch().circle(c.id).radius; })
        .def("__repr__", [](const CircleH& c) {
            if (!sketch().hasEntity(c.id)) {
                return "<Circle " + std::to_string(c.id) + " (deleted)>";
            }
            return "<Circle " + itemName(c.id) + " r=" + fmt(sketch().circle(c.id).radius) + ">";
        });
    entityProps(circle);

    py::class_<ArcH, Item> arc(m, "Arc");
    arc.def_property_readonly("center", [](const ArcH& a) { entity(a.id); return wrap(sketch().arc(a.id).center); })
        .def_property_readonly("start", [](const ArcH& a) { entity(a.id); return wrap(sketch().arc(a.id).start); })
        .def_property_readonly("end", [](const ArcH& a) { entity(a.id); return wrap(sketch().arc(a.id).end); })
        .def_property_readonly("radius", [](const ArcH& a) { entity(a.id); return sketch().arcRadius(a.id); })
        .def("__repr__", [](const ArcH& a) {
            if (!sketch().hasEntity(a.id)) {
                return "<Arc " + std::to_string(a.id) + " (deleted)>";
            }
            return "<Arc " + itemName(a.id) + " r=" + fmt(sketch().arcRadius(a.id)) + ">";
        });
    entityProps(arc);

    // Ellipses: the centre and the ends of the two semi-axes (points, which can be dragged and
    // constrained); the axes are kept at right angles.
    auto ellipseProps = [&entityProps](auto& cls, const char* what) {
        using T = typename std::remove_reference_t<decltype(cls)>::type;
        cls.def_property_readonly("center", [](const T& e) { entity(e.id); return wrap(sketch().ellipseAxes(e.id)[0]); })
            .def_property_readonly("major", [](const T& e) { entity(e.id); return wrap(sketch().ellipseAxes(e.id)[1]); },
                                   "The end of the first semi-axis (it sets the direction).")
            .def_property_readonly("minor", [](const T& e) { entity(e.id); return wrap(sketch().ellipseAxes(e.id)[2]); },
                                   "The end of the other semi-axis.")
            .def_property_readonly("major_radius", [](const T& e) { entity(e.id); return core::curve::ellipseFrame(sketch(), e.id).a; })
            .def_property_readonly("minor_radius", [](const T& e) { entity(e.id); return core::curve::ellipseFrame(sketch(), e.id).b; })
            .def_property_readonly("angle", [](const T& e) {
                entity(e.id);
                const core::curve::EllipseFrame f = core::curve::ellipseFrame(sketch(), e.id);
                return std::atan2(f.u.y, f.u.x) * 180.0 / std::numbers::pi;
            }, "Of the major axis, in degrees from +x.")
            .def("__repr__", [what](const T& e) {
                if (!sketch().hasEntity(e.id)) {
                    return std::string("<") + what + " " + std::to_string(e.id) + " (deleted)>";
                }
                const core::curve::EllipseFrame f = core::curve::ellipseFrame(sketch(), e.id);
                return std::string("<") + what + " " + itemName(e.id) + " " + fmt(f.a) + " x " + fmt(f.b) + ">";
            });
        entityProps(cls);
    };
    py::class_<EllipseH, Item> ellipse(m, "Ellipse");
    ellipseProps(ellipse, "Ellipse");
    py::class_<EllipseArcH, Item> ellipseArc(m, "EllipseArc");
    ellipseArc.def_property_readonly("start", [](const EllipseArcH& a) { entity(a.id); return wrap(sketch().ellipseArc(a.id).start); })
        .def_property_readonly("end", [](const EllipseArcH& a) { entity(a.id); return wrap(sketch().ellipseArc(a.id).end); });
    ellipseProps(ellipseArc, "EllipseArc");

    py::class_<SplineH, Item> spline(m, "Spline");
    spline.def_property_readonly("points", [](const SplineH& sp) { entity(sp.id); return wrapAll(sketch().spline(sp.id).points); },
                                 "Its points: on the curve (fit) or pulling it (control).")
        .def_property_readonly("kind", [](const SplineH& sp) { entity(sp.id); return std::string(core::toString(sketch().spline(sp.id).kind)); })
        .def_property_readonly("closed", [](const SplineH& sp) { entity(sp.id); return sketch().spline(sp.id).closed; })
        .def_property_readonly("start", [](const SplineH& sp) -> py::object {
            entity(sp.id);
            const core::Spline& s = sketch().spline(sp.id);
            return s.isPiece() ? wrap(s.start) : py::none();
        }, "A trimmed piece's first end (None for a whole spline).")
        .def_property_readonly("end", [](const SplineH& sp) -> py::object {
            entity(sp.id);
            const core::Spline& s = sketch().spline(sp.id);
            return s.isPiece() ? wrap(s.end) : py::none();
        })
        .def_property_readonly("length", [](const SplineH& sp) { entity(sp.id); return core::curve::pathOf(sketch(), sp.id).length(); })
        .def("__repr__", [](const SplineH& sp) {
            if (!sketch().hasEntity(sp.id)) {
                return "<Spline " + std::to_string(sp.id) + " (deleted)>";
            }
            const core::Spline& s = sketch().spline(sp.id);
            return "<Spline " + itemName(sp.id) + " " + core::toString(s.kind) + ", " + std::to_string(s.points.size())
                + " points" + (s.closed ? ", closed" : "") + (s.isPiece() ? ", piece" : "") + ">";
        });
    entityProps(spline);

    py::class_<ConstraintH, Item>(m, "Constraint")
        .def_property_readonly("type", [](const ConstraintH& c) {
            return Editor::function(constraintOf(c.id).type).toStdString();
        })
        .def_property_readonly("refs", [](const ConstraintH& c) { return wrapAll(constraintOf(c.id).refs); })
        .def_property_readonly("is_dimension", [](const ConstraintH& c) { return core::isDimension(constraintOf(c.id).type); })
        .def_property_readonly("is_feature", [](const ConstraintH& c) { return core::isFeature(constraintOf(c.id).type); },
                               "A pattern, offset or divide.")
        .def_property_readonly("sources", [](const ConstraintH& c) {
            const core::Constraint& k = constraintOf(c.id);
            if (k.type == ConstraintType::Offset) {
                return wrapAll(core::feature::offsetRefs(k).sources);
            }
            if (k.type == ConstraintType::Divide) {
                return wrapAll(std::vector<Id>{k.refs.front()});
            }
            if (core::isFeature(k.type)) {
                return wrapAll(core::feature::pattern(k).sources);
            }
            throw py::value_error("not a pattern or offset");
        }, "A pattern's or offset's original items.")
        .def_property_readonly("copies", [](const ConstraintH& c) {
            const core::Constraint& k = constraintOf(c.id);
            py::list out;
            if (k.type == ConstraintType::Offset) {
                const auto refs = core::feature::offsetRefs(k);
                out.append(wrapAll(refs.offsets));
                if (!refs.offsets2.empty()) {
                    out.append(wrapAll(refs.offsets2));
                }
            }
            else if (k.type == ConstraintType::Divide) {
                out.append(wrapAll(std::vector<Id>(k.refs.begin() + 1, k.refs.end())));
            }
            else if (core::isFeature(k.type)) {
                for (const auto& instance : core::feature::pattern(k).copies) {
                    out.append(wrapAll(instance));
                }
            }
            else {
                throw py::value_error("not a pattern or offset");
            }
            return out;
        }, "Per copy (one per side for an offset), the items matching `sources`.")
        .def_property(
            "value", [](const ConstraintH& c) -> py::object {
                const core::Constraint& k = constraintOf(c.id);
                return core::hasValue(k.type) ? py::cast(k.value) : py::none();
            },
            [](const ConstraintH& c, py::handle v) {
                constraintOf(c.id);
                Call call;
                QString error;
                const bool done = py::isinstance<py::str>(v)
                    ? call.target().setDimensionExpression(c.id, QString::fromStdString(v.cast<std::string>()), &error)
                    : call.target().setDimensionValue(c.id, v.cast<double>(), &error);
                if (!done) {
                    throw py::value_error(error.toStdString());
                }
            },
            "A dimension's value (lengths, or degrees for angles), a pattern's spacing or total angle, or an "
            "offset's distance. Setting it re-solves. A string using the drawing's parameters (\"width / 2\") "
            "makes it follow them (see s.parameter); a number makes it a plain value again.")
        .def_property_readonly(
            "expression", [](const ConstraintH& c) -> py::object {
                const core::Constraint& k = constraintOf(c.id);
                return k.expression.empty() ? py::none() : py::cast(k.expression);
            },
            "What a dimension is worked out from (\"width / 2\"), or None for a plain value.")
        .def_property(
            "label", [](const ConstraintH& c) {
                const core::Constraint& k = constraintOf(c.id);
                return py::make_tuple(k.labelX, k.labelY);
            },
            [](const ConstraintH& c, py::handle xy) {
                constraintOf(c.id);
                const auto [x, y] = pairOf(xy, "label");
                Call call;
                call.target().beginLabelDrag();
                call.target().moveLabel(c.id, x, y);
                call.target().endLabelDrag();
            },
            "Dimension label position, as an offset from the dimension's anchor.")
        .def("__repr__", [](const ConstraintH& c) {
            if (!sketch().hasConstraint(c.id)) {
                return "<Constraint " + std::to_string(c.id) + " (deleted)>";
            }
            return "<" + Editor::constraintCall(sketch(), c.id).section(QStringLiteral(" = "), 1).toStdString() + ">";
        });

    auto fillOf = [](Id id) -> const core::Fill& {
        if (!sketch().hasFill(id)) {
            throw py::value_error("fill " + std::to_string(id) + " no longer exists");
        }
        return sketch().fill(id);
    };
    py::class_<FillH, Item>(m, "Fill", "A colour filling a closed area (see Sketch.fill).")
        .def_property(
            "color", [fillOf](const FillH& f) { return fillOf(f.id).color.hex(); },
            [fillOf](const FillH& f, py::handle color) {
                const core::Fill& current = fillOf(f.id);
                const core::Point seed = current.seed;
                const PaintArg p = paintArg(color);
                editReturningId("Fill", [&](Editor& ed) { return ed.fill({seed.x, seed.y}, p.color, p.gradient); });
            },
            "'#rrggbb' (or '#rrggbbaa'); set it to recolour (or to a setsquare.linear / setsquare.radial gradient).")
        .def_property(
            "gradient", [fillOf](const FillH& f) -> py::object {
                const core::Fill& fill = fillOf(f.id);
                return fill.gradient.active() ? py::cast(fill.gradient) : py::none();
            },
            [fillOf](const FillH& f, py::object gradient) {
                fillOf(f.id);
                core::Gradient g;
                if (!gradient.is_none()) {
                    g = paintArg(gradient).gradient;
                    if (!g.active()) {
                        throw py::type_error("gradient: a setsquare.linear / setsquare.radial gradient, or None");
                    }
                }
                editReturningId("Gradient", [&](Editor& ed) {
                    ed.setFillGradient(f.id, g);
                    return f.id;
                });
            },
            "Its gradient (setsquare.Gradient), or None for its plain colour. Set it to change; None takes it off.")
        .def_property(
            "hatch", [fillOf](const FillH& f) -> py::object {
                const core::Fill& fill = fillOf(f.id);
                return fill.hatch.active() ? py::cast(fill.hatch) : py::none();
            },
            [fillOf](const FillH& f, py::object hatch) {
                fillOf(f.id);
                const core::Hatch h = hatch.is_none() ? core::Hatch{} : hatch.cast<core::Hatch>();
                editReturningId("Hatch", [&](Editor& ed) {
                    ed.setFillHatch(f.id, h);
                    return f.id;
                });
            },
            "Its hatch (setsquare.Hatch: the area drawn with lines in its colour), or None (filled solid). Set it to "
            "change; None takes it off.")
        .def_property_readonly("at", [fillOf](const FillH& f) {
            const core::Fill& fill = fillOf(f.id);
            return py::make_tuple(fill.seed.x, fill.seed.y);
        }, "A point inside the area.")
        .def_property_readonly("boundary", [fillOf](const FillH& f) {
            const auto region = core::regions::resolve(core::regions::Arrangements(sketch()), fillOf(f.id));
            return wrapAll(region ? region->boundary : std::vector<Id>{});
        }, "The curves around the area now (empty if its outline is open).")
        .def_property_readonly("broken", [fillOf](const FillH& f) {
            return !core::regions::resolve(core::regions::Arrangements(sketch()), fillOf(f.id)).has_value();
        }, "True while its outline is open (it shows again once the outline is closed).")
        .def_property_readonly("area", [fillOf](const FillH& f) {
            const auto region = core::regions::resolve(core::regions::Arrangements(sketch()), fillOf(f.id));
            if (!region) {
                return 0.0;
            }
            double a = region->area;
            for (const auto& hole : region->holes) {
                double h = 0.0;  // (holes run clockwise)
                for (size_t i = 0, j = hole.size() - 1; i < hole.size(); j = i++) {
                    h += hole[j].x * hole[i].y - hole[i].x * hole[j].y;
                }
                a -= std::abs(h) / 2.0;
            }
            return a;
        }, "The filled area (holes taken off).")
        .def("__repr__", [](const FillH& f) {
            if (!sketch().hasFill(f.id)) {
                return "<Fill " + std::to_string(f.id) + " (deleted)>";
            }
            const core::Fill& fill = sketch().fill(f.id);
            return "<Fill f" + std::to_string(f.id) + " "
                   + (fill.gradient.active() ? std::string(core::toString(fill.gradient.kind)) + " gradient"
                      : fill.hatch.active()  ? fill.color.hex() + " " + fill.hatch.pattern + " hatch"
                                             : fill.color.hex())
                   + ">";
        });

    auto imageOf = [](Id id) -> const core::Image& {
        if (!sketch().hasImage(id)) {
            throw py::value_error("image " + std::to_string(id) + " no longer exists");
        }
        return sketch().image(id);
    };
    py::class_<ImageH, Item>(m, "Image",
                             "A reference image to draw over (see Sketch.image): shown under its layer's geometry, "
                             "never part of the drawing (not in presentation view or exports). Change it with "
                             "Sketch.edit_image or calibrate().")
        .def_property_readonly("source", [imageOf](const ImageH& i) { return imageOf(i.id).source; },
                               "The file it was inserted from (it's kept in the drawing).")
        .def_property_readonly("center", [imageOf](const ImageH& i) {
            const core::Image& im = imageOf(i.id);
            return py::make_tuple(im.x, im.y);
        })
        .def_property_readonly("width", [imageOf](const ImageH& i) { return imageOf(i.id).width; }, "mm")
        .def_property_readonly("height", [imageOf](const ImageH& i) { return imageOf(i.id).height(); }, "mm")
        .def_property_readonly("angle", [imageOf](const ImageH& i) { return imageOf(i.id).angle; }, "Degrees.")
        .def_property_readonly("opacity", [imageOf](const ImageH& i) { return imageOf(i.id).opacity; }, "0..1")
        .def_property_readonly("flip_h", [imageOf](const ImageH& i) { return imageOf(i.id).flipH; })
        .def_property_readonly("flip_v", [imageOf](const ImageH& i) { return imageOf(i.id).flipV; })
        .def_property_readonly("visible", [imageOf](const ImageH& i) { return imageOf(i.id).visible; })
        .def_property_readonly("pixels", [imageOf](const ImageH& i) {
            const core::Image& im = imageOf(i.id);
            return py::make_tuple(im.pixelWidth, im.pixelHeight);
        }, "Its size in pixels.")
        .def_property_readonly("corners", [imageOf](const ImageH& i) {
            const core::Image& im = imageOf(i.id);
            py::list out;
            for (const auto& [u, v] : {std::pair{0.0, 0.0}, std::pair{1.0, 0.0}, std::pair{1.0, 1.0}, std::pair{0.0, 1.0}}) {
                const core::Point p = im.at(u, v);
                out.append(py::make_tuple(p.x, p.y));
            }
            return out;
        }, "Its corners in sketch coordinates, from the bottom-left (as shown) counter-clockwise.")
        .def("at", [imageOf](const ImageH& i, double u, double v) {
            const core::Point p = imageOf(i.id).at(u, v);
            return py::make_tuple(p.x, p.y);
        }, py::arg("u"), py::arg("v"), "A place on it, (0, 0) its bottom-left and (1, 1) its top-right, in sketch "
                                       "coordinates.")
        .def("calibrate", [imageOf](const ImageH& i, py::handle a, py::handle b, double distance) {
            core::Image im = imageOf(i.id);
            const auto [ax, ay] = pairOf(a, "a");
            const auto [bx, by] = pairOf(b, "b");
            const double now = std::hypot(bx - ax, by - ay);
            if (!(now > 0.0) || !(distance > 0.0)) {
                throw py::value_error("calibrate: two different points and a distance more than 0");
            }
            const double k = distance / now;
            im.width *= k;
            im.x = ax + (im.x - ax) * k;
            im.y = ay + (im.y - ay) * k;
            editReturningId("Image", [&](Editor& ed) {
                ed.setImage(im);
                return i.id;
            });
        }, py::arg("a"), py::arg("b"), py::arg("distance"),
           "Calibrate, as in Fusion: scale it (about `a`, which stays put) so that the points a and b (sketch "
           "coordinates, e.g. two marks on it a known distance apart) are `distance` mm apart.")
        .def("__repr__", [](const ImageH& i) {
            if (!sketch().hasImage(i.id)) {
                return "<Image " + std::to_string(i.id) + " (deleted)>";
            }
            const core::Image& im = sketch().image(i.id);
            return "<Image i" + std::to_string(i.id) + " " + QFileInfo(QString::fromStdString(im.source)).fileName().toStdString()
                   + ">";
        });

    auto annotationOf = [](Id id) -> const core::DrawnDimension& {
        if (!sketch().hasDrawnDimension(id)) {
            throw py::value_error("annotation " + std::to_string(id) + " no longer exists");
        }
        return sketch().drawnDimension(id);
    };
    py::class_<AnnotationH, Item>(m, "Annotation",
                                  "A dimension or note drawn on the drawing (see Sketch.annotate): it measures the "
                                  "geometry (never drives it) and is shown and exported with the drawing. Change it "
                                  "with Sketch.edit_annotation.")
        .def_property_readonly("kind", [annotationOf](const AnnotationH& a) {
            return std::string(core::toString(annotationOf(a.id).kind));
        })
        .def_property_readonly("refs", [annotationOf](const AnnotationH& a) { return wrapAll(annotationOf(a.id).refs); },
                               "What it measures (or points at).")
        .def_property_readonly("value", [annotationOf](const AnnotationH& a) -> py::object {
            const core::DrawnDimension& d = annotationOf(a.id);
            if (d.kind == core::DrawnDimensionKind::Ordinate) {
                const core::Vec2 c = core::annotation::coordinates(sketch(), d);
                return py::make_tuple(c.x, c.y);
            }
            return py::float_(core::annotation::value(sketch(), d));
        }, "What it measures now: mm, or degrees (an ordinate: (x, y) from its origin; 0 for a leader).")
        .def_property_readonly("label", [annotationOf](const AnnotationH& a) {
            return core::annotation::label(sketch(), annotationOf(a.id));
        }, "What it says, its <> filled in.")
        .def_property_readonly("at", [annotationOf](const AnnotationH& a) {
            const core::annotation::Graphics g = core::annotation::graphics(sketch(), annotationOf(a.id));
            return py::make_tuple(g.label.x, g.label.y);
        }, "Where its label is.")
        .def_property_readonly("text", [annotationOf](const AnnotationH& a) { return annotationOf(a.id).text; },
                               "Its text, <> standing for the value.")
        .def_property_readonly("decimals", [annotationOf](const AnnotationH& a) -> py::object {
            const int d = annotationOf(a.id).decimals;
            return d < 0 ? py::none() : py::object(py::int_(d));
        }, "Places after the point; None: the drawing's (Sketch.dimension_decimals).")
        .def_property_readonly("text_height", [annotationOf](const AnnotationH& a) { return annotationOf(a.id).textHeight; },
                               "Capital letter height, mm.")
        .def_property_readonly("width", [annotationOf](const AnnotationH& a) { return annotationOf(a.id).style.width; },
                               "Line width, mm.")
        .def_property_readonly("color", [annotationOf](const AnnotationH& a) { return annotationOf(a.id).style.color.hex(); })
        .def_property_readonly("font", [annotationOf](const AnnotationH& a) { return annotationOf(a.id).font; })
        .def("__repr__", [](const AnnotationH& a) {
            if (!sketch().hasDrawnDimension(a.id)) {
                return "<Annotation " + std::to_string(a.id) + " (deleted)>";
            }
            const core::DrawnDimension& d = sketch().drawnDimension(a.id);
            return "<Annotation n" + std::to_string(a.id) + " " + core::toString(d.kind) + " '"
                   + core::annotation::label(sketch(), d) + "'>";
        });

    auto textOf = [](Id id) -> const core::Text& {
        if (!sketch().hasText(id)) {
            throw py::value_error("text " + std::to_string(id) + " no longer exists");
        }
        return sketch().text(id);
    };
    py::class_<TextH, Item>(m, "Text", "Text in the drawing (see Sketch.text); change it with Sketch.edit_text.")
        .def_property_readonly("text", [textOf](const TextH& t) { return textOf(t.id).text; })
        .def_property_readonly("font", [textOf](const TextH& t) { return textOf(t.id).font; })
        .def_property_readonly("height", [textOf](const TextH& t) { return textOf(t.id).height; },
                               "Capital letter height, mm.")
        .def_property_readonly("angle", [textOf](const TextH& t) { return textOf(t.id).angle; })
        .def_property_readonly("bold", [textOf](const TextH& t) { return textOf(t.id).bold; })
        .def_property_readonly("italic", [textOf](const TextH& t) { return textOf(t.id).italic; })
        .def_property_readonly("outline", [textOf](const TextH& t) { return std::string(core::toString(textOf(t.id).outline)); },
                               "'as_designed', 'merged' or 'silhouette'.")
        .def_property_readonly("min_thickness", [textOf](const TextH& t) { return textOf(t.id).minThickness; })
        .def_property_readonly("spacing", [textOf](const TextH& t) { return textOf(t.id).spacing; })
        .def_property_readonly("align", [textOf](const TextH& t) { return std::string(core::toString(textOf(t.id).align)); })
        .def_property_readonly("valign", [textOf](const TextH& t) { return std::string(core::toString(textOf(t.id).valign)); })
        .def_property_readonly("flip_h", [textOf](const TextH& t) { return textOf(t.id).flipH; })
        .def_property_readonly("flip_v", [textOf](const TextH& t) { return textOf(t.id).flipV; })
        .def_property_readonly("wrap", [textOf](const TextH& t) { return textOf(t.id).wrap; },
                               "In a box: lines broken between words to fit its width.")
        .def_property_readonly("grow", [textOf](const TextH& t) { return textOf(t.id).grow; },
                               "In a box, wrapping: the box as tall as the text.")
        .def_property_readonly("box", [textOf](const TextH& t) -> py::object {
            const core::Text& text = textOf(t.id);
            if (!text.inBox()) {
                return py::none();
            }
            return wrapAll(core::textPoints(sketch(), text));
        }, "A box text's box corners (from the one its text starts from), else None.")
        .def_property_readonly("path", [textOf](const TextH& t) -> py::object {
            const core::Text& text = textOf(t.id);
            return text.path != kNoId ? wrap(text.path) : py::none();
        }, "The curve it follows, if it's set along one (along a chain: its first; see path_curves).")
        .def_property_readonly("path_curves", [textOf](const TextH& t) {
            const core::Text& text = textOf(t.id);
            std::vector<Id> all;
            if (text.path != kNoId) {
                all.push_back(text.path);
                all.insert(all.end(), text.pathMore.begin(), text.pathMore.end());
            }
            return wrapAll(all);
        }, "The curves it follows, in order along them (empty if it isn't set along a path).")
        .def_property_readonly("color", [textOf](const TextH& t) -> py::object {
            const core::Text& text = textOf(t.id);
            return text.hasColor ? py::cast(text.color.hex()) : py::none();
        })
        .def_property_readonly("gradient", [textOf](const TextH& t) -> py::object {
            const core::Text& text = textOf(t.id);
            return text.hasColor && text.gradient.active() ? py::cast(text.gradient) : py::none();
        }, "Its gradient (setsquare.Gradient; set with color= in Sketch.edit_text), or None.")
        .def_property_readonly("anchor", [textOf](const TextH& t) { return wrap(textOf(t.id).anchor); },
                               "The point at the start of its baseline.")
        .def_property_readonly("contours", [textOf](const TextH& t) {
            py::list out;
            const core::Text& text = textOf(t.id);
            const core::Point& anchor = sketch().point(text.anchor);
            for (const auto& contour : text.shape) {
                py::list pts;
                for (const core::Point& p : contour) {
                    const core::Point w = core::textToWorld(anchor, text.angle, p);
                    pts.append(py::make_tuple(w.x, w.y));
                }
                out.append(pts);
            }
            return out;
        }, "The outline of its letters: closed loops of (x, y) where they are now.")
        .def_property_readonly("bounds", [textOf](const TextH& t) {
            const core::Text& text = textOf(t.id);
            const core::Point& anchor = sketch().point(text.anchor);
            double x0 = 1e300, y0 = 1e300, x1 = -1e300, y1 = -1e300;
            for (const auto& contour : text.shape) {
                for (const core::Point& p : contour) {
                    const core::Point w = core::textToWorld(anchor, text.angle, p);
                    x0 = std::min(x0, w.x), y0 = std::min(y0, w.y), x1 = std::max(x1, w.x), y1 = std::max(y1, w.y);
                }
            }
            return py::make_tuple(x0, y0, x1, y1);
        }, "(x0, y0, x1, y1) round its letters.")
        .def("__repr__", [](const TextH& t) {
            if (!sketch().hasText(t.id)) {
                return "<Text " + std::to_string(t.id) + " (deleted)>";
            }
            return "<Text t" + std::to_string(t.id) + " \"" + sketch().text(t.id).text + "\">";
        });

    // --- solve status ------------------------------------------------------------------------

    py::class_<core::SolveResult>(m, "SolveResult")
        .def_readonly("ok", &core::SolveResult::ok)
        .def_readonly("dof", &core::SolveResult::dof, "Remaining degrees of freedom.")
        .def_property_readonly("conflicting", [](const core::SolveResult& r) { return wrapAll(r.conflicting); })
        .def_property_readonly("redundant", [](const core::SolveResult& r) { return wrapAll(r.redundant); })
        .def_property_readonly("fully_constrained", [](const core::SolveResult& r) {
            return wrapAll({r.fullyConstrained.begin(), r.fullyConstrained.end()});
        })
        .def("__repr__", [](const core::SolveResult& r) {
            return "<SolveResult ok=" + std::string(r.ok ? "True" : "False") + " dof=" + std::to_string(r.dof) + ">";
        });

    // --- sketch ------------------------------------------------------------------------------

    auto constraintFn = [](ConstraintType type) {
        return [type](const SketchH&, py::args args) { return addConstraint(type, idsOf(args), py::none(), py::none()); };
    };
    // Arguments after *args are keyword-only: s.distance(l7, value=40, label=(0, 8)).
    auto dimensionFn = [](ConstraintType type) {
        return [type](const SketchH&, py::args args, py::object value, py::object label) {
            return addConstraint(type, idsOf(args), std::move(value), std::move(label));
        };
    };
    auto listOf = [](std::optional<EntityKind> kind) {
        return [kind](const SketchH&) {
            std::vector<Id> ids;
            for (const auto& [id, e] : sketch().entities()) {
                // The origin and axes are reachable as s.origin / s.x_axis / s.y_axis.
                if ((!kind || e.kind() == *kind) && !sketch().isReference(id)) {
                    ids.push_back(id);
                }
            }
            return wrapAll(ids);
        };
    };

    py::class_<SketchH>(m, "Sketch")
        .def_property_readonly("origin", [](const SketchH&) { return wrap(sketch().origin()); })
        .def_property_readonly("x_axis", [](const SketchH&) { return wrap(sketch().xAxis()); },
                               "The fixed X axis (a line), for constraints and dimensions.")
        .def_property_readonly("y_axis", [](const SketchH&) { return wrap(sketch().yAxis()); })
        .def_property_readonly("page_edges", [](const SketchH&) {
                py::list out;
                if (sketch().pageEdges()[0] != kNoId) {
                    for (Id id : sketch().pageEdges()) {
                        out.append(wrap(id));
                    }
                }
                return out;
            },
            "The page's edges (bottom, right, top, left): fixed construction lines, in every layer, for "
            "constraints and dimensions. Empty without a page.")
        .def_property_readonly("page_corners", [](const SketchH&) {
                py::list out;
                if (sketch().pageCorners()[0] != kNoId) {
                    for (Id id : sketch().pageCorners()) {
                        out.append(wrap(id));
                    }
                }
                return out;
            },
            "The page's corners (bottom left, bottom right, top right, top left). Empty without a page.")
        .def("__getitem__", [](const SketchH&, Id id) {
            if (!isItem(id)) {
                throw py::key_error("no item " + std::to_string(id));
            }
            return wrap(id);
        })
        .def("__contains__", [](const SketchH&, py::handle h) { return isItem(idOf(h)); })
        .def_property_readonly("entities", listOf(std::nullopt))
        .def_property_readonly("points", listOf(EntityKind::Point))
        .def_property_readonly("lines", listOf(EntityKind::Line))
        .def_property_readonly("circles", listOf(EntityKind::Circle))
        .def_property_readonly("arcs", listOf(EntityKind::Arc))
        .def_property_readonly("constraints", [](const SketchH&) {
            std::vector<Id> ids;
            for (const auto& [id, c] : sketch().constraints()) {
                ids.push_back(id);
            }
            return wrapAll(ids);
        })
        .def_property_readonly("dimensions", [](const SketchH&) {
            std::vector<Id> ids;
            for (const auto& [id, c] : sketch().constraints()) {
                if (core::isDimension(c.type)) {
                    ids.push_back(id);
                }
            }
            return wrapAll(ids);
        })

        // geometry
        .def("point", [](const SketchH&, double x, double y) {
            return wrap(editReturningId("Point", [&](Editor& ed) { return ed.point(x, y); }));
        }, py::arg("x"), py::arg("y"))
        .def("line", [](const SketchH&, py::handle a, py::handle b) {
            return wrap(editReturningId("Line", [&](Editor& ed) {
                const Id pa = pointArg(ed, a);
                return ed.line(pa, pointArg(ed, b));
            }));
        }, py::arg("start"), py::arg("end"), "Line between two points or (x, y) pairs.")
        .def("circle", [](const SketchH&, py::handle center, double radius) {
            return wrap(editReturningId("Circle", [&](Editor& ed) { return ed.circle(pointArg(ed, center), radius); }));
        }, py::arg("center"), py::arg("radius"))
        .def("arc", [](const SketchH&, py::handle center, py::handle start, py::handle end) {
            return wrap(editReturningId("Arc", [&](Editor& ed) {
                const Id c = pointArg(ed, center);
                const Id s0 = pointArg(ed, start);
                return ed.arc(c, s0, pointArg(ed, end));
            }));
        }, py::arg("center"), py::arg("start"), py::arg("end"), "Counter-clockwise from start to end.")
        .def("ellipse", [](const SketchH&, py::handle center, py::handle major, py::handle minor) {
            return wrap(editReturningId("Ellipse", [&](Editor& ed) {
                const Id c = pointArg(ed, center);
                const Id a = pointArg(ed, major);
                Id b = kNoId;
                if (py::isinstance<py::float_>(minor) || py::isinstance<py::int_>(minor)) {
                    // A number: the other semi-axis, at right angles (counter-clockwise).
                    const double r = minor.cast<double>();
                    const core::Point& pc = ed.sketch().point(c);
                    const core::Point& pa = ed.sketch().point(a);
                    const double len = std::hypot(pa.x - pc.x, pa.y - pc.y);
                    if (!(r > 0.0) || !(len > 0.0)) {
                        throw core::SketchError("ellipse: the radii must be positive");
                    }
                    b = ed.point(pc.x - (pa.y - pc.y) / len * r, pc.y + (pa.x - pc.x) / len * r);
                }
                else {
                    b = pointArg(ed, minor);
                }
                return ed.ellipse(c, a, b);
            }));
        }, py::arg("center"), py::arg("major"), py::arg("minor"),
           "An ellipse: its centre, the end of one semi-axis, and the end of the other (a point, or just its "
           "length). The axes are kept at right angles.")
        .def("ellipse_arc", [](const SketchH&, py::handle center, py::handle major, py::handle minor, py::handle start,
                               py::handle end) {
            return wrap(editReturningId("Elliptical arc", [&](Editor& ed) {
                const Id c = pointArg(ed, center);
                const Id a = pointArg(ed, major);
                const Id b = pointArg(ed, minor);
                const Id s0 = pointArg(ed, start);
                return ed.ellipseArc(c, a, b, s0, pointArg(ed, end));
            }));
        }, py::arg("center"), py::arg("major"), py::arg("minor"), py::arg("start"), py::arg("end"),
           "Part of an ellipse, counter-clockwise from start to end (both kept on it).")
        .def("spline", [](const SketchH&, py::handle pointsArg, const std::string& kind, bool closed) {
            core::SplineKind k;
            if (!core::splineKindFromString(kind, k)) {
                throw py::value_error("kind: \"fit\" (through the points) or \"control\" (pulled by them)");
            }
            return wrap(editReturningId("Spline", [&](Editor& ed) {
                std::vector<Id> pts;
                for (py::handle h : pointsArg) {
                    pts.push_back(pointArg(ed, h));
                }
                return ed.spline(pts, k, closed);
            }));
        }, py::arg("points"), py::arg("kind") = "fit", py::arg("closed") = false,
           "A smooth curve through the points (kind=\"fit\") or pulled towards them, through the first and last "
           "(kind=\"control\"). Points or (x, y) pairs; closed joins the last back to the first.")
        .def("rectangle", [](const SketchH&, py::handle corner, py::handle opposite) {
            return editCurves("Rectangle", [&](Editor& ed) {
                const Id a = pointArg(ed, corner);
                const auto r = ed.rectangle(a, pointArg(ed, opposite));
                return std::vector<Id>(r.sides.begin(), r.sides.end());
            });
        }, py::arg("corner"), py::arg("opposite"),
             "Axis-aligned rectangle; returns its sides [bottom, right, top, left] (H/V constrained).")
        .def("rectangle_3point", [](const SketchH&, py::handle a, py::handle b, py::handle widthPoint) {
            return editCurves("Rectangle", [&](Editor& ed) {
                const Id pa = pointArg(ed, a);
                const Id pb = pointArg(ed, b);
                const auto r = ed.rectangle3(pa, pb, vecArg(widthPoint));
                return std::vector<Id>(r.sides.begin(), r.sides.end());
            });
        }, py::arg("a"), py::arg("b"), py::arg("width_point"),
             "Rotated rectangle: first side a->b, opposite side through width_point. Returns the sides.")
        .def("rectangle_center", [](const SketchH&, py::handle center, py::handle corner) {
            return editCurves("Rectangle", [&](Editor& ed) {
                const auto r = ed.rectangleCenter(pointArg(ed, center), vecArg(corner));
                return std::vector<Id>(r.sides.begin(), r.sides.end());
            });
        }, py::arg("center"), py::arg("corner"), "Axis-aligned rectangle about a centre point. Returns the sides.")
        .def("circle_2point", [](const SketchH&, py::handle a, py::handle b) {
            return wrap(editReturningId("Circle", [&](Editor& ed) { return ed.circle2(vecArg(a), vecArg(b)); }));
        }, py::arg("a"), py::arg("b"), "Circle with a and b at the ends of a diameter.")
        .def("circle_3point", [](const SketchH&, py::handle a, py::handle b, py::handle c) {
            return wrap(editReturningId("Circle", [&](Editor& ed) { return ed.circle3(vecArg(a), vecArg(b), vecArg(c)); }));
        }, py::arg("a"), py::arg("b"), py::arg("c"), "Circle through three points.")
        .def("circle_tangent", [](const SketchH&, py::handle curves, py::handle near, double radius) {
            std::vector<Id> ids;
            for (py::handle h : curves) {
                ids.push_back(idOf(h));
            }
            const core::Vec2 at = vecArg(near);
            return wrap(editReturningId("Circle", [&](Editor& ed) { return ed.circleTangent(ids, at, radius); }));
        }, py::arg("curves"), py::arg("near"), py::arg("radius") = 0.0,
             "Circle tangent to 2 curves (give radius) or 3 curves, on the side nearest `near`.")
        .def("arc_tangent", [](const SketchH&, py::handle curve, py::handle start, py::handle end) {
            const Id c = idOf(curve);
            return wrap(editReturningId("Arc", [&](Editor& ed) {
                const Id st = pointArg(ed, start);
                return ed.arcTangent(c, st, pointArg(ed, end));
            }));
        }, py::arg("curve"), py::arg("start"), py::arg("end"),
             "Arc continuing tangentially from `start` (an end of a line or arc) to `end`.")
        .def("polygon", [](const SketchH&, py::handle center, py::handle at, int sides, bool circumscribed) {
            return editCurves("Polygon", [&](Editor& ed) {
                return ed.polygon(pointArg(ed, center), vecArg(at), sides,
                                  circumscribed ? core::build::PolygonKind::Circumscribed
                                                : core::build::PolygonKind::Inscribed)
                    .sides;
            });
        }, py::arg("center"), py::arg("at"), py::arg("sides") = 6, py::arg("circumscribed") = false,
             "Regular polygon. `at` is a vertex, or with circumscribed=True a side's midpoint. Returns the sides.")
        .def("polygon_edge", [](const SketchH&, py::handle a, py::handle b, int sides, bool left) {
            return editCurves("Polygon", [&](Editor& ed) {
                const Id pa = pointArg(ed, a);
                return ed.polygonEdge(pa, pointArg(ed, b), sides, left).sides;
            });
        }, py::arg("a"), py::arg("b"), py::arg("sides") = 6, py::arg("left") = true,
             "Regular polygon built on the edge a->b (on its left unless left=False). Returns the sides.")
        .def("slot", [](const SketchH&, py::handle c1, py::handle c2, double width) {
            return editCurves("Slot", [&](Editor& ed) {
                const Id a = pointArg(ed, c1);
                return ed.slot(a, pointArg(ed, c2), width).curves;
            });
        }, py::arg("center1"), py::arg("center2"), py::arg("width"),
             "Centre-to-centre slot. Returns its outline [side, cap, side, cap].")
        .def("slot_overall", [](const SketchH&, py::handle e1, py::handle e2, double width) {
            return editCurves("Slot", [&](Editor& ed) { return ed.slotOverall(vecArg(e1), vecArg(e2), width).curves; });
        }, py::arg("end1"), py::arg("end2"), py::arg("width"), "Slot from its overall end points.")
        .def("slot_center_point", [](const SketchH&, py::handle center, py::handle cap, double width) {
            return editCurves("Slot", [&](Editor& ed) {
                return ed.slotCenterPoint(pointArg(ed, center), vecArg(cap), width).curves;
            });
        }, py::arg("center"), py::arg("cap_center"), py::arg("width"),
             "Slot about its centre point; cap_center is one end's centre.")
        .def("arc_slot", [](const SketchH&, py::handle center, py::handle start, py::handle end, double width) {
            return editCurves("Slot", [&](Editor& ed) {
                return ed.arcSlot(pointArg(ed, center), vecArg(start), vecArg(end), width).curves;
            });
        }, py::arg("center"), py::arg("start"), py::arg("end"), py::arg("width"),
             "Arc slot along a counter-clockwise centreline arc from start to end.")
        .def("arc_slot_3point", [](const SketchH&, py::handle start, py::handle end, py::handle through, double width) {
            return editCurves("Slot", [&](Editor& ed) {
                return ed.arcSlot3(vecArg(start), vecArg(end), vecArg(through), width).curves;
            });
        }, py::arg("start"), py::arg("end"), py::arg("through"), py::arg("width"),
             "Arc slot along the arc through three points.")
        .def("trim", [](const SketchH&, py::handle curve, py::handle near, bool construction) {
            const Id id = idOf(curve);
            const core::Vec2 at = vecArg(near);
            editReturningId("Trim", [&](Editor& ed) {
                ed.trim(id, at, construction);
                return id;
            });
        }, py::arg("curve"), py::arg("near"), py::arg("construction") = false,
             "Remove the part of a curve around `near`, up to where other curves cross it "
             "(or with construction=True, keep it as construction geometry).")
        .def("extend", [](const SketchH&, py::handle curve, py::handle near) {
            const Id id = idOf(curve);
            const core::Vec2 at = vecArg(near);
            editReturningId("Extend", [&](Editor& ed) {
                ed.extend(id, at);
                return id;
            });
        }, py::arg("curve"), py::arg("near"), "Lengthen the end of a line/arc nearest `near` to the next curve.")
        .def("break_curve", [](const SketchH&, py::handle curve, py::handle near) {
            const Id id = idOf(curve);
            const core::Vec2 at = vecArg(near);
            return editCurves("Break", [&](Editor& ed) { return ed.breakCurve(id, at); });
        }, py::arg("curve"), py::arg("near"), "Split a curve where others cross it either side of `near`. Returns the pieces.")
        .def("fillet", [](const SketchH&, py::handle a, py::handle b, double radius, py::object near) {
            const Id ia = idOf(a), ib = idOf(b);
            const auto [na, nb] = cornerNear(ia, ib, near);
            return wrap(editReturningId("Fillet", [&](Editor& ed) { return ed.fillet(ia, na, ib, nb, radius).joint; }));
        }, py::arg("a"), py::arg("b"), py::arg("radius"), py::arg("near") = py::none(),
             "Round the corner between two lines/arcs. The originals become construction. near=(pos_a, pos_b) "
             "picks which side of each to keep (needed unless they share an end). Returns the arc.")
        .def("chamfer", [](const SketchH&, py::handle a, py::handle b, double distance, py::object distance2,
                           py::object angle, py::object near) {
            const Id ia = idOf(a), ib = idOf(b);
            const auto [na, nb] = cornerNear(ia, ib, near);
            auto kind = core::modify::ChamferKind::EqualDistance;
            double second = 0.0;
            if (!angle.is_none()) {
                kind = core::modify::ChamferKind::DistanceAngle;
                second = angle.cast<double>();
            }
            else if (!distance2.is_none()) {
                kind = core::modify::ChamferKind::TwoDistances;
                second = distance2.cast<double>();
            }
            return wrap(editReturningId("Chamfer", [&](Editor& ed) {
                return ed.chamfer(ia, na, ib, nb, kind, distance, second).joint;
            }));
        }, py::arg("a"), py::arg("b"), py::arg("distance"), py::arg("distance2") = py::none(),
             py::arg("angle") = py::none(), py::arg("near") = py::none(),
             "Cut the corner between two lines: equal distances, or distance2=, or angle= (degrees). Returns the chamfer.")
        .def("delete", [](const SketchH&, py::args items) {
            const std::vector<Id> ids = idsOf(items);
            Call call;
            call.target().edit(QStringLiteral("Delete"), [&](Editor& ed) {
                for (Id id : ids) {
                    if (isItem(id)) {
                        ed.remove(id);
                    }
                }
            });
        }, "Delete items (and whatever depends on them).")

        // geometric constraints
        .def("coincident", constraintFn(ConstraintType::Coincident), "(point, point) or (point, curve)")
        .def("horizontal", constraintFn(ConstraintType::Horizontal), "(line) or (point, point)")
        .def("vertical", constraintFn(ConstraintType::Vertical), "(line) or (point, point)")
        .def("parallel", constraintFn(ConstraintType::Parallel))
        .def("perpendicular", constraintFn(ConstraintType::Perpendicular))
        .def("tangent", constraintFn(ConstraintType::Tangent))
        .def("equal", constraintFn(ConstraintType::Equal))
        .def("midpoint", constraintFn(ConstraintType::Midpoint), "(point, line)")
        .def("concentric", constraintFn(ConstraintType::Concentric))
        .def("symmetric", constraintFn(ConstraintType::Symmetric),
             "(a, b, line): two points, lines, circles or arcs, mirror images in the line")
        .def("collinear", constraintFn(ConstraintType::Collinear), "(line, line)")
        .def("group", [](const SketchH&, py::handle items) {
            const std::vector<Id> ids = itemsOf(items);
            Call call;
            QString error;
            const Id id = call.target().addGroup(ids, &error);
            if (id == kNoId) {
                throw py::value_error(error.toStdString());
            }
            return wrap(id);
        }, py::arg("items"),
           "Group items (curves, points): they keep their places relative to each other, like a rigid group "
           "in Fusion, but move and turn together unless something else holds them (a fixed point, constraints to "
           "the origin or axes). Constraints inside the group are carried by it; one setting an orientation "
           "(horizontal, vertical) still stops it turning. Delete the group (s.delete(k)) to ungroup.")
        .def("ratio", [](const SketchH&, py::handle a, py::handle b, py::object value) {
            return addConstraint(ConstraintType::Ratio, {idOf(a), idOf(b)}, value, py::none());
        }, py::arg("a"), py::arg("b"), py::arg("value") = py::none(),
           "(line, line) lengths or (circle|arc, circle|arc) radii: a is `value` times b (default: as they are "
           "now). Change it later with k.value = ...")

        // dimensions: value=None keeps the current size
        .def("distance", dimensionFn(ConstraintType::Distance), py::arg("value") = py::none(),
             py::arg("label") = py::none(), "(line), (point, point) or (point, line)")
        .def("horizontal_distance", dimensionFn(ConstraintType::HorizontalDistance), py::arg("value") = py::none(), py::arg("label") = py::none())
        .def("vertical_distance", dimensionFn(ConstraintType::VerticalDistance), py::arg("value") = py::none(), py::arg("label") = py::none())
        .def("radius", dimensionFn(ConstraintType::Radius), py::arg("value") = py::none(),
             py::arg("label") = py::none())
        .def("diameter", dimensionFn(ConstraintType::Diameter), py::arg("value") = py::none(),
             py::arg("label") = py::none())
        .def("angle", [](const SketchH&, py::handle l1, py::handle l2, py::object value, py::object label,
                          py::object reverse) {
            const Id a = idOf(l1), b = idOf(l2);
            std::uint8_t flip = 0;
            if (!reverse.is_none()) {
                const auto r = reverse.cast<std::pair<bool, bool>>();
                flip = static_cast<std::uint8_t>((r.first ? 1 : 0) | (r.second ? 2 : 0));
            }
            else if (!label.is_none() && sketch().hasEntity(a) && sketch().hasEntity(b)) {
                // No explicit choice: measure the angle on the side the label is placed.
                const auto [lx, ly] = pairOf(label, "label");
                core::Constraint probe{kNoId, ConstraintType::Angle, {a, b}, 0.0};
                probe.labelX = 1.0;
                const core::Vec2 anchor = core::layoutDimension(sketch(), probe).anchor;
                flip = core::angleFlipAt(sketch(), a, b, {anchor.x + lx, anchor.y + ly});
            }
            return addConstraint(ConstraintType::Angle, {a, b}, std::move(value), std::move(label), flip);
        }, py::arg("line1"), py::arg("line2"), py::arg("value") = py::none(), py::arg("label") = py::none(),
           py::arg("reverse") = py::none(),
           "Angle in degrees between rays along two lines. reverse=(r1, r2) flips either line's ray (p1->p2) "
           "to choose which of the four angles is meant; otherwise it's the one the label sits in.")

        // moving things
        .def("drag", [](const SketchH&, py::handle points, double dx, double dy) {
            std::vector<Id> ids;
            for (py::handle h : points) {
                ids.push_back(idOf(h));
            }
            dragPoints(ids, dx, dy);
        }, py::arg("points"), py::arg("dx"), py::arg("dy"), "Drag points by an offset, as with the mouse.")
        .def("drag_edge", [](const SketchH&, py::handle curve, py::handle grab, double dx, double dy) {
            const Id id = idOf(curve);
            const auto [x, y] = pairOf(grab, "grab");
            Call call;
            call.target().beginDragEdge(id, x, y);
            call.target().dragTo(dx, dy);
            call.target().endDrag();
        }, py::arg("curve"), py::arg("grab"), py::arg("dx"), py::arg("dy"), "Drag a circle/arc rim from a grab point.")
        // parameters
        .def("parameter", [](const SketchH&, const std::string& name, py::object expression) -> double {
            if (!expression.is_none()) {
                const std::string text = py::isinstance<py::str>(expression) ? expression.cast<std::string>()
                                                                            : py::str(expression).cast<std::string>();
                Call call;
                QString error;
                if (!call.target().setParameter(QString::fromStdString(name), QString::fromStdString(text), &error)) {
                    throw py::value_error(error.toStdString());
                }
            }
            const int at = core::parameters::find(sketch(), name);
            if (at < 0) {
                throw py::key_error("no parameter called " + name);
            }
            return sketch().parameters()[static_cast<size_t>(at)].value;
        }, py::arg("name"), py::arg("expression") = py::none(),
           "A named value for dimensions to use: s.parameter(\"width\", 120), s.parameter(\"hole\", \"width / 10\") "
           "(parameters may use each other). Changing one re-works out every dimension using it (d.value = "
           "\"width / 2\") and re-solves. Returns its value; with no expression, just reads it.")
        .def("rename_parameter", [](const SketchH&, const std::string& from, const std::string& to) {
            Call call;
            QString error;
            if (!call.target().renameParameter(QString::fromStdString(from), QString::fromStdString(to), &error)) {
                throw py::value_error(error.toStdString());
            }
        }, py::arg("old"), py::arg("new"), "Renames a parameter, and every expression using it.")
        .def("delete_parameter", [](const SketchH&, const std::string& name) {
            Call call;
            QString error;
            if (!call.target().deleteParameter(QString::fromStdString(name), &error)) {
                throw py::value_error(error.toStdString());
            }
        }, py::arg("name"), "Deletes a parameter (not while a dimension or another parameter uses it).")
        .def_property_readonly("parameters", [](const SketchH&) {
            py::list out;
            const py::object item = py::module_::import("types").attr("SimpleNamespace");
            for (const core::Parameter& p : sketch().parameters()) {
                out.append(item(py::arg("name") = p.name, py::arg("expression") = p.expression, py::arg("value") = p.value));
            }
            return out;
        }, "The drawing's parameters, in order: each with .name, .expression and .value.")
        .def("move", [](const SketchH&, py::handle items, double dx, double dy, double angle, py::handle about) {
            std::vector<Id> points;
            for (Id id : core::xform::expand(sketch(), itemsOf(items))) {
                if (sketch().entity(id).kind() == EntityKind::Point) {
                    points.push_back(id);
                }
            }
            const core::Vec2 centre = vecArg(about);
            Call call;
            call.target().beginDrag(points);
            // Turn in small steps, as a mouse would: the solver follows the geometry from where it
            // was, and a big jump could land it in a mirrored arrangement instead.
            const int steps = std::max(1, static_cast<int>(std::ceil(std::abs(angle) / 10.0)));
            for (int i = 1; i <= steps; ++i) {
                const double f = double(i) / steps;
                call.target().dragMove(dx * f, dy * f, angle * f, centre);
            }
            const size_t dropped = call.target().droppedByTurn().size();
            call.target().endDrag();
            if (dropped > 0) {
                const std::string text = "move: turning dropped " + std::to_string(dropped)
                    + (dropped == 1 ? " horizontal/vertical constraint" : " horizontal/vertical constraints")
                    + " (they'd have stopped it turning)";
                if (PyErr_WarnEx(PyExc_UserWarning, text.c_str(), 1) < 0) {
                    throw py::error_already_set();  // (warnings made errors)
                }
            }
        }, py::arg("items"), py::arg("dx") = 0.0, py::arg("dy") = 0.0, py::arg("angle") = 0.0,
           py::arg("about") = py::make_tuple(0, 0),
           "Move items rigidly: turn by `angle` degrees about `about`, then shift by (dx, dy). Connected geometry "
           "follows as the constraints allow, as when dragging. Turned other than by half turns, horizontal and "
           "vertical constraints held within the items are dropped (with a warning): they'd stop it turning.")

        // appearance
        .def("stroke", [](const SketchH&, py::handle curves, py::object color, py::object width, py::object dash) {
            const std::vector<Id> ids = itemsOf(curves);
            // What isn't given stays as the curve has it (or the default for a new stroke).
            std::vector<std::pair<core::StrokeStyle, std::vector<Id>>> groups;
            for (Id id : ids) {
                entity(id);
                core::StrokeStyle style = sketch().stroke(id) ? *sketch().stroke(id) : core::StrokeStyle{};
                if (!color.is_none()) {
                    const PaintArg p = paintArg(color);
                    style.color = p.color;
                    style.gradient = p.gradient;
                }
                if (py::isinstance<py::str>(width)) {
                    const std::string name = width.cast<std::string>();
                    if (name == "none") {
                        style.width = core::StrokeStyle::kNone;
                    }
                    else if (name == "hairline") {
                        style.width = 0.0;
                    }
                    else {
                        throw py::value_error("width: a size in mm, \"hairline\" or \"none\"");
                    }
                }
                else if (!width.is_none()) {
                    style.width = width.cast<double>();
                    if (style.width < 0.0) {
                        throw py::value_error("width: a size in mm, \"hairline\" or \"none\"");
                    }
                }
                if (!dash.is_none()) {
                    const std::string name = dash.cast<std::string>();
                    if (!core::dashFromString(name, style.dash)) {
                        throw py::value_error("unknown line style '" + name
                                              + "' (solid, dashed, dotted, dash_dot, dash_dot_dot)");
                    }
                }
                auto it = std::ranges::find_if(groups, [&](const auto& g) { return g.first == style; });
                if (it == groups.end()) {
                    groups.push_back({style, {id}});
                }
                else {
                    it->second.push_back(id);
                }
            }
            editReturningId("Stroke", [&](Editor& ed) {
                for (const auto& [style, group] : groups) {
                    ed.setStroke(group, style);
                }
                return kNoId;
            });
        }, py::arg("curves"), py::arg("color") = py::none(), py::arg("width") = py::none(),
           py::arg("dash") = py::none(),
           "Give curves a stroke: color as '#rrggbb' (or '#rrggbbaa', or an (r, g, b[, a]) tuple), width in mm "
           "(0 = hairline; \"none\": no line, e.g. for a fill without an outline; color may be a setsquare.linear / "
           "setsquare.radial gradient), dash 'solid', 'dashed', 'dotted', 'dash_dot' or 'dash_dot_dot'.")
        .def("fill", [](const SketchH&, py::handle at, py::handle color, py::object hatch) {
            const core::Vec2 where = vecArg(at);
            const PaintArg p = paintArg(color);
            const core::Hatch h = hatch.is_none() ? core::Hatch{} : hatch.cast<core::Hatch>();
            return wrap(editReturningId("Fill", [&](Editor& ed) { return ed.fill(where, p.color, p.gradient, h); }));
        }, py::arg("at"), py::arg("color") = "#c3c3c3", py::arg("hatch") = py::none(),
           "Fill the closed area around `at` (as a paint bucket would; a fill already there is recoloured). color: a "
           "colour, or a gradient (setsquare.linear / setsquare.radial). hatch: a setsquare.hatch, to draw the area with "
           "its lines in the colour instead. "
           "Raises ValueError if `at` isn't enclosed: see loose_ends().")
        .def("annotate", [](const SketchH&, const std::string& kind, py::object refs, py::object at, py::object text,
                            py::object decimals, py::object textHeight, py::object width, py::object color,
                            py::object font, py::object origin, py::object along, py::object tip, py::object flip) {
            core::DrawnDimension d;
            if (!core::drawnDimensionKindFromString(kind, d.kind)) {
                throw py::value_error("unknown kind '" + kind + "': aligned, horizontal, vertical, radius, diameter, "
                                      "angle, arc_length, ordinate or leader");
            }
            if (!refs.is_none()) {
                d.refs = itemsOf(refs);
            }
            d.text = core::annotation::defaultText(d.kind);
            d.font = textshapes::defaultFamily().toStdString();
            d.style.color = core::Color{0, 0, 0, 255};
            d.style.width = 0.25;
            if (d.kind == core::DrawnDimensionKind::Ordinate && d.refs.size() == 1 && sketch().origin() != kNoId) {
                d.refs.push_back(sketch().origin());  // (from the origin, unless given)
            }
            annotationSettings(d, at, text, decimals, textHeight, width, color, font, origin, along, tip, flip);
            return wrap(editReturningId("Annotate", [&](Editor& ed) { return ed.annotate(d); }));
        }, py::arg("kind"), py::arg("refs") = py::none(), py::arg("at") = py::none(), py::arg("text") = py::none(),
           py::arg("decimals") = py::none(), py::arg("text_height") = py::none(), py::arg("width") = py::none(),
           py::arg("color") = py::none(), py::arg("font") = py::none(), py::arg("origin") = py::none(),
           py::arg("along") = py::none(), py::arg("tip") = py::none(), py::arg("flip") = py::none(),
           "Draw a dimension (or note) on the drawing: it measures, never drives, and is shown and exported with it. "
           "kind: 'aligned', 'horizontal', 'vertical' (refs: two points, a point and a line, or a line), 'radius', "
           "'diameter' (a circle or arc), 'angle' (two lines; flip= 0-3 for which of the four), 'arc_length' (an arc), "
           "'ordinate' (a point: its x and y from origin=, a point, or the sketch's origin; <x> and <y> each on its "
           "own in text), 'leader' (a point, a curve along= its parameter, or nothing and tip=(x, y)). at: where the label "
           "goes. text: what it says, <> standing for the value (the default: '<>', 'R<>', 'Ø<>', '<>°', '⌒<>'; a "
           "leader's 'Note'). decimals: places after the point (None: the drawing's). text_height (mm, capital "
           "letters; 3.5), width (line width, mm; 0.25), color (black), font.")
        .def("edit_annotation", [](const SketchH&, py::handle item, py::object at, py::object text, py::object decimals,
                                   py::object textHeight, py::object width, py::object color, py::object font,
                                   py::object origin, py::object along, py::object tip, py::object flip) {
            const Id id = idOf(item);
            if (!sketch().hasDrawnDimension(id)) {
                throw py::value_error("not an annotation");
            }
            core::DrawnDimension d = sketch().drawnDimension(id);
            annotationSettings(d, at, text, decimals, textHeight, width, color, font, origin, along, tip, flip);
            editReturningId("Edit annotation", [&](Editor& ed) {
                ed.setAnnotation(d);
                return id;
            });
        }, py::arg("annotation"), py::arg("at") = py::none(), py::arg("text") = py::none(),
           py::arg("decimals") = py::none(), py::arg("text_height") = py::none(), py::arg("width") = py::none(),
           py::arg("color") = py::none(), py::arg("font") = py::none(), py::arg("origin") = py::none(),
           py::arg("along") = py::none(), py::arg("tip") = py::none(), py::arg("flip") = py::none(),
           "Change an annotation (what's given; as Sketch.annotate's). decimals=-1: back to the drawing's.")
        .def_property_readonly("annotations", [](const SketchH&) {
            std::vector<Id> ids;
            for (const auto& [id, d] : sketch().drawnDimensions()) {
                ids.push_back(id);
            }
            return wrapAll(ids);
        }, "The dimensions and notes drawn on the drawing (see annotate).")
        .def_property(
            "dimension_decimals", [](const SketchH&) { return sketch().dimensionDecimals(); },
            [](const SketchH&, int places) {
                editReturningId("Dimension decimals", [&](Editor& ed) {
                    ed.setDimensionDecimals(places);
                    return kNoId;
                });
            },
            "How many places after the point annotations show unless they say (0 to 6; 2 to start with).")
        .def("measure_length", [](const SketchH&, py::handle curves) {
            const std::vector<Id> ids = itemsOf(curves);
            for (Id id : ids) {
                if (!sketch().hasEntity(id) || sketch().entity(id).kind() == core::EntityKind::Point) {
                    throw py::value_error("measure_length takes curves (lines, arcs, circles, ellipses, splines)");
                }
            }
            return core::measuring::length(sketch(), ids);
        }, py::arg("curves"),
           "The length of a curve, or the total of a list of them, in mm (as the Measure tool shows).")
        .def("measure_between", [](const SketchH&, py::handle a, py::handle b) {
            const core::measuring::Separation d = core::measuring::between(vecArg(a), vecArg(b));
            return py::make_tuple(d.distance, d.dx, d.dy);
        }, py::arg("a"), py::arg("b"),
           "From point a to point b (points of the sketch, or (x, y)): (distance, dx, dy), dx and dy being b - a.")
        .def("measure_area", [](const SketchH&, py::handle at) -> py::object {
            const auto region = core::measuring::areaAt(sketch(), sketch().activeLayer(), vecArg(at));
            if (!region) {
                return py::none();
            }
            return py::float_(core::measuring::area(*region));
        }, py::arg("at"),
           "The size of the closed area around `at` (of the active layer's geometry), in mm^2, its holes taken off; "
           "None if `at` isn't enclosed.")
        .def("text", [](const SketchH&, const std::string& text, py::object at, double height, py::object font,
                        double angle, bool bold, bool italic, const std::string& outline, double minThickness,
                        py::object color, double spacing, const std::string& align, py::object valign,
                        bool flipH, bool flipV, py::object path, py::object box, bool wrapLines, bool grow) {
            core::Text t;
            t.spacing = spacing;
            t.flipH = flipH;
            t.flipV = flipV;
            if ((wrapLines || grow) && box.is_none()) {
                throw py::value_error("wrap= and grow= are for text in a box (box=)");
            }
            if (grow && !wrapLines) {
                throw py::value_error("grow=True needs wrap=True (the box grows to fit the wrapped lines)");
            }
            t.wrap = wrapLines;
            t.grow = grow;
            const std::string vertical = !valign.is_none() ? valign.cast<std::string>() : box.is_none() ? "baseline" : "top";
            if (!core::textAlignFromString(align, t.align) || !core::textVAlignFromString(vertical, t.valign)) {
                throw py::value_error("align: 'left', 'center' or 'right'; valign: 'baseline', 'top', 'middle' or 'bottom'");
            }
            if (!path.is_none()) {
                // A curve, or curves joined end to end (put in order along their chain).
                std::vector<Id> curves;
                if (py::isinstance<py::sequence>(path) && !py::isinstance<py::str>(path)) {
                    for (py::handle c : path.cast<py::sequence>()) {
                        curves.push_back(idOf(c));
                    }
                }
                else {
                    curves.push_back(idOf(path));
                }
                for (Id c : curves) {
                    if (!sketch().hasEntity(c) || sketch().entity(c).kind() == EntityKind::Point) {
                        throw py::value_error("path: a curve, or a list of curves joined end to end");
                    }
                }
                if (curves.empty()) {
                    throw py::value_error("path: a curve, or a list of curves joined end to end");
                }
                try {
                    curves = core::feature::chainOrder(sketch(), curves);
                }
                catch (const core::SketchError& e) {
                    throw py::value_error(std::string("path: ") + e.what());
                }
                t.path = curves.front();
                t.pathMore.assign(curves.begin() + 1, curves.end());
            }
            else if (at.is_none() && box.is_none()) {
                throw py::value_error("give where it goes (at=, or a box=(corner, opposite)), or a curve to follow (path=)");
            }

            t.text = text;
            t.font = font.is_none() ? textshapes::defaultFamily().toStdString() : font.cast<std::string>();
            t.height = height;
            t.angle = angle;
            t.bold = bold;
            t.italic = italic;
            if (!core::textOutlineFromString(outline, t.outline)) {
                throw py::value_error("outline: 'as_designed', 'merged' or 'silhouette'");
            }
            t.minThickness = minThickness;
            if (!color.is_none()) {
                const PaintArg p = paintArg(color);
                t.hasColor = true;
                t.color = p.color;
                t.gradient = p.gradient;
            }
            if (!box.is_none()) {
                const py::sequence corners = box.cast<py::sequence>();
                if (corners.size() != 2) {
                    throw py::value_error("box: (corner, opposite corner)");
                }
                return wrap(editReturningId("Text", [&](Editor& ed) {
                    const Id a = pointArg(ed, corners[0]);
                    const Id b = pointArg(ed, corners[1]);
                    return ed.textBox(t, a, b);
                }));
            }            return wrap(editReturningId("Text", [&](Editor& ed) {
                t.anchor = t.path != kNoId ? ed.sketch().definingPoints(t.path).front() : pointArg(ed, at);
                return ed.text(t);
            }));
        }, py::arg("text"), py::arg("at") = py::none(), py::arg("height") = 10.0, py::arg("font") = py::none(),
           py::arg("angle") = 0.0, py::arg("bold") = false, py::arg("italic") = false,
           py::arg("outline") = "as_designed", py::arg("min_thickness") = 0.0, py::arg("color") = py::none(),
           py::arg("spacing") = 0.0, py::arg("align") = "left", py::arg("valign") = py::none(),
           py::arg("flip_h") = false, py::arg("flip_v") = false, py::arg("path") = py::none(),
           py::arg("box") = py::none(), py::arg("wrap") = false, py::arg("grow") = false,
           "Text with its baseline starting at `at` (a point, or (x, y) for a new one). height: capital letters, "
           "mm. outline: 'as_designed', 'merged' (overlaps joined) or 'silhouette' (outside edge only). "
           "min_thickness: thicken any part narrower than this (mm). color: fill it in the drawing. Lines split "
           "at \\n. spacing: extra between letters, % of the size. align/valign: which part sits on `at`. "
           "flip_h/flip_v: mirrored. box=(corner, opposite): in a box (a construction rectangle made between them; "
           "align: against its left/centre/right, valign: top (the default)/middle/bottom). path=curve: set along a "
           "curve instead (any: line, arc, circle, ellipse, spline), or path=[curves] along a chain of them joined "
           "end to end (align: start/middle/end of it; valign: standing on it, centred, or hanging below; flip_h: "
           "the other way round). In a box: wrap=True breaks lines between words to fit its width; grow=True (with "
           "wrap) makes the box as tall as the text, moving the side away from where it's aligned (ValueError if "
           "that side can't move).")
        .def("edit_text", [](const SketchH&, py::handle text, py::object str, py::object height, py::object font,
                             py::object angle, py::object bold, py::object italic, py::object outline,
                             py::object minThickness, py::object color, py::object spacing, py::object align,
                             py::object valign, py::object flipH, py::object flipV, py::object wrapLines,
                             py::object grow) {
            const Id id = idOf(text);
            if (!sketch().hasText(id)) {
                throw py::value_error("not a text");
            }
            core::Text t = sketch().text(id);
            if (!str.is_none()) t.text = str.cast<std::string>();
            if (!height.is_none()) t.height = height.cast<double>();
            if (!font.is_none()) t.font = font.cast<std::string>();
            if (!angle.is_none()) t.angle = angle.cast<double>();
            if (!bold.is_none()) t.bold = bold.cast<bool>();
            if (!italic.is_none()) t.italic = italic.cast<bool>();
            if (!outline.is_none() && !core::textOutlineFromString(outline.cast<std::string>(), t.outline)) {
                throw py::value_error("outline: 'as_designed', 'merged' or 'silhouette'");
            }
            if (!minThickness.is_none()) t.minThickness = minThickness.cast<double>();
            if (!color.is_none()) {
                const PaintArg p = paintArg(color);
                t.hasColor = true;
                t.color = p.color;
                t.gradient = p.gradient;
            }
            if (!spacing.is_none()) {
                t.spacing = spacing.cast<double>();
            }
            if ((!align.is_none() && !core::textAlignFromString(align.cast<std::string>(), t.align))
                || (!valign.is_none() && !core::textVAlignFromString(valign.cast<std::string>(), t.valign))) {
                throw py::value_error("align: 'left', 'center' or 'right'; valign: 'baseline', 'top', 'middle' or 'bottom'");
            }
            if (!flipH.is_none()) {
                t.flipH = flipH.cast<bool>();
            }
            if (!flipV.is_none()) {
                t.flipV = flipV.cast<bool>();
            }
            if (!wrapLines.is_none()) {
                t.wrap = wrapLines.cast<bool>();
                if (!t.wrap) {
                    t.grow = false;  // (it grows with the wrapped lines)
                }
            }
            if (!grow.is_none()) {
                t.grow = grow.cast<bool>();
            }
            if ((t.wrap || t.grow) && !t.inBox()) {
                throw py::value_error("wrap= and grow= are for text in a box");
            }
            if (t.grow && !t.wrap) {
                throw py::value_error("grow=True needs wrap=True (the box grows to fit the wrapped lines)");
            }
            editReturningId("Text", [&](Editor& ed) {
                ed.setText(t);
                return id;
            });
        }, py::arg("text_item"), py::arg("text") = py::none(), py::arg("height") = py::none(),
           py::arg("font") = py::none(), py::arg("angle") = py::none(), py::arg("bold") = py::none(),
           py::arg("italic") = py::none(), py::arg("outline") = py::none(), py::arg("min_thickness") = py::none(),
           py::arg("color") = py::none(), py::arg("spacing") = py::none(), py::arg("align") = py::none(),
           py::arg("valign") = py::none(), py::arg("flip_h") = py::none(), py::arg("flip_v") = py::none(),
           py::arg("wrap") = py::none(), py::arg("grow") = py::none(),
           "Change a text's settings (any of them); its letters are remade.")
        .def("import_file", [](const SketchH&, const std::string& path, const std::string& into, bool keepLayers,
                               const std::string& placement, py::object at, const std::string& curves, double pxPerInch,
                               py::object units, bool usePage) {
            importer::Options o;
            if (into == "sublayer") o.into = importer::Into::Sublayer;
            else if (into == "new_sublayer") o.into = importer::Into::NewSublayer;
            else if (into == "new_layer") o.into = importer::Into::NewLayer;
            else throw py::value_error("into: 'sublayer', 'new_sublayer' or 'new_layer'");
            o.keepLayers = keepLayers;
            o.usePage = usePage;
            if (placement == "file") o.placement = importer::Placement::AsInFile;
            else if (placement == "center") o.placement = importer::Placement::Center;
            else if (placement == "page") o.placement = importer::Placement::Page;
            else throw py::value_error("placement: 'file', 'center' or 'page'");
            if (o.placement == importer::Placement::Center) {
                if (at.is_none()) {
                    throw py::value_error("placement='center' needs at=(x, y)");
                }
                o.at = vecArg(at);
            }
            if (curves == "exact") o.curves = importer::Curves::Exact;
            else if (curves == "smooth") o.curves = importer::Curves::Smooth;
            else throw py::value_error("curves: 'exact' or 'smooth'");
            std::optional<double> mmPerUnit;
            if (!units.is_none()) {
                if (py::isinstance<py::str>(units)) {
                    static const std::map<std::string, double> names = {
                        {"mm", 1.0}, {"cm", 10.0}, {"m", 1000.0}, {"in", 25.4}, {"ft", 304.8}};
                    const auto it = names.find(units.cast<std::string>());
                    if (it == names.end()) {
                        throw py::value_error("units: 'mm', 'cm', 'm', 'in', 'ft', or millimetres per unit");
                    }
                    mmPerUnit = it->second;
                }
                else {
                    mmPerUnit = units.cast<double>();
                }
            }
            importer::Report report;
            editReturningId("Import", [&](Editor& ed) {
                report = ed.importFile(QString::fromStdString(path), o, pxPerInch, mmPerUnit);
                return kNoId;
            });
            py::dict d;
            d["curves"] = report.curves;
            d["fills"] = report.fills;
            d["gradients"] = report.gradients;
            d["hatches"] = report.hatches;
            d["images"] = report.images;
            d["texts"] = report.texts;
            d["groups"] = report.groups;
            d["sublayers"] = report.sublayers;
            d["skipped"] = report.skipped;
            d["notes"] = report.notes;
            return d;
        }, py::arg("path"), py::arg("into") = "new_sublayer", py::arg("keep_layers") = true, py::arg("placement") = "file",
           py::arg("at") = py::none(), py::arg("curves") = "exact", py::arg("px_per_inch") = 96.0,
           py::arg("units") = py::none(), py::arg("use_page") = false,
           "Import an SVG or DXF drawing (use_page=True: an SVG's page, its size given in real units or one this app wrote, becomes the sketch's): its curves (with their strokes) and fills, each of the file's layers held "
           "together as a group. into: 'sublayer' (the one being edited), 'new_sublayer' or 'new_layer'; keep_layers: "
           "the file's layers each in a sublayer of their own. placement: 'file' (its own coordinates; an SVG's page "
           "with its bottom-left corner on the origin), 'center' (its middle at at=(x, y)) or 'page' (an SVG's page on "
           "ours). curves: 'exact' (a spline per Bezier piece) or 'smooth' (fewer fit splines). px_per_inch: SVG "
           "lengths without units. units: a DXF's ('mm', 'in', ... or mm per unit; else as the file says, or mm). "
           "Returns what came in: {'curves', 'fills', 'groups', 'sublayers', 'skipped', 'notes'}.")
        .def("image", [](const SketchH&, const std::string& path, py::handle at, py::object width, py::object height,
                         double angle, double opacity, bool flipH, bool flipV) {
            core::Image im;
            try {
                im = images::load(QString::fromStdString(path));
            }
            catch (const core::SketchError& e) {
                throw py::value_error(e.what());
            }
            std::tie(im.x, im.y) = pairOf(at, "at");
            if (!width.is_none()) {
                im.width = width.cast<double>();
            }
            else if (!height.is_none()) {
                im.width = height.cast<double>() * im.pixelWidth / im.pixelHeight;
            }
            if (!(im.width > 0.0)) {
                throw py::value_error("width / height: more than 0");
            }
            im.angle = angle;
            im.opacity = std::clamp(opacity, 0.0, 1.0);
            im.flipH = flipH;
            im.flipV = flipV;
            return wrap(editReturningId("Image", [&](Editor& ed) { return ed.addImage(im); }));
        }, py::arg("path"), py::arg("at") = py::make_tuple(0.0, 0.0), py::arg("width") = py::none(),
           py::arg("height") = py::none(), py::arg("angle") = 0.0, py::arg("opacity") = 0.5, py::arg("flip_h") = false,
           py::arg("flip_v") = false,
           "Insert a reference image (PNG, JPEG, ...; kept in the drawing) centred at `at`, in the active layer. "
           "width or height in mm (else its natural size: from its resolution, or 96 pixels per inch); angle in "
           "degrees; opacity 0..1. Scale it to a known distance with Image.calibrate.")
        .def("edit_image", [](const SketchH&, py::handle image, py::object at, py::object width, py::object height,
                              py::object angle, py::object opacity, py::object flipH, py::object flipV, py::object visible) {
            const Id id = idOf(image);
            if (!sketch().hasImage(id)) {
                throw py::value_error("not an image");
            }
            core::Image im = sketch().image(id);
            if (!at.is_none()) {
                std::tie(im.x, im.y) = pairOf(at, "at");
            }
            if (!width.is_none()) {
                im.width = width.cast<double>();
            }
            else if (!height.is_none()) {
                im.width = height.cast<double>() * im.pixelWidth / im.pixelHeight;
            }
            if (!angle.is_none()) im.angle = angle.cast<double>();
            if (!opacity.is_none()) im.opacity = std::clamp(opacity.cast<double>(), 0.0, 1.0);
            if (!flipH.is_none()) im.flipH = flipH.cast<bool>();
            if (!flipV.is_none()) im.flipV = flipV.cast<bool>();
            if (!visible.is_none()) im.visible = visible.cast<bool>();
            editReturningId("Image", [&](Editor& ed) {
                ed.setImage(im);
                return id;
            });
        }, py::arg("image"), py::arg("at") = py::none(), py::arg("width") = py::none(), py::arg("height") = py::none(),
           py::arg("angle") = py::none(), py::arg("opacity") = py::none(), py::arg("flip_h") = py::none(),
           py::arg("flip_v") = py::none(), py::arg("visible") = py::none(),
           "Change a reference image's placement (any of them): centre, size (width or height, mm), angle, opacity, "
           "flips, and whether it shows.")
        .def_property_readonly("images", [](const SketchH&) {
            std::vector<Id> ids;
            for (const auto& [id, im] : sketch().images()) {
                ids.push_back(id);
            }
            return wrapAll(ids);
        }, "The reference images, bottom first.")
        .def_property_readonly("page", [](const SketchH&) -> py::object {
            const auto& page = sketch().page();
            if (!page) {
                return py::none();
            }
            py::dict d;
            d["size"] = page->size;
            d["width"] = page->width;
            d["height"] = page->height;
            d["orientation"] = page->landscape() ? "landscape" : "portrait";
            d["origin"] = std::string(core::toString(page->origin));
            d["color"] = page->hasColor ? py::cast(page->color.hex()) : py::none();
            d["rect"] = py::make_tuple(page->rect()[0], page->rect()[1], page->rect()[2], page->rect()[3]);
            return d;
        }, "The page as {'size', 'width', 'height', 'orientation', 'origin', 'color', 'rect'} (mm; rect: "
           "x0, y0, x1, y1 in sketch coordinates), or None (see set_page).")
        .def("set_page", [](const SketchH&, py::object size, py::object orientation, py::object width,
                            py::object height, const std::string& origin, py::object color) {
            core::Page p;
            if (!size.is_none() && size.cast<std::string>() != "custom") {
                p.size = size.cast<std::string>();
                if (!core::pageSizeByName(p.size, p.width, p.height)) {
                    std::string names;
                    for (const core::PageSize& ps : core::pageSizes()) {
                        names += (names.empty() ? "" : ", ") + std::string(ps.name);
                    }
                    throw py::value_error("unknown page size '" + p.size + "' (" + names + ", or width= and height=)");
                }
            }
            else if (!width.is_none() && !height.is_none()) {
                p.size = "custom";
                p.width = width.cast<double>();
                p.height = height.cast<double>();
            }
            else {
                throw py::value_error("give a page size ('A4', 'Letter', ...) or width= and height= (mm)");
            }
            if (!orientation.is_none()) {
                const std::string o = orientation.cast<std::string>();
                if (o != "portrait" && o != "landscape") {
                    throw py::value_error("orientation: 'portrait' or 'landscape'");
                }
                if ((o == "landscape") != p.landscape() && p.width != p.height) {
                    std::swap(p.width, p.height);
                }
            }
            if (!core::pageOriginFromString(origin, p.origin)) {
                throw py::value_error("origin: 'bottom_left', 'center' or 'top_left'");
            }
            if (!color.is_none()) {
                p.hasColor = true;
                p.color = colorArg(color);
            }
            editReturningId("Page", [&](Editor& ed) {
                ed.setPage(p);
                return kNoId;
            });
        }, py::arg("size") = py::none(), py::arg("orientation") = py::none(), py::arg("width") = py::none(),
           py::arg("height") = py::none(), py::arg("origin") = "bottom_left", py::arg("color") = py::none(),
           "Put the drawing on a page: a standard size ('A0'..'A5', 'Letter', 'Legal', 'Tabloid'; orientation "
           "'portrait' (the default) or 'landscape'), or width= and height= in mm. origin: where the sketch origin "
           "is on it, 'bottom_left' (the default), 'center' or 'top_left'. color: the paper's colour (default: none). "
           "The SVG export is then the page.")
        .def("clear_page", [](const SketchH&) {
            editReturningId("Page", [&](Editor& ed) {
                ed.setPage(std::nullopt);
                return kNoId;
            });
        }, "No page: the drawing on an endless canvas (exports fit round it).")
        .def_property_readonly("texts", [](const SketchH&) {
            std::vector<Id> ids;
            for (const auto& [id, t] : sketch().texts()) {
                ids.push_back(id);
            }
            return wrapAll(ids);
        })
        .def_property_readonly("sublayers", [](const SketchH&) {
            py::list out;
            for (Id id : sketch().sublayersOf(sketch().activeLayer())) {
                out.append(SublayerH{id});
            }
            return out;
        }, "The active layer's sublayers, in order (other layers': Layer.sublayers).")
        .def_property_readonly("layers", [](const SketchH&) {
            py::list out;
            for (const core::Layer& l : sketch().layers()) {
                out.append(LayerH{l.id});
            }
            return out;
        }, "Its layers, top first.")
        .def_property(
            "layer", [](const SketchH&) { return LayerH{sketch().activeLayer()}; },
            [](const SketchH&, py::handle which) {
                const Id id = layerArg(which);
                Call call;
                call.target().setActiveLayer(id);
            },
            "The active layer (its active sublayer is where new things go); set it to a Layer or a name.")
        .def("add_layer", [](const SketchH&, const std::string& name) {
            const Id id = editReturningId("New layer", [&](Editor& ed) { return ed.addLayer(QString::fromStdString(name)); });
            return LayerH{id};
        }, py::arg("name") = "Layer", "A new layer on top, with one sublayer (made the active one).")
        .def("rename_layer", [](const SketchH&, py::handle which, const std::string& name) {
            const Id id = layerArg(which);
            editReturningId("Rename layer", [&](Editor& ed) {
                ed.renameLayer(id, QString::fromStdString(name));
                return id;
            });
        }, py::arg("layer"), py::arg("name"))
        .def("delete_layer", [](const SketchH&, py::handle which) {
            const Id id = layerArg(which);
            editReturningId("Delete layer", [&](Editor& ed) {
                ed.removeLayer(id);
                return id;
            });
        }, py::arg("layer"), "Delete a layer and everything in it (not the last one). Projections of its "
                             "geometry elsewhere stay, unlinked.")
        .def("move_layer", [](const SketchH&, py::handle which, int index) {
            const Id id = layerArg(which);
            editReturningId("Move layer", [&](Editor& ed) {
                ed.moveLayer(id, index);
                return id;
            });
        }, py::arg("layer"), py::arg("index"), "To that place in the order (0 = top).")
        .def("show_layer", [](const SketchH&, py::handle which, bool visible) {
            const Id id = layerArg(which);
            Call call;
            call.target().setLayerVisible(id, visible);
        }, py::arg("layer"), py::arg("visible") = true)
        .def("project", [](const SketchH&, py::handle items) {
            const std::vector<Id> ids = itemsOf(items);
            return editCurves("Project", [&](Editor& ed) { return ed.project(ids); });
        }, py::arg("items"),
           "Copy geometry from other layers into the active one, linked: the copies stay where the originals are "
           "(fixed, and construction until made solid). Returns the copies, in order.")
        .def("break_link", [](const SketchH&, py::handle items) {
            const std::vector<Id> ids = itemsOf(items);
            editReturningId("Break link", [&](Editor& ed) {
                ed.breakLink(ids);
                return kNoId;
            });
        }, py::arg("items"), "Make projections ordinary geometry (no longer following their originals).")
        .def_property(
            "sublayer", [](const SketchH&) { return SublayerH{sketch().activeSublayer()}; },
            [](const SketchH&, py::handle which) {
                const Id id = sublayerArg(which);
                Call call;
                call.target().setActiveSublayer(id);
            },
            "The active sublayer (new things go in it); set it to a Sublayer or a name.")
        .def("add_sublayer", [](const SketchH&, const std::string& name) {
            const Id id = editReturningId("New sublayer", [&](Editor& ed) { return ed.addSublayer(QString::fromStdString(name)); });
            return SublayerH{id};
        }, py::arg("name") = "Sublayer", "A new sublayer (made the active one; a taken name gets a number).")
        .def("rename_sublayer", [](const SketchH&, py::handle which, const std::string& name) {
            const Id id = sublayerArg(which);
            editReturningId("Rename sublayer", [&](Editor& ed) {
                ed.renameSublayer(id, QString::fromStdString(name));
                return id;
            });
        }, py::arg("sublayer"), py::arg("name"))
        .def("delete_sublayer", [](const SketchH&, py::handle which) {
            const Id id = sublayerArg(which);
            editReturningId("Delete sublayer", [&](Editor& ed) {
                ed.removeSublayer(id);
                return id;
            });
        }, py::arg("sublayer"), "Delete a sublayer and everything in it (not the last one).")
        .def("move_sublayer", [](const SketchH&, py::handle which, int index) {
            const Id id = sublayerArg(which);
            editReturningId("Move sublayer", [&](Editor& ed) {
                ed.moveSublayer(id, index);
                return id;
            });
        }, py::arg("sublayer"), py::arg("index"), "To that place in the order (0 = first).")
        .def("move_to_sublayer", [](const SketchH&, py::handle items, py::handle which) {
            const std::vector<Id> ids = itemsOf(items);
            const Id id = sublayerArg(which);
            editReturningId("Move to sublayer", [&](Editor& ed) {
                ed.moveToSublayer(ids, id);
                return id;
            });
        }, py::arg("items"), py::arg("sublayer"),
           "Move items (curves take their points, and constraints among them come too) to a sublayer.")
        .def("show_sublayer", [](const SketchH&, py::handle which, bool visible) {
            const Id id = sublayerArg(which);
            Call call;
            call.target().setSublayerVisible(id, visible);
        }, py::arg("sublayer"), py::arg("visible") = true)
        .def_property_readonly("fills", [](const SketchH&) {
            std::vector<Id> ids;
            for (const auto& [id, f] : sketch().fills()) {
                ids.push_back(id);
            }
            return wrapAll(ids);
        }, "Fills, bottom to top.")
        .def("loose_ends", [](const SketchH&) {
            py::list out;
            for (const core::Vec2& p : core::regions::Arrangement(sketch(), sketch().activeLayer()).looseEnds()) {
                out.append(py::make_tuple(p.x, p.y));
            }
            return out;
        }, "Ends of solid curves that meet nothing: where an outline may be open.")
        .def("clear_stroke", [](const SketchH&, py::handle curves) {
            const std::vector<Id> ids = itemsOf(curves);
            editReturningId("Remove stroke", [&](Editor& ed) {
                ed.clearStroke(ids);
                return kNoId;
            });
        }, py::arg("curves"), "Take curves' strokes off (they draw as plain geometry again).")

        // copies and linked features
        .def("copy", [](const SketchH&, py::handle items, double dx, double dy, double angle, py::handle about) {
            const std::vector<Id> ids = itemsOf(items);
            const core::Vec2 centre = vecArg(about);
            return editCurves("Copy", [&](Editor& ed) { return mapped(ids, ed.copy(ids, dx, dy, angle, centre)); });
        }, py::arg("items"), py::arg("dx") = 0.0, py::arg("dy") = 0.0, py::arg("angle") = 0.0,
           py::arg("about") = py::make_tuple(0, 0),
           "Copy items (with the constraints among them), turned by `angle` degrees about `about` and shifted by "
           "(dx, dy). Returns the copies, in the same order.")
        .def("scale", [](const SketchH&, py::handle items, double factor, py::handle about) {
            const std::vector<Id> ids = itemsOf(items);
            const core::Vec2 centre = vecArg(about);
            editReturningId("Scale", [&](Editor& ed) {
                ed.scale(ids, factor, centre);
                return kNoId;
            });
        }, py::arg("items"), py::arg("factor"), py::arg("about") = py::make_tuple(0, 0),
           "Scale items about a point; dimensions between them scale too.")
        .def("mirror", [](const SketchH&, py::handle items, py::handle line) {
            const std::vector<Id> ids = itemsOf(items);
            const Id across = idOf(line);
            return editCurves("Mirror", [&](Editor& ed) { return mapped(ids, ed.mirror(ids, across)); });
        }, py::arg("items"), py::arg("line"),
           "Mirror items across a line, kept symmetric to the originals. Returns the mirror images (items lying "
           "on the line have none).")
        .def("rectangular_pattern", [](const SketchH&, py::handle items, py::object count, py::object spacing,
                                       py::object angle, py::object count2, py::object spacing2, bool symmetric,
                                       bool symmetric2) {
            const std::vector<Id> ids = itemsOf(items);
            core::feature::LinearPatternParams p;
            Editor::Settings settings;
            setting(settings, "count", count, p.count);
            setting(settings, "spacing", spacing, p.spacing);
            setting(settings, "angle", angle, p.angle);
            setting(settings, "count2", count2, p.count2);
            setting(settings, "spacing2", spacing2, p.spacing2);
            p.symmetric = symmetric;
            p.symmetric2 = symmetric2;
            return wrap(editReturningId("Pattern", [&](Editor& ed) { return ed.rectangularPattern(ids, p, settings); }));
        }, py::arg("items"), py::arg("count") = 3, py::arg("spacing") = 10.0, py::arg("angle") = 0.0,
           py::arg("count2") = 1, py::arg("spacing2") = 10.0, py::arg("symmetric") = false,
           py::arg("symmetric2") = false,
           "Linked copies in rows: `count` along `angle` degrees, `count2` at right angles to it (counts include "
           "the original; symmetric= repeats them both ways). Any of the numbers may be an expression using the "
           "drawing's parameters (count=\"holes\"), which it then follows. Returns the pattern; its value is the "
           "spacing.")
        .def("circular_pattern", [](const SketchH&, py::handle items, py::handle center, py::object count,
                                    py::object angle) {
            const std::vector<Id> ids = itemsOf(items);
            const Id c = idOf(center);
            core::feature::CircularPatternParams p;
            Editor::Settings settings;
            setting(settings, "count", count, p.count);
            setting(settings, "angle", angle, p.angle);
            return wrap(editReturningId("Pattern", [&](Editor& ed) { return ed.circularPattern(ids, c, p, settings); }));
        }, py::arg("items"), py::arg("center"), py::arg("count") = 6, py::arg("angle") = 360.0,
           "Linked copies around a centre point: `count` (including the original) over `angle` degrees. A full "
           "turn spaces them evenly. Either may be an expression using the drawing's parameters. Returns the "
           "pattern; its value is the angle.")
        .def("set_pattern", [](const SketchH&, py::handle pattern, py::object count, py::object spacing,
                               py::object angle, py::object count2, py::object spacing2, py::object symmetric,
                               py::object symmetric2) {
            const Id id = idOf(pattern);
            const core::Constraint& k = constraintOf(id);
            Editor::Settings settings = k.settings;  // (those not given stay as they are)
            auto set = [](auto& field, const py::object& v) {
                if (!v.is_none()) {
                    field = v.cast<std::remove_reference_t<decltype(field)>>();
                }
            };
            if (k.type == ConstraintType::LinearPattern) {
                auto p = core::feature::linearParams(k);
                setting(settings, "count", count, p.count);
                setting(settings, "spacing", spacing, p.spacing);
                setting(settings, "angle", angle, p.angle);
                setting(settings, "count2", count2, p.count2);
                setting(settings, "spacing2", spacing2, p.spacing2);
                set(p.symmetric, symmetric);
                set(p.symmetric2, symmetric2);
                return wrap(editReturningId("Pattern", [&](Editor& ed) { return ed.setPattern(id, p, settings); }));
            }
            if (k.type == ConstraintType::CircularPattern) {
                auto p = core::feature::circularParams(k);
                setting(settings, "count", count, p.count);
                setting(settings, "angle", angle, p.angle);
                return wrap(editReturningId("Pattern", [&](Editor& ed) { return ed.setPattern(id, p, settings); }));
            }
            throw py::value_error("not a pattern");
        }, py::arg("pattern"), py::arg("count") = py::none(), py::arg("spacing") = py::none(),
           py::arg("angle") = py::none(), py::arg("count2") = py::none(), py::arg("spacing2") = py::none(),
           py::arg("symmetric") = py::none(), py::arg("symmetric2") = py::none(),
           "Change a pattern's settings (numbers, or expressions using the drawing's parameters; those not given "
           "stay as they are). A new count remakes the copies; returns the pattern (a new one then).")        .def("offset", [](const SketchH&, py::handle curves, double distance, py::object toward, bool bothSides,
                          bool caps) {
            std::vector<Id> ids = itemsOf(curves);
            if (ids.size() == 1 && !py::isinstance<py::sequence>(curves)) {
                ids = core::feature::chainAt(sketch(), ids.front());  // one curve: its whole chain
            }
            if (toward.is_none() && !bothSides) {
                throw py::value_error("say which side with toward=(x, y), or use both_sides=True");
            }
            const core::Vec2 side = toward.is_none() ? core::Vec2{} : vecArg(toward);
            return wrap(editReturningId("Offset", [&](Editor& ed) { return ed.offset(ids, distance, side, bothSides, caps); }));
        }, py::arg("curves"), py::arg("distance"), py::arg("toward") = py::none(), py::arg("both_sides") = false,
           py::arg("caps") = false,
           "Linked offset of a chain of lines/arcs (a list, or one curve for its whole chain) or a circle, on the "
           "side `toward` is on, or with both_sides=True to both sides (caps=True closes an open chain's ends with "
           "half circles). Returns the offset; its value is the distance.")
        .def("divide", [](const SketchH&, py::handle line, py::object count, py::object ratio, bool reversed) {
            const Id l = idOf(line);
            const core::feature::DivideParams p = divideArgs(count, ratio, reversed);
            return wrap(editReturningId("Divide", [&](Editor& ed) { return ed.divide(l, p); }));
        }, py::arg("line"), py::arg("count") = py::none(), py::arg("ratio") = py::none(), py::arg("reversed") = false,
           "Points along a line, kept there as it moves: `count` evenly spaced, or with ratio=\"3:2:1\" (or "
           "[3, 2, 1]) the gaps from the line's start to its end in that ratio (reversed=True: from its end). "
           "Returns the feature; its refs are the line then the points.")
        .def("set_divide", [](const SketchH&, py::handle feature, py::object count, py::object ratio,
                              py::object reversed) {
            const Id id = idOf(feature);
            const core::Constraint& k = constraintOf(id);
            if (k.type != ConstraintType::Divide) {
                throw py::value_error("not a divide");
            }
            const core::feature::DivideParams old = core::feature::divideParams(k);
            core::feature::DivideParams p = old;
            if (!count.is_none() || !ratio.is_none()) {
                p = divideArgs(count, ratio, old.reversed);
            }
            if (!reversed.is_none()) {
                p.reversed = reversed.cast<bool>();
            }
            return wrap(editReturningId("Divide", [&](Editor& ed) { return ed.setDivide(id, p); }));
        }, py::arg("divide"), py::arg("count") = py::none(), py::arg("ratio") = py::none(),
           py::arg("reversed") = py::none(),
           "Change a divide's spacing. A different number of points remakes them; returns the divide (a new one then).")
        .def("chain", [](const SketchH&, py::handle curve) { return wrapAll(core::feature::chainAt(sketch(), idOf(curve))); },
             py::arg("curve"), "The curves joined end to end with this one, in order.")
        .def("extract", [](const SketchH&, py::handle items) {
            return core::toJson(core::xform::extract(sketch(), itemsOf(items)), true);
        }, py::arg("items"), "Items and the constraints among them as JSON, as the clipboard holds them.")
        .def("paste", [](const SketchH&, const std::string& data, py::handle at) {
            core::Sketch fragment;
            try {
                fragment = core::fromJson(data);
            }
            catch (const core::SketchError& e) {
                throw py::value_error(e.what());
            }
            const core::Vec2 where = vecArg(at);
            return editCurves("Paste", [&](Editor& ed) {
                std::vector<Id> made;
                for (const auto& [from, to] : ed.paste(fragment, where)) {
                    if (fragment.hasEntity(from)) {
                        made.push_back(to);
                    }
                }
                return made;
            });
        }, py::arg("data"), py::arg("at"),
           "Insert items from extract() (or the clipboard), centred at `at`. Returns the new entities.")

        // status
        .def("solve", [](const SketchH&) { return doc().solveResult(); },
             "Solve status (the sketch is re-solved after every change).")
        .def_property_readonly("dof", [](const SketchH&) { return doc().solveResult().dof; })
        .def("__repr__", [](const SketchH&) {
            return "<Sketch " + std::to_string(sketch().entities().size()) + " items, "
                + std::to_string(sketch().constraints().size()) + " constraints>";
        });

    // --- document ----------------------------------------------------------------------------

    py::class_<Transaction>(m, "Transaction")
        .def("__enter__", [](Transaction& t) -> Transaction& {
            doc().beginTransaction();
            return t;
        })
        .def("__exit__", [](Transaction&, py::args) {
            doc().endTransaction();
            return false;
        });

    py::class_<DocumentH>(m, "Document")
        .def_property_readonly("sketch", [](const DocumentH&) { return SketchH{}; })
        .def("undo", [](const DocumentH&) {
            if (doc().inTransaction()) {
                throw std::runtime_error("can't undo while a script or transaction is running");
            }
            Document::SilentRecording silent(doc());
            doc().undo();
        })
        .def("redo", [](const DocumentH&) {
            if (doc().inTransaction()) {
                throw std::runtime_error("can't redo while a script or transaction is running");
            }
            Document::SilentRecording silent(doc());
            doc().redo();
        })
        .def_property_readonly("can_undo", [](const DocumentH&) { return doc().canUndo(); })
        .def_property_readonly("can_redo", [](const DocumentH&) { return doc().canRedo(); })
        .def("save", [](const DocumentH&, const std::string& path) {
            Document::SilentRecording silent(doc());
            QString error;
            if (!doc().save(QString::fromStdString(path), &error)) {
                throw std::runtime_error(error.toStdString());
            }
        }, py::arg("path"))
        .def("open", [](const DocumentH&, const std::string& path) {

            Document::SilentRecording silent(doc());
            QString error;
            if (!doc().open(QString::fromStdString(path), &error)) {
                throw std::runtime_error(error.toStdString());
            }
        }, py::arg("path"))
        .def("new", [](const DocumentH&) {

            Document::SilentRecording silent(doc());
            doc().newSketch();
        })
        .def_property_readonly("path", [](const DocumentH&) { return doc().filePath().toStdString(); })
        .def_property_readonly("modified", [](const DocumentH&) { return doc().isModified(); })
        .def_property_readonly("history_from_older_version", [](const DocumentH&) { return doc().historyFromOlderVersion(); },
                               "True if the history was recorded by an older version: replaying it may not rebuild the drawing exactly")
        .def("transaction", [](const DocumentH&) { return Transaction{}; },
             "with doc.transaction(): ... groups changes into one undo step")
        .def("to_json", [](const DocumentH&) { return core::toJson(sketch()); })
        .def("export_svg", [](const DocumentH&, const std::string& path, bool fills, bool undecorated,
                              bool construction, double margin, py::object canvas, bool page, bool textAsOutlines,
                              bool hatchLines, bool dimensions) {
            core::exporter::SvgOptions o;
            o.fills = fills;
            o.undecorated = undecorated;
            o.construction = construction;
            o.margin = margin;
            o.page = page;
            o.textAsOutlines = textAsOutlines;
            o.hatchLines = hatchLines;
            o.dimensions = dimensions;
            if (!canvas.is_none()) {
                o.canvas = canvas.cast<std::array<double, 4>>();
            }
            Document::SilentRecording quiet(doc());
            QString error;
            if (!doc().exportSvg(QString::fromStdString(path), o, &error)) {
                throw std::runtime_error(error.toStdString());
            }
        }, py::arg("path"), py::arg("fills") = true, py::arg("undecorated") = true, py::arg("construction") = false,
           py::arg("margin") = 2.0, py::arg("canvas") = py::none(), py::arg("page") = true,
           py::arg("text_as_outlines") = false, py::arg("hatch_lines") = false, py::arg("dimensions") = true,
           "Write the drawing as SVG, in millimetres: fills, strokes, and (undecorated=True) curves without a "
           "stroke in thin black; construction only if asked; text as text in its font (text_as_outlines=True: "
           "as its letters' shapes); hatches as <pattern>s (hatch_lines=True: their lines, cut to their areas); "
           "the annotations (dimensions=True). With a page set (and page=True), the SVG is the page; else fitted round the "
           "drawing with `margin`. canvas=(x0, y0, x1, y1) frames a fixed area in its own units instead (for icons).")
        .def("export_pdf", [](const DocumentH&, const std::string& path, bool fills, bool undecorated,
                              bool construction, double margin, bool page, bool textAsOutlines, bool dimensions) {
            core::exporter::SvgOptions o;
            o.fills = fills;
            o.undecorated = undecorated;
            o.construction = construction;
            o.margin = margin;
            o.page = page;
            o.textAsOutlines = textAsOutlines;
            o.dimensions = dimensions;
            Document::SilentRecording quiet(doc());
            QString error;
            if (!doc().exportPdf(QString::fromStdString(path), o, &error)) {
                throw std::runtime_error(error.toStdString());
            }
        }, py::arg("path"), py::arg("fills") = true, py::arg("undecorated") = true, py::arg("construction") = false,
           py::arg("margin") = 2.0, py::arg("page") = true, py::arg("text_as_outlines") = false,
           py::arg("dimensions") = true,
           "Write the drawing as a PDF, at true size: with a page set (and page=True), the PDF's page is it (what's "
           "off it cut off); else a page fitted round the drawing with `margin`. As export_svg draws it, text as "
           "selectable text in its embedded font (text_as_outlines=True: as its letters' shapes), hatches as lines.")
        .def("export_image", [](const DocumentH&, const std::string& path, double dpi, bool transparent, int quality,
                                bool fills, bool undecorated, bool construction, double margin, bool page,
                                bool dimensions) {
            core::exporter::SvgOptions o;
            o.fills = fills;
            o.undecorated = undecorated;
            o.construction = construction;
            o.margin = margin;
            o.page = page;
            o.dimensions = dimensions;
            Document::ImageOptions image;
            image.dpi = dpi;
            image.transparent = transparent;
            image.quality = quality;
            Document::SilentRecording quiet(doc());
            QString error;
            if (!doc().exportImage(QString::fromStdString(path), o, image, &error)) {
                throw std::runtime_error(error.toStdString());
            }
        }, py::arg("path"), py::arg("dpi") = 300.0, py::arg("transparent") = false, py::arg("quality") = 92,
           py::arg("fills") = true, py::arg("undecorated") = true, py::arg("construction") = false,
           py::arg("margin") = 2.0, py::arg("page") = true, py::arg("dimensions") = true,
           "Write the drawing as a PNG or JPEG (by the path's suffix: .png, .jpg or .jpeg), as export_pdf draws it, "
           "at `dpi` (saved in the file, so it prints at true size). transparent=True: a PNG with nothing behind "
           "the drawing (else white; a page's colour shows either way). quality: the JPEG's, 1-100.")
        .def("to_svg", [](const DocumentH&, bool fills, bool undecorated, bool construction, double margin, bool page,
                          bool textAsOutlines, bool hatchLines, bool dimensions) {
            core::exporter::SvgOptions o;
            o.fills = fills;
            o.undecorated = undecorated;
            o.construction = construction;
            o.margin = margin;
            o.page = page;
            o.textAsOutlines = textAsOutlines;
            o.hatchLines = hatchLines;
            o.dimensions = dimensions;
            try {
                return core::exporter::toSvg(sketch(), o);
            }
            catch (const core::SketchError& e) {
                throw py::value_error(e.what());
            }
        }, py::arg("fills") = true, py::arg("undecorated") = true, py::arg("construction") = false,
           py::arg("margin") = 2.0, py::arg("page") = true, py::arg("text_as_outlines") = false,
           py::arg("hatch_lines") = false, py::arg("dimensions") = true, "The SVG export as a string (see export_svg).")
        .def("export_dxf", [](const DocumentH&, const std::string& path, bool styles, bool construction, bool fills,
                              bool textAsOutlines, bool dimensions) {
            core::exporter::DxfOptions o;
            o.styles = styles;
            o.construction = construction;
            o.fills = fills;
            o.textAsOutlines = textAsOutlines;
            o.dimensions = dimensions;
            Document::SilentRecording quiet(doc());
            QString error;
            if (!doc().exportDxf(QString::fromStdString(path), o, &error)) {
                throw std::runtime_error(error.toStdString());
            }
        }, py::arg("path"), py::arg("styles") = true, py::arg("construction") = false, py::arg("fills") = true,
           py::arg("text_as_outlines") = false, py::arg("dimensions") = true,
           "Write the drawing as DXF (2004, millimetres): every curve exact (lines, circles, arcs, ellipses, "
           "splines); strokes' colours, line styles and weights (styles=True); fills as hatches; text in its font "
           "(text_as_outlines=True: its letters' shapes); the annotations, drawn out on a DIMENSIONS layer "
           "(dimensions=True); construction on a switched-off layer if asked.")
        .def("to_dxf", [](const DocumentH&, bool styles, bool construction, bool fills, bool textAsOutlines,
                          bool dimensions) {
            core::exporter::DxfOptions o;
            o.styles = styles;
            o.construction = construction;
            o.fills = fills;
            o.textAsOutlines = textAsOutlines;
            o.dimensions = dimensions;
            try {
                return core::exporter::toDxf(sketch(), o);
            }
            catch (const core::SketchError& e) {
                throw py::value_error(e.what());
            }
        }, py::arg("styles") = true, py::arg("construction") = false, py::arg("fills") = true,
           py::arg("text_as_outlines") = false, py::arg("dimensions") = true, "The DXF export as a string.")
        .def_property_readonly("history", [](const DocumentH&) {
            py::list out;
            for (const QString& entry : doc().history()) {
                out.append(entry.toStdString());
            }
            return out;
        }, "The Python commands that built the sketch, one per edit (saved with it; undo takes "
           "commands off). Run them in a new document to rebuild it.");

    m.def("document", []() {
        doc();  // raises if there is none
        return DocumentH{};
    }, "The document scripts act on.");

    // --- UI (only when running with a window) ------------------------------------------------

    py::class_<AppH>(m, "App")
        .def_property_readonly("has_ui", [](const AppH&) { return static_cast<bool>(g_ui.fit); })
        .def("fit", [](const AppH&) { ui().fit(); }, "Zoom to show the whole drawing.")
        .def("fonts", [](const AppH&, const std::string& search) {
            std::vector<std::string> out;
            for (const QString& f : textshapes::families(QString::fromStdString(search))) {
                out.push_back(f.toStdString());
            }
            return out;
        }, py::arg("search") = "", "Installed font families whose names contain `search` (any case).")
        .def_property(
            "selection", [](const AppH&) {
                const auto ids = ui().selection();
                return wrapAll({ids.begin(), ids.end()});
            },
            [](const AppH&, py::handle items) {
                std::set<Id> ids;
                for (py::handle h : items) {
                    ids.insert(idOf(h));
                }
                ui().select(ids);
            })
        .def_property(
            "tool", [](const AppH&) { return ui().tool().toStdString(); },
            [](const AppH&, const std::string& name) {
                if (!ui().setTool(QString::fromStdString(name))) {
                    throw py::value_error("unknown tool '" + name + "'");
                }
            },
            "The active tool: select, line, rectangle, circle, arc, arc3, dimension, coincident, ...")
        .def_property(
            "presentation", [](const AppH&) { return ui().presentation(); },
            [](const AppH&, bool on) { ui().setPresentation(on); },
            "Presentation view: the drawing as it will look (strokes at full strength, no constraints).")
        .def("message", [](const AppH&, const std::string& text) { ui().message(QString::fromStdString(text)); });
    m.attr("app") = AppH{};

    // Output hook used by the host's sys.stdout / sys.stderr replacements.
    m.def("_emit", [](const std::string& text, bool isError) {
        const QString t = QString::fromStdString(text);
        if (g_output) {
            g_output(t, isError);
        }
        else {
            std::fputs(text.c_str(), isError ? stderr : stdout);
            std::fflush(isError ? stderr : stdout);
        }
    });
}

// =============================================================================================
// Host
// =============================================================================================

namespace script
{
namespace
{
// Python-side helpers: output redirection, running code, and the console's name lookup.
constexpr const char* kHelpers = R"PY(
import builtins
import os
import re
import sys
import traceback

import setsquare


class _Writer:
    def __init__(self, is_error):
        self._is_error = is_error

    def write(self, text):
        if text:
            setsquare._emit(text, self._is_error)
        return len(text)

    def flush(self):
        pass

    def isatty(self):
        return False


sys.stdout = _Writer(False)
sys.stderr = _Writer(True)


def base_globals(name, path=None):
    g = {
        "__name__": name,
        "__builtins__": builtins,
        "setsquare": setsquare,
        "doc": setsquare.document(),
        "s": setsquare.document().sketch,
        "app": setsquare.app,
    }
    if path:
        g["__file__"] = path
    return g


def _run(source, filename, mode, g, l):
    try:
        code = compile(source, filename, mode)
    except SyntaxError:
        if mode != "single":
            traceback.print_exc(limit=0)
            return 1
        try:
            code = compile(source, filename, "exec")  # several statements pasted at once
        except SyntaxError:
            traceback.print_exc(limit=0)
            return 1
    try:
        exec(code, g, l)
        return 0
    except SystemExit as e:
        if e.code is None:
            return 0
        if isinstance(e.code, int):
            return e.code
        print(e.code, file=sys.stderr)
        return 1
    except BaseException:
        etype, value, tb = sys.exc_info()
        traceback.print_exception(etype, value, tb.tb_next)  # skip this frame
        return 1


def run_source(source, name):
    g = base_globals("__main__")
    return _run(source, name, "exec", g, g)


def run_file(path, argv):
    try:
        with open(path, encoding="utf-8-sig") as f:  # -sig: Windows editors may add a BOM
            source = f.read()
    except OSError as e:
        print(f"can't read {path}: {e}", file=sys.stderr)
        return 1
    g = base_globals("__main__", path)
    old_argv, old_path = sys.argv, list(sys.path)
    sys.argv = [path] + list(argv)
    sys.path.insert(0, os.path.dirname(os.path.abspath(path)))
    try:
        return _run(source, path, "exec", g, g)
    finally:
        sys.argv, sys.path[:] = old_argv, old_path


_NAME = re.compile(r"^(sp|ea|[plcakdftein])(\d+)$")


class ConsoleNamespace(dict):
    """Console variables. Unknown names like p12, l7, d20 resolve to sketch items by id, so
    recorded actions can be pasted back in."""

    def __missing__(self, key):
        m = _NAME.match(key)
        if m:
            try:
                return setsquare.document().sketch[int(m.group(2))]
            except (KeyError, ValueError):
                pass
        raise KeyError(key)


def new_console():
    return base_globals("__console__"), ConsoleNamespace()


def run_console(source, g, l):
    return _run(source, "<console>", "single", g, l)
)PY";

QString defaultHome()
{
    return QDir(QCoreApplication::applicationDirPath()).filePath(QStringLiteral("python"));
}

RunResult toResult(int code)
{
    RunResult r;
    r.exitCode = code;
    r.ok = code == 0;
    return r;
}

// Runs fn with the document grouped into one undo step (if there is a document).
template<typename F>
RunResult inTransaction(bool transaction, F&& fn)
{
    if (!g_initialised) {
        RunResult r;
        r.ok = false;
        r.exitCode = 1;
        r.error = QStringLiteral("Python is not initialised");
        return r;
    }
    if (transaction && g_doc) {
        g_doc->beginTransaction();
    }
    int code = 1;
    try {
        code = fn();
    }
    catch (const py::error_already_set& e) {
        if (g_output) {
            g_output(QString::fromStdString(e.what()) + QLatin1Char('\n'), true);
        }
        else {
            std::fprintf(stderr, "%s\n", e.what());
        }
    }
    if (transaction && g_doc) {
        g_doc->endTransaction();
    }
    return toResult(code);
}
}  // namespace

bool initialise(QString* error, const QString& pythonHome)
{
    if (g_initialised) {
        return true;
    }
    const QString home = pythonHome.isEmpty() ? defaultHome() : pythonHome;
    if (!QFileInfo(home).isDir()) {
        if (error) {
            *error = QStringLiteral("Python runtime not found at %1").arg(QDir::toNativeSeparators(home));
        }
        return false;
    }

    PyConfig config;
    PyConfig_InitIsolatedConfig(&config);  // ignore PYTHONPATH etc. and any installed Python
    config.write_bytecode = 0;
    config.install_signal_handlers = 0;
    config.site_import = 0;
    const std::wstring homeW = QDir::toNativeSeparators(home).toStdWString();
    PyStatus status = PyConfig_SetString(&config, &config.home, homeW.c_str());
    if (PyStatus_Exception(status)) {
        PyConfig_Clear(&config);
        if (error) {
            *error = QStringLiteral("Couldn't configure Python");
        }
        return false;
    }
    try {
        py::initialize_interpreter(&config, 0, nullptr, false);
    }
    catch (const std::exception& e) {
        if (error) {
            *error = QStringLiteral("Couldn't start Python: %1").arg(QString::fromUtf8(e.what()));
        }
        return false;
    }

    try {
        g_objects = std::make_unique<HostObjects>();
        py::module_ helpers = py::module_::import("types").attr("ModuleType")("_setsquare_host");
        py::exec(kHelpers, helpers.attr("__dict__"));
        g_objects->helpers = helpers;
    }
    catch (const py::error_already_set& e) {
        if (error) {
            *error = QStringLiteral("Couldn't set up the Python environment: %1").arg(QString::fromUtf8(e.what()));
        }
        g_objects.reset();
        py::finalize_interpreter();
        return false;
    }
    g_initialised = true;
    return true;
}

void shutdown()
{
    if (!g_initialised) {
        return;
    }
    g_objects.reset();
    py::finalize_interpreter();
    g_initialised = false;
}

bool isInitialised()
{
    return g_initialised;
}

void setDocument(Document* document)
{
    g_doc = document;
}

Document* document()
{
    return g_doc;
}

void setUiHooks(UiHooks hooks)
{
    g_ui = std::move(hooks);
}

void setOutput(std::function<void(const QString&, bool)> sink)
{
    g_output = std::move(sink);
}

RunResult runFile(const QString& path, const QStringList& argv)
{
    return inTransaction(true, [&] {
        py::list args;
        for (const QString& a : argv) {
            args.append(a.toStdString());
        }
        return g_objects->helpers.attr("run_file")(path.toStdString(), args).cast<int>();
    });
}

RunResult runSource(const QString& source, const QString& name)
{
    return inTransaction(true, [&] {
        return g_objects->helpers.attr("run_source")(source.toStdString(), name.toStdString()).cast<int>();
    });
}

RunResult runConsole(const QString& source)
{
    // Not one transaction: each API call is its own undo step, exactly like the UI action it
    // may have been recorded from (so doc.undo() works from the console too).
    return inTransaction(false, [&] {
        if (!g_objects->consoleLocals) {
            resetConsole();
        }
        return g_objects->helpers.attr("run_console")(source.toStdString(), g_objects->consoleGlobals,
                                                      g_objects->consoleLocals)
            .cast<int>();
    });
}

void resetConsole()
{
    if (!g_initialised || !g_doc) {
        return;
    }
    py::tuple ns = g_objects->helpers.attr("new_console")();
    g_objects->consoleGlobals = ns[0].cast<py::dict>();
    g_objects->consoleLocals = ns[1];
}

}  // namespace script
