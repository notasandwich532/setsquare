#pragma once

#include <core/Builders.h>
#include <core/Features.h>
#include <core/Modify.h>
#include <core/Sketch.h>
#include <core/Transform.h>

#include <QString>
#include <QStringList>

#include <array>
#include <map>
#include <vector>

// The sketch-editing commands shared by the UI tools and the Python API. Each one edits the
// sketch and appends the equivalent Python to a log, which is how UI actions get recorded:
// anything a tool does through an Editor can be replayed as a script.
//
// Recorded Python names things by id: p12 (point), l13 (line), c14 (circle), a15 (arc),
// k16 (constraint), d17 (dimension), s.origin. In the console those names resolve to the live
// objects, so recorded lines can be pasted back in.
class Editor
{
public:
    Editor(core::Sketch& sketch, QStringList* log)
        : m_sketch(sketch)
        , m_log(log)
    {}

    const core::Sketch& sketch() const { return m_sketch; }

    core::Id point(double x, double y);
    core::Id line(core::Id a, core::Id b);
    core::Id circle(core::Id center, double radius);
    core::Id arc(core::Id center, core::Id start, core::Id end);

    // Composite shapes (see core/Builders.h); each records as one Python call.
    core::build::Rectangle rectangle(core::Id corner, core::Id opposite);
    core::build::Rectangle rectangle3(core::Id a, core::Id b, core::Vec2 widthPoint);
    core::build::Rectangle rectangleCenter(core::Id center, core::Vec2 corner);
    core::Id circle2(core::Vec2 a, core::Vec2 b);
    core::Id circle3(core::Vec2 a, core::Vec2 b, core::Vec2 c);
    core::Id circleTangent(const std::vector<core::Id>& curves, core::Vec2 near, double radius);
    core::Id arcTangent(core::Id curve, core::Id start, core::Id end);
    core::build::Polygon polygon(core::Id center, core::Vec2 at, int sides, core::build::PolygonKind kind);
    core::build::Polygon polygonEdge(core::Id a, core::Id b, int sides, bool left);
    core::build::Slot slot(core::Id center1, core::Id center2, double width);
    core::build::Slot slotOverall(core::Vec2 end1, core::Vec2 end2, double width);
    core::build::Slot slotCenterPoint(core::Id center, core::Vec2 capCenter, double width);
    core::build::Slot arcSlot(core::Id center, core::Vec2 start, core::Vec2 end, double width);
    core::build::Slot arcSlot3(core::Vec2 start, core::Vec2 end, core::Vec2 through, double width);

    // Cutting and joining (see core/Modify.h). `near` is where the curve was clicked.
    void trim(core::Id curve, core::Vec2 near, bool toConstruction = false);
    void extend(core::Id curve, core::Vec2 near);
    std::vector<core::Id> breakCurve(core::Id curve, core::Vec2 near);
    core::modify::Corner fillet(core::Id a, core::Vec2 nearA, core::Id b, core::Vec2 nearB, double radius);
    core::modify::Corner chamfer(core::Id a, core::Vec2 nearA, core::Id b, core::Vec2 nearB,
                                 core::modify::ChamferKind kind, double value1, double value2 = 0.0);

