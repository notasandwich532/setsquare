#include "Editor.h"

#include "TextShapes.h"

#include <core/Gradient.h>
#include <core/Hatch.h>
#include <core/Regions.h>
#include <core/Serialize.h>

#include <QDir>
#include <QFileInfo>

#include <format>
#include <set>

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
    for (int i = 0; i < 4; ++i) {
        if (id == s.pageEdges()[i]) {
            return QStringLiteral("s.page_edges[%1]").arg(i);
        }
        if (id == s.pageCorners()[i]) {
            return QStringLiteral("s.page_corners[%1]").arg(i);
        }
    }
    if (s.hasConstraint(id)) {
        return (core::isDimension(s.constraint(id).type) ? QStringLiteral("d") : QStringLiteral("k"))
            + QString::number(id);
    }
    if (s.hasFill(id)) {
        return QStringLiteral("f%1").arg(id);
    }
    if (s.hasText(id)) {
        return QStringLiteral("t%1").arg(id);
    }
    if (s.hasImage(id)) {
        return QStringLiteral("i%1").arg(id);
    }
    if (s.hasEntity(id)) {
        switch (s.entity(id).kind()) {
            case core::EntityKind::Point: return QStringLiteral("p%1").arg(id);
            case core::EntityKind::Line: return QStringLiteral("l%1").arg(id);
            case core::EntityKind::Circle: return QStringLiteral("c%1").arg(id);
            case core::EntityKind::Arc: return QStringLiteral("a%1").arg(id);
            case core::EntityKind::Ellipse: return QStringLiteral("e%1").arg(id);
            case core::EntityKind::EllipseArc: return QStringLiteral("ea%1").arg(id);
            case core::EntityKind::Spline: return QStringLiteral("sp%1").arg(id);
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
    if (c.type == core::ConstraintType::Group) {
        return QStringLiteral("%1 = s.group([%2])").arg(name(s, id), args.join(QStringLiteral(", ")));
    }
    if (c.type == core::ConstraintType::Ratio) {
        args << QStringLiteral("value=") + number(c.value);
    }
    if (core::isDimension(c.type)) {
        args << QStringLiteral("value=") + number(c.value);
        if (c.labelX != 0.0 || c.labelY != 0.0) {
            args << QStringLiteral("label=(%1, %2)").arg(number(c.labelX), number(c.labelY));
        }
        if (c.flip != 0) {
            args << QStringLiteral("reverse=(%1, %2)")
                        .arg((c.flip & 1) ? QStringLiteral("True") : QStringLiteral("False"),
                             (c.flip & 2) ? QStringLiteral("True") : QStringLiteral("False"));
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

Id Editor::ellipse(Id center, Id major, Id minor)
{
    const Id id = m_sketch.addEllipse(center, major, minor);
    log(QStringLiteral("%1 = s.ellipse(%2, %3, %4)")
            .arg(name(m_sketch, id), name(m_sketch, center), name(m_sketch, major), name(m_sketch, minor)));
    return id;
}

Id Editor::ellipseArc(Id center, Id major, Id minor, Id start, Id end)
{
    const Id id = m_sketch.addEllipseArc(center, major, minor, start, end);
    log(QStringLiteral("%1 = s.ellipse_arc(%2, %3, %4, %5, %6)")
            .arg(name(m_sketch, id), name(m_sketch, center), name(m_sketch, major), name(m_sketch, minor),
                 name(m_sketch, start), name(m_sketch, end)));
    return id;
}

Id Editor::spline(const std::vector<Id>& points, core::SplineKind kind, bool closed)
{
    core::Spline sp;
    sp.points = points;
    sp.kind = kind;
    sp.closed = closed;
    const Id id = m_sketch.addSpline(sp);
    QString args = list(m_sketch, points);
    if (kind == core::SplineKind::Control) {
        args += QStringLiteral(", kind=\"control\"");
    }
    if (closed) {
        args += QStringLiteral(", closed=True");
    }
    log(QStringLiteral("%1 = s.spline(%2)").arg(name(m_sketch, id), args));
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

namespace modify = core::modify;

void Editor::trim(Id curve, core::Vec2 near, bool toConstruction)
{
    const QString what = name(m_sketch, curve);  // before it changes (it may disappear)
    if (toConstruction) {
        modify::trimToConstruction(m_sketch, curve, near);
        modify::dropTrivialCoincidences(m_sketch);
        log(QStringLiteral("s.trim(%1, near=%2, construction=True)").arg(what, pair(near)));
    }
    else {
        modify::trim(m_sketch, curve, near);
        modify::dropTrivialCoincidences(m_sketch);
        log(QStringLiteral("s.trim(%1, near=%2)").arg(what, pair(near)));
    }
}

void Editor::extend(Id curve, core::Vec2 near)
{
    const QString what = name(m_sketch, curve);
    modify::extend(m_sketch, curve, near);
    modify::dropTrivialCoincidences(m_sketch);
    log(QStringLiteral("s.extend(%1, near=%2)").arg(what, pair(near)));
}

std::vector<Id> Editor::breakCurve(Id curve, core::Vec2 near)
{
    const QString what = name(m_sketch, curve);
    std::vector<Id> pieces = modify::breakCurve(m_sketch, curve, near);
    modify::dropTrivialCoincidences(m_sketch);
    log(QStringLiteral("s.break_curve(%1, near=%2)").arg(what, pair(near)));
    return pieces;
}

modify::Corner Editor::fillet(Id a, core::Vec2 nearA, Id b, core::Vec2 nearB, double radius)
{
    const QString na = name(m_sketch, a), nb = name(m_sketch, b);
    modify::Corner c = modify::fillet(m_sketch, a, nearA, b, nearB, radius);
    log(QStringLiteral("%1 = s.fillet(%2, %3, %4, near=(%5, %6))")
            .arg(name(m_sketch, c.joint), na, nb, number(radius), pair(nearA), pair(nearB)));
    return c;
}

modify::Corner Editor::chamfer(Id a, core::Vec2 nearA, Id b, core::Vec2 nearB, modify::ChamferKind kind,
                               double value1, double value2)
{
    const QString na = name(m_sketch, a), nb = name(m_sketch, b);
    modify::Corner c = modify::chamfer(m_sketch, a, nearA, b, nearB, kind, value1, value2);
    QString extra;
    if (kind == modify::ChamferKind::TwoDistances) {
        extra = QStringLiteral(", distance2=") + number(value2);
    }
    else if (kind == modify::ChamferKind::DistanceAngle) {
        extra = QStringLiteral(", angle=") + number(value2);
    }
    log(QStringLiteral("%1 = s.chamfer(%2, %3, %4%5, near=(%6, %7))")
            .arg(name(m_sketch, c.joint), na, nb, number(value1), extra, pair(nearA), pair(nearB)));
    return c;
}

QString Editor::list(const core::Sketch& s, const std::vector<Id>& ids)
{
    QStringList names;
    for (Id id : ids) {
        names << name(s, id);
    }
    return QLatin1Char('[') + names.join(QStringLiteral(", ")) + QLatin1Char(']');
}

QString Editor::moveArgs(double dx, double dy, double angle, core::Vec2 about)
{
    QString args;
    if (dx != 0.0) {
        args += QStringLiteral(", dx=") + number(dx);
    }
    if (dy != 0.0) {
        args += QStringLiteral(", dy=") + number(dy);
    }
    if (angle != 0.0) {
        args += QStringLiteral(", angle=") + number(angle);
        if (about.x != 0.0 || about.y != 0.0) {
            args += QStringLiteral(", about=") + pair(about);
        }
    }
    return args;
}

core::Affine Editor::moveMap(double dx, double dy, double angle, core::Vec2 about)
{
    return core::Affine::rotation(about, angle).then(core::Affine::translation(dx, dy));
}

namespace xform = core::xform;
namespace feature = core::feature;

std::map<Id, Id> Editor::copy(const std::vector<Id>& items, double dx, double dy, double angle, core::Vec2 about)
{
    const QString what = list(m_sketch, items);
    std::map<Id, Id> map = xform::copy(m_sketch, items, moveMap(dx, dy, angle, about));
    log(QStringLiteral("s.copy(%1%2)").arg(what, moveArgs(dx, dy, angle, about)));
    return map;
}

void Editor::scale(const std::vector<Id>& items, double factor, core::Vec2 about)
{
    xform::apply(m_sketch, items, core::Affine::scaling(about, factor));
    QString call = QStringLiteral("s.scale(%1, %2").arg(list(m_sketch, items), number(factor));
    if (about.x != 0.0 || about.y != 0.0) {
        call += QStringLiteral(", about=") + pair(about);
    }
    log(call + QLatin1Char(')'));
}

std::map<Id, Id> Editor::mirror(const std::vector<Id>& items, Id line)
{
    const QString what = list(m_sketch, items);
    std::map<Id, Id> map = xform::mirror(m_sketch, items, line);
    log(QStringLiteral("s.mirror(%1, %2)").arg(what, name(m_sketch, line)));
    return map;
}

namespace
{
QString linearArgs(const feature::LinearPatternParams& p)
{
    QString args = QStringLiteral("count=%1, spacing=%2").arg(p.count).arg(Editor::number(p.spacing));
    if (p.angle != 0.0) {
        args += QStringLiteral(", angle=") + Editor::number(p.angle);
    }
    if (p.symmetric) {
        args += QStringLiteral(", symmetric=True");
    }
    if (p.count2 != 1) {
        args += QStringLiteral(", count2=%1, spacing2=%2").arg(p.count2).arg(Editor::number(p.spacing2));
    }
    if (p.symmetric2) {
        args += QStringLiteral(", symmetric2=True");
    }
    return args;
}

QString circularArgs(const feature::CircularPatternParams& p)
{
    QString args = QStringLiteral("count=%1").arg(p.count);
    if (p.angle != 360.0) {
        args += QStringLiteral(", angle=") + Editor::number(p.angle);
    }
    return args;
}
}  // namespace

Id Editor::rectangularPattern(const std::vector<Id>& items, const feature::LinearPatternParams& p)
{
    const QString what = list(m_sketch, items);
    const Id id = feature::linearPattern(m_sketch, items, p);
    log(QStringLiteral("%1 = s.rectangular_pattern(%2, %3)").arg(name(m_sketch, id), what, linearArgs(p)));
    return id;
}

Id Editor::circularPattern(const std::vector<Id>& items, Id center, const feature::CircularPatternParams& p)
{
    const QString what = list(m_sketch, items);
    const Id id = feature::circularPattern(m_sketch, items, center, p);
    log(QStringLiteral("%1 = s.circular_pattern(%2, %3, %4)")
            .arg(name(m_sketch, id), what, name(m_sketch, center), circularArgs(p)));
    return id;
}

Id Editor::setPattern(Id pattern, const feature::LinearPatternParams& p)
{
    const QString what = name(m_sketch, pattern);
    const Id id = feature::setLinearPattern(m_sketch, pattern, p);
    log(QStringLiteral("%1 = s.set_pattern(%2, %3)").arg(name(m_sketch, id), what, linearArgs(p)));
    return id;
}

Id Editor::setPattern(Id pattern, const feature::CircularPatternParams& p)
{
    const QString what = name(m_sketch, pattern);
    const Id id = feature::setCircularPattern(m_sketch, pattern, p);
    log(QStringLiteral("%1 = s.set_pattern(%2, %3)").arg(name(m_sketch, id), what, circularArgs(p)));
    return id;
}

namespace
{
QString divideArgs(const feature::DivideParams& p)
{
    QString args = p.byRatio ? QStringLiteral("ratio=\"%1\"").arg(QString::fromStdString(feature::ratioText(p.parts)))
                             : QStringLiteral("count=%1").arg(p.parts.size() - 1);
    if (p.reversed) {
        args += QStringLiteral(", reversed=True");
    }
    return args;
}
}  // namespace

Id Editor::divide(Id line, const feature::DivideParams& p)
{
    const Id id = feature::divide(m_sketch, line, p);
    log(QStringLiteral("%1 = s.divide(%2, %3)").arg(name(m_sketch, id), name(m_sketch, line), divideArgs(p)));
    return id;
}

Id Editor::setDivide(Id divide, const feature::DivideParams& p)
{
    const QString what = name(m_sketch, divide);
    const Id id = feature::setDivide(m_sketch, divide, p);
    log(QStringLiteral("%1 = s.set_divide(%2, %3)").arg(name(m_sketch, id), what, divideArgs(p)));
    return id;
}

Id Editor::offset(const std::vector<Id>& curves, double distance, core::Vec2 toward, bool bothSides, bool caps)
{
    const QString what = list(m_sketch, curves);
    const Id id = feature::offset(m_sketch, curves, distance, toward, bothSides, caps);
    if (bothSides) {
        log(QStringLiteral("%1 = s.offset(%2, %3, both_sides=True%4)")
                .arg(name(m_sketch, id), what, number(distance), caps ? QStringLiteral(", caps=True") : QString()));
    }
    else {
        log(QStringLiteral("%1 = s.offset(%2, %3, toward=%4)").arg(name(m_sketch, id), what, number(distance), pair(toward)));
    }
    return id;
}

std::map<Id, Id> Editor::paste(const core::Sketch& fragment, core::Vec2 at)
{
    std::map<Id, Id> map = xform::paste(m_sketch, fragment, at);
    // The fragment travels as JSON (single-quoted: it has no single quotes or backslashes).
    log(QStringLiteral("s.paste('%1', at=%2)").arg(QString::fromStdString(core::toJson(fragment, true)), pair(at)));
    return map;
}

QString Editor::strokeArgs(const core::Sketch& sketch, const core::StrokeStyle& style)
{
    const QString color = style.gradient.active() ? gradientCall(sketch, style.gradient)
                                                  : QStringLiteral("\"%1\"").arg(QString::fromStdString(style.color.hex()));
    return QStringLiteral("color=%1, width=%2, dash=\"%3\"")
        .arg(color, style.none() ? QStringLiteral("\"none\"") : number(style.width),
             QString::fromLatin1(core::toString(style.dash)));
}

QString Editor::gradientCall(const core::Sketch& sketch, const core::Gradient& g)
{
    QStringList stops;
    for (const core::GradientStop& s : g.stops) {
        stops << QStringLiteral("(%1, \"%2\")").arg(number(s.offset), QString::fromStdString(s.color.hex()));
    }
    QStringList args;
    args << QLatin1Char('[') + stops.join(QStringLiteral(", ")) + QLatin1Char(']');
    const bool radial = g.kind == core::GradientKind::Radial;
    if (radial) {
        args << QStringLiteral("shape=") + name(sketch, g.shape);
        if (g.focal != Id(core::kNoId)) {
            args << QStringLiteral("focal=") + name(sketch, g.focal);
        }
    }
    else {
        args << QStringLiteral("start=") + name(sketch, g.start) << QStringLiteral("end=") + name(sketch, g.end);
    }
    if (g.spread != core::GradientSpread::Pad) {
        args << QStringLiteral("spread=\"%1\"").arg(QString::fromLatin1(core::toString(g.spread)));
    }
    return QStringLiteral("drawapp.%1(%2)").arg(radial ? QStringLiteral("radial") : QStringLiteral("linear"),
                                                args.join(QStringLiteral(", ")));
}

void Editor::settleGradient(core::Gradient& g, core::Color& color, Id layer) const
{
    if (!g.active()) {
        return;
    }
    // Its geometry deleted: the item goes back to a plain colour (the gradient's average).
    for (Id id : g.refs()) {
        if (!m_sketch.hasEntity(id)) {
            color = g.average();
            g = {};
            return;
        }
    }
    core::gradient::check(m_sketch, g, layer);
}

void Editor::setStroke(const std::vector<Id>& curves, const core::StrokeStyle& given)
{
    core::StrokeStyle style = given;
    settleGradient(style.gradient, style.color, curves.empty() ? Id(core::kNoId) : m_sketch.layerOf(curves.front()));
    for (Id c : curves) {
        if (style.gradient.active()) {
            core::gradient::check(m_sketch, style.gradient, m_sketch.layerOf(c));
        }
        m_sketch.setStroke(c, style);
    }
    log(QStringLiteral("s.stroke(%1, %2)").arg(list(m_sketch, curves), strokeArgs(m_sketch, style)));
}
void Editor::clearStroke(const std::vector<Id>& curves)
{
    for (Id c : curves) {
        m_sketch.clearStroke(c);
    }
    log(QStringLiteral("s.clear_stroke(%1)").arg(list(m_sketch, curves)));
}

namespace
{
// A Python string literal.
QString pyString(const std::string& text)
{
    QString out = QStringLiteral("\"");
    for (const QChar ch : QString::fromStdString(text)) {
        if (ch == QLatin1Char('\\') || ch == QLatin1Char('"')) {
            out += QLatin1Char('\\');
        }
        if (ch == QLatin1Char('\n')) {
            out += QStringLiteral("\\n");
            continue;
        }
        out += ch;
    }
    return out + QLatin1Char('"');
}

// The settings of a text as Python keyword arguments (leaving out the defaults).
QString textArgs(const core::Sketch& sketch, const core::Text& t)
{
    QStringList args;
    args << QStringLiteral("height=") + Editor::number(t.height);
    args << QStringLiteral("font=") + pyString(t.font);
    if (t.angle != 0.0 && !t.inBox() && t.path == core::kNoId) {  // (a box or path sets its own)
        args << QStringLiteral("angle=") + Editor::number(t.angle);
    }
    if (t.bold) {
        args << QStringLiteral("bold=True");
    }
    if (t.italic) {
        args << QStringLiteral("italic=True");
    }
    if (t.outline != core::TextOutline::AsDesigned) {
        args << QStringLiteral("outline=\"%1\"").arg(QString::fromLatin1(core::toString(t.outline)));
    }
    if (t.minThickness > 0.0) {
        args << QStringLiteral("min_thickness=") + Editor::number(t.minThickness);
    }
    if (t.hasColor) {
        args << (t.gradient.active() ? QStringLiteral("color=") + Editor::gradientCall(sketch, t.gradient)
                                     : QStringLiteral("color=\"%1\"").arg(QString::fromStdString(t.color.hex())));
    }
    if (t.spacing != 0.0) {
        args << QStringLiteral("spacing=") + Editor::number(t.spacing);
    }
    if (t.align != core::TextAlign::Left) {
        args << QStringLiteral("align=\"%1\"").arg(QString::fromLatin1(core::toString(t.align)));
    }
    if (t.valign != core::TextVAlign::Baseline) {
        args << QStringLiteral("valign=\"%1\"").arg(QString::fromLatin1(core::toString(t.valign)));
    }
    if (t.flipH) {
        args << QStringLiteral("flip_h=True");
    }
    if (t.flipV) {
        args << QStringLiteral("flip_v=True");
    }
    if (t.inBox()) {  // (said either way: edit_text keeps what isn't given)
        args << (t.wrap ? QStringLiteral("wrap=True") : QStringLiteral("wrap=False"));
        if (t.wrap) {
            args << (t.grow ? QStringLiteral("grow=True") : QStringLiteral("grow=False"));
        }
    }
    return args.join(QStringLiteral(", "));
}
}  // namespace

Id Editor::text(core::Text t)
{
    settleGradient(t.gradient, t.color, m_sketch.activeLayer());
    textshapes::update(t, &m_sketch);
    const Id id = m_sketch.addText(t);
    QString where = t.path != core::kNoId ? QStringLiteral("path=") + name(m_sketch, t.path) : name(m_sketch, t.anchor);
    if (!t.pathMore.empty()) {  // (a chain: its curves in order)
        std::vector<Id> chain{t.path};
        chain.insert(chain.end(), t.pathMore.begin(), t.pathMore.end());
        where = QStringLiteral("path=") + list(m_sketch, chain);
    }
    log(QStringLiteral("%1 = s.text(%2, %3, %4)").arg(name(m_sketch, id), pyString(t.text), where, textArgs(m_sketch, t)));
    return id;
}

Id Editor::textBox(core::Text t, Id corner, Id opposite)
{
    // The box: a construction rectangle between the two corners (level and upright, so it can be
    // dimensioned; take those constraints off to turn it, and the text turns with it).
    const QString a = name(m_sketch, corner), b = name(m_sketch, opposite);
    const build::Rectangle r = build::rectangle(m_sketch, corner, opposite);
    for (Id side : r.sides) {
        m_sketch.entity(side).construction = true;
    }
    // Its corners by where they are: the text runs along the bottom, from the left.
    auto at = [&](Id p) { return m_sketch.point(p); };
    double x0 = 1e300, y0 = 1e300;
    for (Id p : r.corners) {
        x0 = std::min(x0, at(p).x);
        y0 = std::min(y0, at(p).y);
    }
    for (Id p : r.corners) {
        const bool left = std::abs(at(p).x - x0) < 1e-9, bottom = std::abs(at(p).y - y0) < 1e-9;
        if (left && bottom) {
            t.anchor = p;
        }
        else if (!left && bottom) {
            t.boxSide = p;
        }
        else if (!left && !bottom) {
            t.boxCorner = p;
        }
    }
    t.path = core::kNoId;
    settleGradient(t.gradient, t.color, m_sketch.activeLayer());
    textshapes::update(t, &m_sketch);
    const Id id = m_sketch.addText(t);
    fitBox(id);
    log(QStringLiteral("%1 = s.text(%2, box=(%3, %4), %5)").arg(name(m_sketch, id), pyString(t.text), a, b, textArgs(m_sketch, t)));
    return id;
}

void Editor::fitBox(Id text)
{
    // A box that grows with its text: made tall enough now, or the edit's refused.
    if (!m_sketch.hasText(text) || !m_sketch.text(text).wrap || !m_sketch.text(text).grow) {
        return;
    }
    std::string why;
    if (!textshapes::fitBox(m_sketch, text, &why)) {
        throw core::SketchError(why);
    }
    textshapes::refreshPathTexts(m_sketch);  // (laid out in the box as it is now)
}

void Editor::setText(core::Text t)
{
    settleGradient(t.gradient, t.color, m_sketch.layerOf(t.id));
    textshapes::update(t, &m_sketch);
    m_sketch.setText(t);
    fitBox(t.id);
    log(QStringLiteral("s.edit_text(%1, text=%2, %3)").arg(name(m_sketch, t.id), pyString(t.text), textArgs(m_sketch, t)));
}

QString Editor::uniqueSublayerName(const core::Sketch& s, const QString& wanted, Id except)
{
    // (unique within its layer: the active one, for a new sublayer)
    const Id layer = except != core::kNoId ? s.layerOfSublayer(except) : s.activeLayer();
    auto taken = [&](const QString& name) {
        return std::ranges::any_of(s.sublayers(), [&](const core::Sublayer& l) {
            return l.id != except && l.layer == layer && QString::fromStdString(l.name) == name;
        });
    };
    const QString base = wanted.trimmed().isEmpty() ? QStringLiteral("Sublayer") : wanted.trimmed();
    if (!taken(base)) {
        return base;
    }
    for (int n = 2;; ++n) {
        const QString name = QStringLiteral("%1 %2").arg(base).arg(n);
        if (!taken(name)) {
            return name;
        }
    }
}

QString Editor::sublayerName(const core::Sketch& s, Id sublayer)
{
    if (!s.hasSublayer(sublayer)) {
        return pyString(std::string());
    }
    // Its name, or "Layer/Sublayer" where another layer has one called that too.
    const core::Sublayer& l = s.sublayer(sublayer);
    const bool shared = std::ranges::any_of(s.sublayers(), [&](const core::Sublayer& o) {
        return o.id != sublayer && o.name == l.name;
    });
    return pyString(shared ? s.layer(l.layer).name + "/" + l.name : l.name);
}

Id Editor::addSublayer(const QString& name)
{
    const QString unique = uniqueSublayerName(m_sketch, name);
    const Id id = m_sketch.addSublayer(unique.toStdString());
    m_sketch.setActiveSublayer(id);
    log(QStringLiteral("s.add_sublayer(%1)").arg(pyString(unique.toStdString())));
    return id;
}

void Editor::renameSublayer(Id sublayer, const QString& name)
{
    const QString was = sublayerName(m_sketch, sublayer);
    const QString unique = uniqueSublayerName(m_sketch, name, sublayer);
    m_sketch.renameSublayer(sublayer, unique.toStdString());
    log(QStringLiteral("s.rename_sublayer(%1, %2)").arg(was, pyString(unique.toStdString())));
}

void Editor::removeSublayer(Id sublayer)
{
    const QString was = sublayerName(m_sketch, sublayer);
    m_sketch.removeSublayer(sublayer);
    log(QStringLiteral("s.delete_sublayer(%1)").arg(was));
}

void Editor::moveSublayer(Id sublayer, int index)
{
    m_sketch.moveSublayer(sublayer, static_cast<size_t>(std::max(0, index)));
    log(QStringLiteral("s.move_sublayer(%1, %2)").arg(sublayerName(m_sketch, sublayer)).arg(index));
}

void Editor::moveToSublayer(const std::vector<Id>& items, Id sublayer)
{
    // A curve takes its points along (unless they're another sublayer's curves' too); a text its
    // points (and path).
    std::set<Id> moving(items.begin(), items.end());
    for (Id id : items) {
        if (m_sketch.hasEntity(id)) {
            for (Id p : m_sketch.definingPoints(id)) {
                moving.insert(p);
            }
        }
        if (m_sketch.hasText(id)) {
            const core::Text& t = m_sketch.text(id);
            for (Id p : core::textPoints(m_sketch, t)) {
                moving.insert(p);
            }
            std::vector<Id> chain;
            if (t.path != core::kNoId) {
                chain.push_back(t.path);
                chain.insert(chain.end(), t.pathMore.begin(), t.pathMore.end());
            }
            for (Id c : chain) {
                moving.insert(c);
                for (Id p : m_sketch.definingPoints(c)) {
                    moving.insert(p);
                }
            }
        }
    }
    std::erase_if(moving, [&](Id id) { return m_sketch.isReference(id); });
    const Id toLayer = m_sketch.layerOfSublayer(sublayer);
    const bool crossing = std::ranges::any_of(moving, [&](Id id) {
        const Id l = m_sketch.layerOf(id);
        return l != core::kNoId && l != toLayer;
    });
    if (crossing) {
        // Into another layer: nothing may stay behind that's tied to what goes (a point shared
        // with a curve that stays, a text on moved points), since layers can't refer to each
        // other. Constraints tying what goes to what stays are dropped.
        for (const auto& [id, e] : m_sketch.entities()) {
            if (moving.contains(id) || m_sketch.isReference(id)) {
                continue;
            }
            for (Id p : m_sketch.definingPoints(id)) {
                if (p != id && moving.contains(p)) {
                    throw core::SketchError("a point is shared with " + name(m_sketch, id).toStdString()
                                            + ", which isn't moving: move it too");
                }
            }
        }
        for (const auto& [id, t] : m_sketch.texts()) {
            if (moving.contains(id)) {
                continue;
            }
            std::vector<Id> on = core::textPoints(m_sketch, t);
            on.push_back(t.path);
            on.insert(on.end(), t.pathMore.begin(), t.pathMore.end());
            if (std::ranges::any_of(on, [&](Id p) { return moving.contains(p); })) {
                throw core::SketchError("a text sits on that geometry: move it too");
            }
        }
        std::vector<Id> ties;
        for (const auto& [cid, c] : m_sketch.constraints()) {
            const bool some = std::ranges::any_of(c.refs, [&](Id r) { return moving.contains(r); });
            const bool all = std::ranges::all_of(c.refs, [&](Id r) { return moving.contains(r) || m_sketch.isReference(r); });
            if (some && !all) {
                ties.push_back(cid);
            }
        }
        for (Id cid : ties) {
            m_sketch.remove(cid);
        }
    }
    for (Id id : moving) {
        if (m_sketch.hasEntity(id) || m_sketch.hasConstraint(id) || m_sketch.hasText(id)) {
            m_sketch.setSublayerOf(id, sublayer);
        }
    }
    // Constraints wholly among what moved go too.
    for (const auto& [cid, c] : m_sketch.constraints()) {
        if (std::ranges::all_of(c.refs, [&](Id r) { return moving.contains(r) || m_sketch.isReference(r); })
            && std::ranges::any_of(c.refs, [&](Id r) { return moving.contains(r); })) {
            m_sketch.setSublayerOf(cid, sublayer);
        }
    }
    log(QStringLiteral("s.move_to_sublayer(%1, %2)").arg(list(m_sketch, items), sublayerName(m_sketch, sublayer)));
}

QString Editor::uniqueLayerName(const core::Sketch& s, const QString& wanted, Id except)
{
    auto taken = [&](const QString& name) {
        return std::ranges::any_of(s.layers(), [&](const core::Layer& l) {
            return l.id != except && QString::fromStdString(l.name) == name;
        });
    };
    const QString base = wanted.trimmed().isEmpty() ? QStringLiteral("Layer") : wanted.trimmed();
    if (!taken(base)) {
        return base;
    }
    for (int n = 2;; ++n) {
        const QString name = QStringLiteral("%1 %2").arg(base).arg(n);
        if (!taken(name)) {
            return name;
        }
    }
}

QString Editor::layerName(const core::Sketch& s, Id layer)
{
    return pyString(s.hasLayer(layer) ? s.layer(layer).name : std::string());
}

Id Editor::addLayer(const QString& name)
{
    const QString unique = uniqueLayerName(m_sketch, name);
    const Id id = m_sketch.addLayer(unique.toStdString(), "Sublayer 1");
    m_sketch.setActiveLayer(id);
    log(QStringLiteral("s.add_layer(%1)").arg(pyString(unique.toStdString())));
    return id;
}

void Editor::renameLayer(Id layer, const QString& name)
{
    const QString was = layerName(m_sketch, layer);
    const QString unique = uniqueLayerName(m_sketch, name, layer);
    m_sketch.renameLayer(layer, unique.toStdString());
    log(QStringLiteral("s.rename_layer(%1, %2)").arg(was, pyString(unique.toStdString())));
}

void Editor::removeLayer(Id layer)
{
    const QString was = layerName(m_sketch, layer);
    m_sketch.removeLayer(layer);
    log(QStringLiteral("s.delete_layer(%1)").arg(was));
}

void Editor::moveLayer(Id layer, int index)
{
    m_sketch.moveLayer(layer, static_cast<size_t>(std::max(0, index)));
    log(QStringLiteral("s.move_layer(%1, %2)").arg(layerName(m_sketch, layer)).arg(index));
}

std::vector<Id> Editor::project(const std::vector<Id>& sources)
{
    std::vector<Id> copies;
    for (Id id : sources) {
        copies.push_back(m_sketch.project(id));
    }
    log(QStringLiteral("s.project(%1)").arg(list(m_sketch, sources)));
    return copies;
}

namespace
{
// An image's placement as Python keyword arguments.
QString imageArgs(const core::Image& im)
{
    QStringList args;
    args << QStringLiteral("at=") + Editor::pair({im.x, im.y}) << QStringLiteral("width=") + Editor::number(im.width);
    if (im.angle != 0.0) {
        args << QStringLiteral("angle=") + Editor::number(im.angle);
    }
    args << QStringLiteral("opacity=") + Editor::number(im.opacity);
    if (im.flipH) {
        args << QStringLiteral("flip_h=True");
    }
    if (im.flipV) {
        args << QStringLiteral("flip_v=True");
    }
    return args.join(QStringLiteral(", "));
}

QString pyPath(const std::string& path)
{
    QString out = QStringLiteral("r\"");
    out += QString::fromStdString(path);
    return out + QLatin1Char('"');
}
}  // namespace

importer::Report Editor::importFile(const QString& path, importer::Options o, double pxPerInch, std::optional<double> mmPerUnit)
{
    if (o.name.empty()) {
        o.name = QFileInfo(path).completeBaseName().toStdString();
    }
    const importer::Drawing drawing = importer::read(path, pxPerInch, mmPerUnit);
    importer::Report report = importer::build(m_sketch, drawing, o);
    QStringList args;
    args << pyPath(QDir::toNativeSeparators(QFileInfo(path).absoluteFilePath()).toStdString());
    args << QStringLiteral("into=\"%1\"").arg(o.into == importer::Into::Sublayer      ? QStringLiteral("sublayer")
                                              : o.into == importer::Into::NewLayer ? QStringLiteral("new_layer")
                                                                                    : QStringLiteral("new_sublayer"));
    if (!o.keepLayers) {
        args << QStringLiteral("keep_layers=False");
    }
    if (o.placement == importer::Placement::Center) {
        args << QStringLiteral("placement=\"center\", at=") + pair(o.at);
    }
    else if (o.placement == importer::Placement::Page) {
        args << QStringLiteral("placement=\"page\"");
    }
    if (o.curves == importer::Curves::Smooth) {
        args << QStringLiteral("curves=\"smooth\"");
    }
    if (pxPerInch != 96.0) {
        args << QStringLiteral("px_per_inch=") + number(pxPerInch);
    }
    if (mmPerUnit) {
        args << QStringLiteral("units=") + number(*mmPerUnit);
    }
    log(QStringLiteral("s.import_file(%1)").arg(args.join(QStringLiteral(", "))));
    return report;
}

core::Id Editor::addImage(const core::Image& image)
{
    const Id id = m_sketch.addImage(image);
    const core::Image& im = m_sketch.image(id);
    log(QStringLiteral("%1 = s.image(%2, %3)").arg(name(m_sketch, id), pyPath(im.source), imageArgs(im)));
    return id;
}

void Editor::setImage(const core::Image& image)
{
    const core::Image before = m_sketch.image(image.id);
    m_sketch.setImage(image);
    QString call = QStringLiteral("s.edit_image(%1, %2").arg(name(m_sketch, image.id), imageArgs(image));
    if (!image.flipH && before.flipH) {
        call += QStringLiteral(", flip_h=False");
    }
    if (!image.flipV && before.flipV) {
        call += QStringLiteral(", flip_v=False");
    }
    if (image.visible != before.visible) {
        call += image.visible ? QStringLiteral(", visible=True") : QStringLiteral(", visible=False");
    }
    log(call + QLatin1Char(')'));
}

QString Editor::pageCall(const core::Page& p)
{
    QStringList args;
    double w = 0, h = 0;
    if (core::pageSizeByName(p.size, w, h)) {
        args << QStringLiteral("\"%1\"").arg(QString::fromStdString(p.size));
        if (p.landscape()) {
            args << QStringLiteral("orientation=\"landscape\"");
        }
    }
    else {
        args << QStringLiteral("width=") + number(p.width) << QStringLiteral("height=") + number(p.height);
    }
    if (p.origin != core::PageOrigin::BottomLeft) {
        args << QStringLiteral("origin=\"%1\"").arg(QString::fromLatin1(core::toString(p.origin)));
    }
    if (p.hasColor) {
        args << QStringLiteral("color=\"%1\"").arg(QString::fromStdString(p.color.hex()));
    }
    return QStringLiteral("s.set_page(%1)").arg(args.join(QStringLiteral(", ")));
}

void Editor::setPage(const std::optional<core::Page>& page)
{
    m_sketch.setPage(page);
    log(page ? pageCall(*page) : QStringLiteral("s.clear_page()"));
}

void Editor::breakLink(const std::vector<Id>& items)
{
    for (Id id : items) {
        m_sketch.breakLink(id);
    }
    log(QStringLiteral("s.break_link(%1)").arg(list(m_sketch, items)));
}

void Editor::setFillGradient(Id id, const core::Gradient& given)
{
    core::Fill f = m_sketch.fill(id);
    core::Gradient gradient = given;
    settleGradient(gradient, f.color, f.layer);
    f.gradient = gradient;
    if (gradient.active()) {
        f.hatch = {};  // (filled with the gradient: not hatched)
    }
    m_sketch.setFill(f);
    log(QStringLiteral("%1.gradient = %2")
            .arg(name(m_sketch, id), gradient.active() ? gradientCall(m_sketch, gradient) : QStringLiteral("None")));
}

void Editor::setFillHatch(Id id, const core::Hatch& hatch)
{
    if (hatch.active() && !(hatch.scale > 0)) {
        throw core::SketchError("a hatch's scale must be more than 0");
    }
    core::Fill f = m_sketch.fill(id);
    f.hatch = hatch;
    if (hatch.active()) {
        f.gradient = {};  // (hatched: its lines in the plain colour)
    }
    m_sketch.setFill(f);
    log(QStringLiteral("%1.hatch = %2").arg(name(m_sketch, id), hatch.active() ? hatchCall(hatch) : QStringLiteral("None")));
}

QString Editor::hatchCall(const core::Hatch& h)
{
    QStringList args{QStringLiteral("\"%1\"").arg(QString::fromStdString(h.pattern))};
    // A built-in pattern by its name alone; any other with its lines.
    const auto known = core::hatch::builtin(h.pattern);
    if (!known || known->lines != h.lines) {
        QStringList lines;
        for (const core::HatchLine& l : h.lines) {
            QStringList dashes;
            for (double d : l.dashes) {
                dashes << number(d);
            }
            lines << QStringLiteral("(%1, %2, %3, %4, %5, [%6])")
                         .arg(number(l.angle), number(l.x), number(l.y), number(l.dx), number(l.dy), dashes.join(QStringLiteral(", ")));
        }
        args << QStringLiteral("lines=[%1]").arg(lines.join(QStringLiteral(", ")));
    }
    if (h.angle != 0.0) {
        args << QStringLiteral("angle=%1").arg(number(h.angle));
    }
    if (h.scale != 1.0) {
        args << QStringLiteral("scale=%1").arg(number(h.scale));
    }
    if (h.width != 0.0) {
        args << QStringLiteral("width=%1").arg(number(h.width));
    }
    return QStringLiteral("drawapp.hatch(%1)").arg(args.join(QStringLiteral(", ")));
}

Id Editor::fill(core::Vec2 at, const core::Color& givenColor, const core::Gradient& givenGradient, const core::Hatch& hatch)
{
    if (hatch.active() && !(hatch.scale > 0)) {
        throw core::SketchError("a hatch's scale must be more than 0");
    }
    core::Color color = givenColor;
    core::Gradient gradient = hatch.active() ? core::Gradient{} : givenGradient;
    settleGradient(gradient, color, m_sketch.activeLayer());
    const core::regions::Arrangement arrangement(m_sketch, m_sketch.activeLayer());
    const auto region = arrangement.regionAt(at);
    if (!region) {
        const size_t loose = arrangement.looseEnds().size();
        throw core::SketchError(loose == 0 ? "there's no closed area there"
                                           : "there's no closed area there (" + std::to_string(loose)
                                                 + (loose == 1 ? " loose end" : " loose ends") + " in the drawing)");
    }
    // The same area already filled: recolour that fill rather than stacking another.
    Id id = core::kNoId;
    for (const auto& [fid, f] : m_sketch.fills()) {
        if (f.layer != m_sketch.activeLayer()) {
            continue;
        }
        const auto there = core::regions::resolve(arrangement, f);
        // (Outlines, not just curves and area: two equal circles overlapping make two crescents
        // with the same curves and the same area.)
        if (there && there->boundary == region->boundary && std::ranges::equal(there->outer, region->outer, [](core::Vec2 a, core::Vec2 b) { return a.x == b.x && a.y == b.y; })) {
            id = fid;
        }
    }
    if (id != core::kNoId) {
        core::Fill f = m_sketch.fill(id);
        f.color = color;
        f.gradient = gradient;
        f.hatch = hatch;
        m_sketch.setFill(f);
    }
    else {
        core::Fill f;
        f.color = color;
        f.gradient = gradient;
        f.hatch = hatch;
        f.seed = core::Point{at.x, at.y};
        f.boundary = region->boundary;
        id = m_sketch.addFill(f);
    }
    log(QStringLiteral("%1 = s.fill(at=%2, color=%3%4)")
            .arg(name(m_sketch, id), pair(at),
                 gradient.active() ? gradientCall(m_sketch, gradient)
                                   : QStringLiteral("\"%1\"").arg(QString::fromStdString(color.hex())),
                 hatch.active() ? QStringLiteral(", hatch=") + hatchCall(hatch) : QString()));
    return id;
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
    // A fill whose outline is gone altogether (its shape deleted) goes too. (One whose outline is
    // only broken stays, to come back when it's mended; but with nothing of its shape left, it
    // would only colour whatever's drawn there next.)
    std::vector<Id> orphans;
    for (const auto& [fid, f] : m_sketch.fills()) {
        if (!f.boundary.empty() && std::ranges::none_of(f.boundary, [&](Id c) { return m_sketch.hasEntity(c); })) {
            orphans.push_back(fid);
        }
    }
    for (Id fid : orphans) {
        m_sketch.remove(fid);
    }
    log(QStringLiteral("s.delete(%1)").arg(what));
}

void Editor::setFixed(Id id, bool fixed)
{
    if (!fixed && m_sketch.isProjection(id)) {
        throw core::SketchError("projected geometry stays where its original is: break the link to change it");
    }
    m_sketch.entity(id).fixed = fixed;
    log(QStringLiteral("%1.fixed = %2").arg(name(m_sketch, id), fixed ? "True" : "False"));
}

void Editor::setConstruction(Id id, bool construction)
{
    m_sketch.entity(id).construction = construction;
    log(QStringLiteral("%1.construction = %2").arg(name(m_sketch, id), construction ? "True" : "False"));
}
