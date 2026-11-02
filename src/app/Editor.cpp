#include "Editor.h"

#include "TextShapes.h"

#include <core/Regions.h>
#include <core/Serialize.h>

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
    if (s.hasFill(id)) {
        return QStringLiteral("f%1").arg(id);
    }
    if (s.hasText(id)) {
        return QStringLiteral("t%1").arg(id);
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
        log(QStringLiteral("s.trim(%1, near=%2, construction=True)").arg(what, pair(near)));
    }
    else {
        modify::trim(m_sketch, curve, near);
        log(QStringLiteral("s.trim(%1, near=%2)").arg(what, pair(near)));
    }
}

void Editor::extend(Id curve, core::Vec2 near)
{
    const QString what = name(m_sketch, curve);
    modify::extend(m_sketch, curve, near);
    log(QStringLiteral("s.extend(%1, near=%2)").arg(what, pair(near)));
}

std::vector<Id> Editor::breakCurve(Id curve, core::Vec2 near)
{
    const QString what = name(m_sketch, curve);
    std::vector<Id> pieces = modify::breakCurve(m_sketch, curve, near);
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

QString Editor::strokeArgs(const core::StrokeStyle& style)
{
    return QStringLiteral("color=\"%1\", width=%2, dash=\"%3\"")
        .arg(QString::fromStdString(style.color.hex()), number(style.width), QString::fromLatin1(core::toString(style.dash)));
}

void Editor::setStroke(const std::vector<Id>& curves, const core::StrokeStyle& style)
{
    for (Id c : curves) {
        m_sketch.setStroke(c, style);
    }
    log(QStringLiteral("s.stroke(%1, %2)").arg(list(m_sketch, curves), strokeArgs(style)));
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
QString textArgs(const core::Text& t)
{
    QStringList args;
    args << QStringLiteral("height=") + Editor::number(t.height);
    args << QStringLiteral("font=") + pyString(t.font);
    if (t.angle != 0.0) {
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
        args << QStringLiteral("color=\"%1\"").arg(QString::fromStdString(t.color.hex()));
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
    return args.join(QStringLiteral(", "));
}
}  // namespace

Id Editor::text(core::Text t)
{
    textshapes::update(t, &m_sketch);
    const Id id = m_sketch.addText(t);
    const QString where = t.path != core::kNoId ? QStringLiteral("path=") + name(m_sketch, t.path) : name(m_sketch, t.anchor);
    log(QStringLiteral("%1 = s.text(%2, %3, %4)").arg(name(m_sketch, id), pyString(t.text), where, textArgs(t)));
    return id;
}

void Editor::setText(core::Text t)
{
    textshapes::update(t, &m_sketch);
    m_sketch.setText(t);
    log(QStringLiteral("s.edit_text(%1, text=%2, %3)").arg(name(m_sketch, t.id), pyString(t.text), textArgs(t)));
}

Id Editor::fill(core::Vec2 at, const core::Color& color)
{
    const core::regions::Arrangement arrangement(m_sketch);
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
        const auto there = core::regions::resolve(arrangement, f);
        if (there && there->boundary == region->boundary && there->area == region->area) {
            id = fid;
        }
    }
    if (id != core::kNoId) {
        core::Fill f = m_sketch.fill(id);
        f.color = color;
        m_sketch.setFill(f);
    }
    else {
        core::Fill f;
        f.color = color;
        f.seed = core::Point{at.x, at.y};
        f.boundary = region->boundary;
        id = m_sketch.addFill(f);
    }
    log(QStringLiteral("%1 = s.fill(at=%2, color=\"%3\")")
            .arg(name(m_sketch, id), pair(at), QString::fromStdString(color.hex())));
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
