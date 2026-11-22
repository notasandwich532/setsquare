// Text in Move/Copy, Mirror and the patterns: it goes with what it hangs on, turns as it turns, is
// mirrored still readable, and a pattern's copies follow their original.

#include "Document.h"
#include "Editor.h"
#include "PythonHost.h"
#include "SketchView.h"
#include "TextShapes.h"

#include <core/Features.h>
#include <core/Serialize.h>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <QRectF>

#include <cmath>
#include <functional>
#include <numbers>

using namespace core;
using Catch::Approx;

namespace
{
// A text's letters where they are in the drawing: their box, and which way round their outlines
// go overall (a mirror image's go the other way).
struct Footprint
{
    QRectF box;
    double winding = 0.0;  // sum of the contours' signed areas
};
Footprint footprint(const Sketch& s, Id id)
{
    const Text& t = s.text(id);
    const Point& anchor = s.point(t.anchor);
    Footprint f;
    double x0 = 1e300, y0 = 1e300, x1 = -1e300, y1 = -1e300;
    for (const auto& contour : t.shape) {
        for (size_t i = 0; i < contour.size(); ++i) {
            const Point a = textToWorld(anchor, t.angle, contour[i]);
            const Point b = textToWorld(anchor, t.angle, contour[(i + 1) % contour.size()]);
            f.winding += a.x * b.y - b.x * a.y;
            x0 = std::min(x0, a.x);
            y0 = std::min(y0, a.y);
            x1 = std::max(x1, a.x);
            y1 = std::max(y1, a.y);
        }
    }
    f.box = QRectF(QPointF(x0, y0), QPointF(x1, y1));
    return f;
}

Text text(const QString& words, double height = 5)
{
    Text t;
    t.text = words.toStdString();
    t.font = textshapes::defaultFamily().toStdString();
    t.height = height;
    return t;
}

// An edit that may be refused: whether it was.
bool tryEdit(Document& doc, const char* label, const std::function<void(Editor&)>& f)
{
    try {
        doc.edit(label, f);
        return true;
    }
    catch (const SketchError&) {
        return false;
    }
}
}  // namespace

TEST_CASE("Text in Mirror: where the mirror puts it, but readable (point, box, along a line, along an arc)", "[text][transform]")
{
    Document doc;
    Id axis = kNoId, original = kNoId;
    const int kind = GENERATE(0, 1, 2, 3);
    INFO("kind " << kind);
    Text t = text("Mirror me");
    doc.modify("setup", [&](Sketch& s) {
        axis = s.addLine(0, -40, 0, 60);
        if (kind == 0) {
            t.anchor = s.addPoint(8, 30);
        }
        else if (kind == 1) {
            const Id a = s.addPoint(8, 10), b = s.addPoint(50, 10), c = s.addPoint(50, 24), d = s.addPoint(8, 24);
            for (auto [p, q] : {std::pair{a, b}, {b, c}, {c, d}, {d, a}}) {
                s.entity(s.addLine(p, q)).construction = true;
            }
            t.anchor = a;
            t.boxSide = b;
            t.boxCorner = c;
        }
        else if (kind == 2) {
            const Id l = s.addLine(8, -20, 48, -12);
            t.path = l;
            t.anchor = s.definingPoints(l).front();
        }
        else {
            const Id arc = s.addArc(25, -30, 15, 0, 180);
            t.path = arc;
            t.anchor = s.definingPoints(arc).front();
        }
    });
    doc.edit("text", [&](Editor& ed) { original = ed.text(t); });    std::map<Id, Id> made;
    REQUIRE(tryEdit(doc, "mirror", [&](Editor& ed) { made = ed.mirror({original}, axis); }));
    REQUIRE(made.contains(original));
    const Footprint a = footprint(doc.sketch(), original), b = footprint(doc.sketch(), made.at(original));
    // Where the mirror puts it. (Along a sloped or curved path the box's edges are set by the
    // letters at its ends, and read the right way round, those are the other letters: a little
    // more leeway. On the wrong side of its path, it'd be out by the text's whole height.)
    const double leeway = kind >= 2 ? 2.0 : 0.6;
    CHECK(b.box.left() == Approx(-a.box.right()).margin(leeway));
    CHECK(b.box.right() == Approx(-a.box.left()).margin(leeway));
    CHECK(b.box.top() == Approx(a.box.top()).margin(leeway));
    CHECK(b.box.bottom() == Approx(a.box.bottom()).margin(leeway));
    // Readable: its outlines wind as the original's do (a mirror image's would go the other way).
    CHECK((a.winding > 0) == (b.winding > 0));
    CHECK(std::abs(b.winding) == Approx(std::abs(a.winding)).epsilon(0.05));
    CHECK(doc.solveResult().ok);
}

