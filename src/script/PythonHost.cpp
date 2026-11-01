// The embedded interpreter and the `drawapp` Python module.
//
// The module lives in this file (not a separate one) because PYBIND11_EMBEDDED_MODULE registers
// itself from a static initialiser, and a linker would drop an otherwise-unreferenced object
// file from a static library.

#include "PythonHost.h"

#include "Document.h"
#include "Editor.h"

#include <core/Dimensions.h>
#include <core/Features.h>
#include <core/Modify.h>
#include <core/Serialize.h>
#include <core/Solver.h>
#include <core/Transform.h>

// Qt's `slots` macro collides with a struct member in Python's object.h.
#pragma push_macro("slots")
#undef slots
#include <pybind11/embed.h>
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
    py::object helpers;  // the _drawapp_host helper module
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
struct ConstraintH : Item
{};

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

py::object wrap(Id id)
{
    const core::Sketch& s = sketch();
    if (s.hasConstraint(id)) {
        return py::cast(ConstraintH{{id}});
    }
    switch (entity(id).kind()) {
        case EntityKind::Point: return py::cast(PointH{{id}});
        case EntityKind::Line: return py::cast(LineH{{id}});
        case EntityKind::Circle: return py::cast(CircleH{{id}});
        case EntityKind::Arc: return py::cast(ArcH{{id}});
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

// Adds a (checked) constraint or dimension; refuses over-constraining like the UI does.
py::object addConstraint(ConstraintType type, std::vector<Id> refs, py::object value, py::object label,
                         std::uint8_t flip = 0)
{
    double v = 0.0;
    if (core::isDimension(type)) {
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

}  // namespace
}  // namespace script

// =============================================================================================
// The module
// =============================================================================================

PYBIND11_EMBEDDED_MODULE(drawapp, m)
{
    using namespace script;
    m.doc() = "Scripting API for the drawing app. Everything the UI does can be done from here.";

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

    // --- items -------------------------------------------------------------------------------

    py::class_<Item>(m, "Item", "Base of all sketch items.")
        .def_property_readonly("id", [](const Item& i) { return i.id; })
        .def_property_readonly("name", [](const Item& i) { return itemName(i.id); },
                               "The name used in recorded scripts, e.g. 'l7'.")
        .def_property_readonly("exists", [](const Item& i) {
            return sketch().hasEntity(i.id) || sketch().hasConstraint(i.id);
        })
        .def("__eq__", [](const Item& a, py::handle b) { return py::isinstance<Item>(b) && a.id == b.cast<Item&>().id; })
        .def("__hash__", [](const Item& i) { return py::hash(py::int_(i.id)); });

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
                d["width"] = style->width;
                d["dash"] = std::string(core::toString(style->dash));
                return d;
            }, "The curve's stroke as {'color', 'width', 'dash'}, or None (see Sketch.stroke).");
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

    py::class_<ConstraintH, Item>(m, "Constraint")
        .def_property_readonly("type", [](const ConstraintH& c) {
            return Editor::function(constraintOf(c.id).type).toStdString();
        })
        .def_property_readonly("refs", [](const ConstraintH& c) { return wrapAll(constraintOf(c.id).refs); })
        .def_property_readonly("is_dimension", [](const ConstraintH& c) { return core::isDimension(constraintOf(c.id).type); })
        .def_property_readonly("is_feature", [](const ConstraintH& c) { return core::isFeature(constraintOf(c.id).type); },
                               "A pattern or offset.")
        .def_property_readonly("sources", [](const ConstraintH& c) {
            const core::Constraint& k = constraintOf(c.id);
            if (k.type == ConstraintType::Offset) {
                return wrapAll(core::feature::offsetRefs(k).sources);
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
            [](const ConstraintH& c, double v) {
                constraintOf(c.id);
                Call call;
                QString error;
                if (!call.target().setDimensionValue(c.id, v, &error)) {
                    throw py::value_error(error.toStdString());
                }
            },
            "A dimension's value (lengths, or degrees for angles), a pattern's spacing or total angle, or an "
            "offset's distance. Setting it re-solves.")
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
        .def("__getitem__", [](const SketchH&, Id id) {
            if (!sketch().hasEntity(id) && !sketch().hasConstraint(id)) {
                throw py::key_error("no item " + std::to_string(id));
            }
            return wrap(id);
        })
        .def("__contains__", [](const SketchH&, py::handle h) {
            const Id id = idOf(h);
            return sketch().hasEntity(id) || sketch().hasConstraint(id);
        })
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
                    if (ed.sketch().hasEntity(id) || ed.sketch().hasConstraint(id)) {
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
        .def("symmetric", constraintFn(ConstraintType::Symmetric), "(point, point, line)")
        .def("collinear", constraintFn(ConstraintType::Collinear), "(line, line)")

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
            call.target().endDrag();
        }, py::arg("items"), py::arg("dx") = 0.0, py::arg("dy") = 0.0, py::arg("angle") = 0.0,
           py::arg("about") = py::make_tuple(0, 0),
           "Move items rigidly: turn by `angle` degrees about `about`, then shift by (dx, dy). Connected geometry "
           "follows as the constraints allow, as when dragging.")

        // appearance
        .def("stroke", [](const SketchH&, py::handle curves, py::object color, py::object width, py::object dash) {
            const std::vector<Id> ids = itemsOf(curves);
            // What isn't given stays as the curve has it (or the default for a new stroke).
            std::vector<std::pair<core::StrokeStyle, std::vector<Id>>> groups;
            for (Id id : ids) {
                entity(id);
                core::StrokeStyle style = sketch().stroke(id) ? *sketch().stroke(id) : core::StrokeStyle{};
                if (!color.is_none()) {
                    style.color = colorArg(color);
                }
                if (!width.is_none()) {
                    style.width = width.cast<double>();
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
           "(0 = hairline), dash 'solid', 'dashed', 'dotted', 'dash_dot' or 'dash_dot_dot'.")
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
        .def("rectangular_pattern", [](const SketchH&, py::handle items, int count, double spacing, double angle,
                                       int count2, double spacing2, bool symmetric, bool symmetric2) {
            const std::vector<Id> ids = itemsOf(items);
            const core::feature::LinearPatternParams p{count, spacing, angle, count2, spacing2, symmetric, symmetric2};
            return wrap(editReturningId("Pattern", [&](Editor& ed) { return ed.rectangularPattern(ids, p); }));
        }, py::arg("items"), py::arg("count") = 3, py::arg("spacing") = 10.0, py::arg("angle") = 0.0,
           py::arg("count2") = 1, py::arg("spacing2") = 10.0, py::arg("symmetric") = false,
           py::arg("symmetric2") = false,
           "Linked copies in rows: `count` along `angle` degrees, `count2` at right angles to it (counts include "
           "the original; symmetric= repeats them both ways). Returns the pattern; its value is the spacing.")
        .def("circular_pattern", [](const SketchH&, py::handle items, py::handle center, int count, double angle) {
            const std::vector<Id> ids = itemsOf(items);
            const Id c = idOf(center);
            const core::feature::CircularPatternParams p{count, angle};
            return wrap(editReturningId("Pattern", [&](Editor& ed) { return ed.circularPattern(ids, c, p); }));
        }, py::arg("items"), py::arg("center"), py::arg("count") = 6, py::arg("angle") = 360.0,
           "Linked copies around a centre point: `count` (including the original) over `angle` degrees. A full "
           "turn spaces them evenly. Returns the pattern; its value is the angle.")
        .def("set_pattern", [](const SketchH&, py::handle pattern, py::object count, py::object spacing,
                               py::object angle, py::object count2, py::object spacing2, py::object symmetric,
                               py::object symmetric2) {
            const Id id = idOf(pattern);
            const core::Constraint& k = constraintOf(id);
            auto set = [](auto& field, const py::object& v) {
                if (!v.is_none()) {
                    field = v.cast<std::remove_reference_t<decltype(field)>>();
                }
            };
            if (k.type == ConstraintType::LinearPattern) {
                auto p = core::feature::linearParams(k);
                set(p.count, count);
                set(p.spacing, spacing);
                set(p.angle, angle);
                set(p.count2, count2);
                set(p.spacing2, spacing2);
                set(p.symmetric, symmetric);
                set(p.symmetric2, symmetric2);
                return wrap(editReturningId("Pattern", [&](Editor& ed) { return ed.setPattern(id, p); }));
            }
            if (k.type == ConstraintType::CircularPattern) {
                auto p = core::feature::circularParams(k);
                set(p.count, count);
                set(p.angle, angle);
                return wrap(editReturningId("Pattern", [&](Editor& ed) { return ed.setPattern(id, p); }));
            }
            throw py::value_error("not a pattern");
        }, py::arg("pattern"), py::arg("count") = py::none(), py::arg("spacing") = py::none(),
           py::arg("angle") = py::none(), py::arg("count2") = py::none(), py::arg("spacing2") = py::none(),
           py::arg("symmetric") = py::none(), py::arg("symmetric2") = py::none(),
           "Change a pattern's settings. A new count remakes the copies; returns the pattern (a new one then).")
        .def("offset", [](const SketchH&, py::handle curves, double distance, py::object toward, bool bothSides,
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
        .def("transaction", [](const DocumentH&) { return Transaction{}; },
             "with doc.transaction(): ... groups changes into one undo step")
        .def("to_json", [](const DocumentH&) { return core::toJson(sketch()); })
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

import drawapp


class _Writer:
    def __init__(self, is_error):
        self._is_error = is_error

    def write(self, text):
        if text:
            drawapp._emit(text, self._is_error)
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
        "drawapp": drawapp,
        "doc": drawapp.document(),
        "s": drawapp.document().sketch,
        "app": drawapp.app,
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


_NAME = re.compile(r"^([plcakd])(\d+)$")


class ConsoleNamespace(dict):
    """Console variables. Unknown names like p12, l7, d20 resolve to sketch items by id, so
    recorded actions can be pasted back in."""

    def __missing__(self, key):
        m = _NAME.match(key)
        if m:
            try:
                return drawapp.document().sketch[int(m.group(2))]
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
        py::module_ helpers = py::module_::import("types").attr("ModuleType")("_drawapp_host");
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
