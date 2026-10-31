#pragma once

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

    // Axis-aligned rectangle on two opposite corner points: adds the other two corners, four
    // sides (bottom, right, top, left) and their horizontal/vertical constraints.
    struct Rectangle
    {
        std::array<core::Id, 4> corners;
        std::array<core::Id, 4> sides;
    };
    Rectangle rectangle(core::Id corner, core::Id opposite);

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
