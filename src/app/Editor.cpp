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

Editor::Rectangle Editor::rectangle(Id corner, Id opposite)
{
    // Done on the sketch directly and logged as one call: s.rectangle() replays all of it.
    const core::Point a = m_sketch.point(corner);
    const core::Point c = m_sketch.point(opposite);
    Rectangle r;
    r.corners = {corner, m_sketch.addPoint(c.x, a.y), opposite, m_sketch.addPoint(a.x, c.y)};
    for (int i = 0; i < 4; ++i) {
        r.sides[i] = m_sketch.addLine(r.corners[i], r.corners[(i + 1) % 4]);
    }
    m_sketch.addConstraint(ConstraintType::Horizontal, {r.sides[0]});
    m_sketch.addConstraint(ConstraintType::Vertical, {r.sides[1]});
    m_sketch.addConstraint(ConstraintType::Horizontal, {r.sides[2]});
    m_sketch.addConstraint(ConstraintType::Vertical, {r.sides[3]});
    log(QStringLiteral("s.rectangle(%1, %2)").arg(name(m_sketch, corner), name(m_sketch, opposite)));
    return r;
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