TEST_CASE("Text in Move/Copy: with its holder, turned as it turns; dragging a turned text turns it", "[text][transform]")
{
    Document doc;
    Id t = kNoId;
    doc.edit("text", [&](Editor& ed) {
        Text x = text("Turn");
        x.anchor = ed.point(10, 0);
        t = ed.text(x);
    });
    std::map<Id, Id> made;
    REQUIRE(tryEdit(doc, "copy", [&](Editor& ed) { made = ed.copy({t}, 0, 0, 90, {0, 0}); }));
    const Text& copy = doc.sketch().text(made.at(t));
    CHECK(copy.angle == Approx(90.0));
    CHECK(doc.sketch().point(copy.anchor).x == Approx(0.0).margin(1e-9));
    CHECK(doc.sketch().point(copy.anchor).y == Approx(10.0));

    // Moved (as Move/Copy drags): the text turns with its point.
    doc.beginDrag({doc.sketch().text(t).anchor});
    doc.dragMove(0, 0, 45, {0, 0});
    doc.endDrag();
    CHECK(doc.sketch().text(t).angle == Approx(45.0));
    CHECK(doc.history().last().startsWith("s.move("));
}

TEST_CASE("Text in a pattern: copies follow their original, the count, and deleting", "[text][transform][pattern]")
{
    Document doc;
    Id t = kNoId, k = kNoId;
    doc.edit("text", [&](Editor& ed) {
        Text x = text("Ring");
        x.anchor = ed.point(10, 0);
        t = ed.text(x);
    });
    auto copies = [&] {
        std::vector<const Text*> out;
        for (const auto& [id, x] : doc.sketch().texts()) {
            if (x.isPatternCopy()) {
                out.push_back(&x);
            }
        }
        return out;
    };
    feature::CircularPatternParams p;
    p.count = 4;
    REQUIRE(tryEdit(doc, "pattern", [&](Editor& ed) { k = ed.circularPattern({t}, doc.sketch().origin(), p); }));
    REQUIRE(copies().size() == 3);
    for (const Text* c : copies()) {
        CHECK(c->text == "Ring");
        const Point& at = doc.sketch().point(c->anchor);
        CHECK(std::fmod(c->angle - std::atan2(at.y, at.x) * 180 / std::numbers::pi + 720, 360.0) == Approx(0.0).margin(1e-6));
    }

    // The original edited: the copies follow (and can't be edited on their own).
    Text changed = doc.sketch().text(t);
    changed.text = "Round";
    changed.height = 7;
    REQUIRE(tryEdit(doc, "edit", [&](Editor& ed) { ed.setText(changed); }));
    for (const Text* c : copies()) {
        CHECK(c->text == "Round");
        CHECK(c->height == 7);
        CHECK(c->shape.size() == doc.sketch().text(t).shape.size());  // (the same letters)
        CHECK(c->shape.front().size() == doc.sketch().text(t).shape.front().size());
    }
    Text own = *copies().front();
    own.text = "Mine";
    CHECK(!tryEdit(doc, "edit copy", [&](Editor& ed) { ed.setText(own); }));

    // The count: copies made and dropped with it. Saved and loaded, still linked.
    p.count = 6;
    REQUIRE(tryEdit(doc, "count", [&](Editor& ed) { k = ed.setPattern(k, p); }));
    CHECK(copies().size() == 5);
    const Sketch loaded = fromJson(toJson(doc.sketch()));
    CHECK(std::ranges::count_if(loaded.texts(), [](const auto& kv) { return kv.second.isPatternCopy(); }) == 5);
    CHECK(loaded.constraint(k).texts == std::vector<Id>{t});

    // A copy deleted: the original no longer repeated (its copies all go; it stays).
    REQUIRE(tryEdit(doc, "delete copy", [&](Editor& ed) { ed.remove(copies().front()->id); }));
    CHECK(copies().empty());
    CHECK(doc.sketch().hasText(t));
    CHECK(doc.sketch().hasConstraint(k));
}

TEST_CASE("Text in Move/Copy, Mirror and patterns from Python, and the history replays it", "[text][transform][scripting]")
{
    Document doc;
    script::setDocument(&doc);
    const script::RunResult r = script::runConsole(
        "t = s.text('Hi', (10, 0), height=5)\n"
        "s.move([t], angle=90, about=(0, 0))\n"
        "s.copy([t], dx=20)\n"
        "axis = s.line((50, -10), (50, 10))\n"
        "s.mirror([t], axis)\n"
        "k = s.circular_pattern([t], s.origin, count=3)\n");
    script::setDocument(nullptr);
    INFO(r.error.toStdString());
    REQUIRE(r.ok);
    CHECK(doc.sketch().texts().size() == 5);

    Document replay;
    script::setDocument(&replay);
    const script::RunResult again = script::runConsole(doc.history().join('\n'));
    script::setDocument(nullptr);
    INFO(again.error.toStdString());
    REQUIRE(again.ok);
    REQUIRE(replay.sketch().texts().size() == 5);
    for (const auto& [id, x] : doc.sketch().texts()) {
        REQUIRE(replay.sketch().hasText(id));
        CHECK(replay.sketch().text(id).angle == Approx(x.angle).margin(1e-6));
        CHECK(replay.sketch().text(id).align == x.align);
    }
}