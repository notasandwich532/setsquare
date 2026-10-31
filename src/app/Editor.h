#pragma once

#include <core/Builders.h>
#include <core/Sketch.h>

#include <QString>
#include <QStringList>

#include <array>
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
