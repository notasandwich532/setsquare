#include "Editor.h"

#include <format>

using core::ConstraintType;
using core::Id;

QString Editor::number(double v)
{
    if (v == 0.0) {
        return QStringLiteral("0");  // also turns -0 into 0
    }
    return QString::fromStdString(std::format("{}", v));  // shortest string that round-trips
}

QString Editor::name(const core::Sketch& s, Id id)
{
    if (s.isOrigin(id)) {
        return QStringLiteral("s.origin");
    }
    if (id == s.xAxis()) {
        return QStringLiteral("s.x_axis");
    }
    if (id == s.yAxis()) {
        return QStringLiteral("s.y_axis");
    }
    if (s.hasConstraint(id)) {
        return (core::isDimension(s.constraint(id).type) ? QStringLiteral("d") : QStringLiteral("k"))
            + QString::number(id);
    }
    if (s.hasEntity(id)) {
        switch (s.entity(id).kind()) {
            case core::EntityKind::Point: return QStringLiteral("p%1").arg(id);
            case core::EntityKind::Line: return QStringLiteral("l%1").arg(id);
            case core::EntityKind::Circle: return QStringLiteral("c%1").arg(id);
            case core::EntityKind::Arc: return QStringLiteral("a%1").arg(id);
        }
    }
    return QStringLiteral("s[%1]").arg(id);
}

QString Editor::function(ConstraintType type)
{
    switch (type) {
        case ConstraintType::HorizontalDistance: return QStringLiteral("horizontal_distance");
        case ConstraintType::VerticalDistance: return QStringLiteral("vertical_distance");
        default: return QString::fromLatin1(core::toString(type));
    }
}

QString Editor::constraintCall(const core::Sketch& s, Id id)
{
    const core::Constraint& c = s.constraint(id);
    QStringList args;
    for (Id r : c.refs) {
        args << name(s, r);
    }
    if (core::isDimension(c.type)) {
        args << QStringLiteral("value=") + number(c.value);
        if (c.labelX != 0.0 || c.labelY != 0.0) {
            args << QStringLiteral("label=(%1, %2)").arg(number(c.labelX), number(c.labelY));
        }
    }
    return QStringLiteral("%1 = s.%2(%3)").arg(name(s, id), function(c.type), args.join(QStringLiteral(", ")));
}

Id Editor::point(double x, double y)
{
    const Id id = m_sketch.addPoint(x, y);
    log(QStringLiteral("%1 = s.point(%2, %3)").arg(name(m_sketch, id), number(x), number(y)));
    return id;
}

Id Editor::line(Id a, Id b)
{
    const Id id = m_sketch.addLine(a, b);
    log(QStringLiteral("%1 = s.line(%2, %3)").arg(name(m_sketch, id), name(m_sketch, a), name(m_sketch, b)));
    return id;
}

Id Editor::circle(Id center, double radius)
{
    const Id id = m_sketch.addCircle(center, radius);
    log(QStringLiteral("%1 = s.circle(%2, %3)").arg(name(m_sketch, id), name(m_sketch, center), number(radius)));
    return id;
}

Id Editor::arc(Id center, Id start, Id end)
{
    const Id id = m_sketch.addArc(center, start, end);
    log(QStringLiteral("%1 = s.arc(%2, %3, %4)")
            .arg(name(m_sketch, id), name(m_sketch, center), name(m_sketch, start), name(m_sketch, end)));
    return id;
}

QString Editor::pair(core::Vec2 v)
{
    return QStringLiteral("(%1, %2)").arg(number(v.x), number(v.y));
}

// Composite shapes are built on the sketch directly and logged as one call, which replays all
// of it (the builders are deterministic, so ids come out the same).

namespace build = core::build;

build::Rectangle Editor::rectangle(Id corner, Id opposite)
{
    const build::Rectangle r = build::rectangle(m_sketch, corner, opposite);
    log(QStringLiteral("s.rectangle(%1, %2)").arg(name(m_sketch, corner), name(m_sketch, opposite)));
    return r;
}

build::Rectangle Editor::rectangle3(Id a, Id b, core::Vec2 widthPoint)
{
    const build::Rectangle r = build::rectangle3(m_sketch, a, b, widthPoint);
    log(QStringLiteral("s.rectangle_3point(%1, %2, %3)").arg(name(m_sketch, a), name(m_sketch, b), pair(widthPoint)));
    return r;
}

build::Rectangle Editor::rectangleCenter(Id center, core::Vec2 corner)
{
    const build::Rectangle r = build::rectangleCenter(m_sketch, center, corner);
    log(QStringLiteral("s.rectangle_center(%1, %2)").arg(name(m_sketch, center), pair(corner)));
    return r;
}

Id Editor::circle2(core::Vec2 a, core::Vec2 b)
{
    const Id id = build::circle2(m_sketch, a, b);
    log(QStringLiteral("%1 = s.circle_2point(%2, %3)").arg(name(m_sketch, id), pair(a), pair(b)));
    return id;
}