    // Copies, transforms and linked features (see core/Transform.h, core/Features.h). Copy and
    // move maps turn by `angle` degrees about `about`, then shift by (dx, dy).
    std::map<core::Id, core::Id> copy(const std::vector<core::Id>& items, double dx, double dy, double angle = 0.0,
                                      core::Vec2 about = {});
    void scale(const std::vector<core::Id>& items, double factor, core::Vec2 about);
    std::map<core::Id, core::Id> mirror(const std::vector<core::Id>& items, core::Id line);
    core::Id rectangularPattern(const std::vector<core::Id>& items, const core::feature::LinearPatternParams& p);
    core::Id circularPattern(const std::vector<core::Id>& items, core::Id center,
                             const core::feature::CircularPatternParams& p);
    // Returns the pattern's id, which changes when the count does (the copies are remade).
    core::Id setPattern(core::Id pattern, const core::feature::LinearPatternParams& p);
    core::Id setPattern(core::Id pattern, const core::feature::CircularPatternParams& p);
    // `toward` picks the side; with bothSides it's ignored (and caps closes an open chain's ends).
    core::Id offset(const std::vector<core::Id>& curves, double distance, core::Vec2 toward, bool bothSides = false,
                    bool caps = false);
    // Points spaced along a line, linked to it. setDivide returns the feature's id, which
    // changes when the number of points does.
    core::Id divide(core::Id line, const core::feature::DivideParams& p);
    core::Id setDivide(core::Id divide, const core::feature::DivideParams& p);
    // Inserts clipboard contents (a sketch fragment, see core::xform::paste) centred at `at`.
    std::map<core::Id, core::Id> paste(const core::Sketch& fragment, core::Vec2 at);

    static core::Affine moveMap(double dx, double dy, double angle, core::Vec2 about);

    // Appearance: give curves a stroke (colour, width, line style), or take it away.
    void setStroke(const std::vector<core::Id>& curves, const core::StrokeStyle& style);
    void clearStroke(const std::vector<core::Id>& curves);
    static QString strokeArgs(const core::StrokeStyle& style);  // "color=\"#ff8800\", width=0.5, dash=\"dashed\""
    // Fills the closed area around `at` (a fill already there is recoloured). Throws
    // SketchError, saying how many loose ends there are, if `at` isn't enclosed.
    core::Id fill(core::Vec2 at, const core::Color& color);

    // Sublayers (named uniquely; scripts refer to them by name). A new one becomes the active one.
    core::Id addSublayer(const QString& name);
    void renameSublayer(core::Id sublayer, const QString& name);
    void removeSublayer(core::Id sublayer);  // and everything in it
    void moveSublayer(core::Id sublayer, int index);
    void moveToSublayer(const std::vector<core::Id>& items, core::Id sublayer);
    // A name not yet taken: `wanted`, or it numbered.
    static QString uniqueSublayerName(const core::Sketch& sketch, const QString& wanted, core::Id except = core::kNoId);
    static QString sublayerName(const core::Sketch& sketch, core::Id sublayer);  // as a Python string
    // Text at t.anchor (an existing point); its letter shapes are made here from its settings.
    core::Id text(core::Text t);
    // Text in a box: a construction rectangle between two corner points, made here.
    core::Id textBox(core::Text t, core::Id corner, core::Id opposite);
    void setText(core::Text t);  // by t.id: new settings (and shape)

    // Unchecked (inference while drawing); Document::addConstraint is the checked version.
    core::Id constrain(core::ConstraintType type, std::vector<core::Id> refs, double value = 0.0);

    void remove(core::Id id);
    void setFixed(core::Id id, bool fixed);
    void setConstruction(core::Id id, bool construction);

    // --- Python spelling, shared with Document's own recording ---
    static QString name(const core::Sketch& sketch, core::Id id);  // p12, l13, s.origin, ...
    static QString number(double v);                                // shortest round-trip form
    static QString function(core::ConstraintType type);            // "horizontal_distance", ...
    // e.g. "d17 = s.distance(l13, value=40, label=(0, 8))"
    static QString constraintCall(const core::Sketch& sketch, core::Id id);
    static QString pair(core::Vec2 v);  // "(x, y)"
    static QString list(const core::Sketch& sketch, const std::vector<core::Id>& ids);  // "[l13, c14]"
    // ", dx=.., dy=.., angle=.., about=(..)" for the parts that aren't zero.
    static QString moveArgs(double dx, double dy, double angle, core::Vec2 about);

private:
    void log(const QString& line)
    {
        if (m_log) {
            m_log->append(line);
        }
    }

    core::Sketch& m_sketch;
    QStringList* m_log;
};