Id Editor::circle3(core::Vec2 a, core::Vec2 b, core::Vec2 c)
{
    const Id id = build::circle3(m_sketch, a, b, c);
    log(QStringLiteral("%1 = s.circle_3point(%2, %3, %4)").arg(name(m_sketch, id), pair(a), pair(b), pair(c)));
    return id;
}

Id Editor::circleTangent(const std::vector<Id>& curves, core::Vec2 near, double radius)
{
    const Id id = build::circleTangent(m_sketch, curves, near, radius);
    QStringList names;
    for (Id c : curves) {
        names << name(m_sketch, c);
    }
    QString call = QStringLiteral("%1 = s.circle_tangent([%2], near=%3")
                       .arg(name(m_sketch, id), names.join(QStringLiteral(", ")), pair(near));
    if (curves.size() == 2) {
        call += QStringLiteral(", radius=") + number(radius);
    }
    log(call + QLatin1Char(')'));
    return id;
}

Id Editor::arcTangent(Id curve, Id start, Id end)
{
    const Id id = build::arcTangent(m_sketch, curve, start, end);
    log(QStringLiteral("%1 = s.arc_tangent(%2, %3, %4)")
            .arg(name(m_sketch, id), name(m_sketch, curve), name(m_sketch, start), name(m_sketch, end)));
    return id;
}

build::Polygon Editor::polygon(Id center, core::Vec2 at, int sides, build::PolygonKind kind)
{
    const build::Polygon p = build::polygon(m_sketch, center, at, sides, kind);
    log(QStringLiteral("s.polygon(%1, %2, sides=%3%4)")
            .arg(name(m_sketch, center), pair(at), QString::number(sides),
                 kind == build::PolygonKind::Circumscribed ? QStringLiteral(", circumscribed=True") : QString()));
    return p;
}

build::Polygon Editor::polygonEdge(Id a, Id b, int sides, bool left)
{
    const build::Polygon p = build::polygonEdge(m_sketch, a, b, sides, left);
    log(QStringLiteral("s.polygon_edge(%1, %2, sides=%3%4)")
            .arg(name(m_sketch, a), name(m_sketch, b), QString::number(sides),
                 left ? QString() : QStringLiteral(", left=False")));
    return p;
}

build::Slot Editor::slot(Id center1, Id center2, double width)
{
    const build::Slot s = build::slot(m_sketch, center1, center2, width);
    log(QStringLiteral("s.slot(%1, %2, %3)").arg(name(m_sketch, center1), name(m_sketch, center2), number(width)));
    return s;
}

build::Slot Editor::slotOverall(core::Vec2 end1, core::Vec2 end2, double width)
{
    const build::Slot s = build::slotOverall(m_sketch, end1, end2, width);
    log(QStringLiteral("s.slot_overall(%1, %2, %3)").arg(pair(end1), pair(end2), number(width)));
    return s;
}

build::Slot Editor::slotCenterPoint(Id center, core::Vec2 capCenter, double width)
{
    const build::Slot s = build::slotCenterPoint(m_sketch, center, capCenter, width);
    log(QStringLiteral("s.slot_center_point(%1, %2, %3)").arg(name(m_sketch, center), pair(capCenter), number(width)));
    return s;
}

build::Slot Editor::arcSlot(Id center, core::Vec2 start, core::Vec2 end, double width)
{
    const build::Slot s = build::arcSlot(m_sketch, center, start, end, width);
    log(QStringLiteral("s.arc_slot(%1, %2, %3, %4)").arg(name(m_sketch, center), pair(start), pair(end), number(width)));
    return s;
}

build::Slot Editor::arcSlot3(core::Vec2 start, core::Vec2 end, core::Vec2 through, double width)
{
    const build::Slot s = build::arcSlot3(m_sketch, start, end, through, width);
    log(QStringLiteral("s.arc_slot_3point(%1, %2, %3, %4)").arg(pair(start), pair(end), pair(through), number(width)));
    return s;
}

Id Editor::constrain(ConstraintType type, std::vector<Id> refs, double value)
{
    const Id id = m_sketch.addConstraint(type, std::move(refs), value);
    log(constraintCall(m_sketch, id));
    return id;
}

void Editor::remove(Id id)
{
    const QString what = name(m_sketch, id);  // before it's gone
    m_sketch.remove(id);
    log(QStringLiteral("s.delete(%1)").arg(what));
}

void Editor::setFixed(Id id, bool fixed)
{
    m_sketch.entity(id).fixed = fixed;
    log(QStringLiteral("%1.fixed = %2").arg(name(m_sketch, id), fixed ? "True" : "False"));
}

void Editor::setConstruction(Id id, bool construction)
{
    m_sketch.entity(id).construction = construction;
    log(QStringLiteral("%1.construction = %2").arg(name(m_sketch, id), construction ? "True" : "False"));
}
