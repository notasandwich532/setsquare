#include "ConstraintTools.h"

#include "../Document.h"
#include "../SketchView.h"
#include "../Theme.h"

#include <core/Curves.h>
#include <core/Dimensions.h>
#include <core/Geometry.h>

#include <QPainter>
#include <QPolygonF>

#include <algorithm>
#include <cmath>
#include <set>

using core::ConstraintType;
using core::EntityKind;
using core::Id;
using core::kNoId;
using core::Sketch;
using core::Vec2;

namespace
{
EntityKind kindOf(const Sketch& s, Id id)
{
    return s.entity(id).kind();
}

Vec2 pos(const Sketch& s, Id point)
{
    const core::Point& p = s.point(point);
    return {p.x, p.y};
}

// Horizontal if the pair spans more x than y.
ConstraintType horizontalOrVertical(Vec2 a, Vec2 b)
{
    return std::abs(b.x - a.x) >= std::abs(b.y - a.y) ? ConstraintType::Horizontal : ConstraintType::Vertical;
}

// Fusion-style choice for a length between a and b, from where the label is being placed:
// alongside the segment -> aligned; off its ends but within its x-span -> horizontal;
// within its y-span -> vertical.
ConstraintType linearTypeFor(Vec2 a, Vec2 b, Vec2 mouse)
{
    const double dx = b.x - a.x, dy = b.y - a.y;
    const double l2 = dx * dx + dy * dy;
    const double t = l2 > 0 ? ((mouse.x - a.x) * dx + (mouse.y - a.y) * dy) / l2 : 0.0;
    if (t >= 0.0 && t <= 1.0) {
        return ConstraintType::Distance;
    }
    if (mouse.x >= std::min(a.x, b.x) && mouse.x <= std::max(a.x, b.x)) {
        return ConstraintType::HorizontalDistance;
    }
    if (mouse.y >= std::min(a.y, b.y) && mouse.y <= std::max(a.y, b.y)) {
        return ConstraintType::VerticalDistance;
    }
    return ConstraintType::Distance;
}

ConstraintType typeFor(ToolKind kind)
{
    switch (kind) {
        case ToolKind::Coincident: return ConstraintType::Coincident;
        case ToolKind::Parallel: return ConstraintType::Parallel;
        case ToolKind::Perpendicular: return ConstraintType::Perpendicular;
        case ToolKind::Tangent: return ConstraintType::Tangent;
        case ToolKind::Equal: return ConstraintType::Equal;
        case ToolKind::Ratio: return ConstraintType::Ratio;
        case ToolKind::Midpoint: return ConstraintType::Midpoint;
        case ToolKind::Concentric: return ConstraintType::Concentric;
        case ToolKind::Symmetric: return ConstraintType::Symmetric;
        case ToolKind::Collinear: return ConstraintType::Collinear;
        default: return ConstraintType::Horizontal;  // HorizontalVertical decides per pick
    }
}
}  // namespace

// =============================================================================================
// ConstraintTool
// =============================================================================================

ConstraintTool::ConstraintTool(SketchView& view, ToolKind kind)
    : Tool(view)
    , m_kind(kind)
{}

QString ConstraintTool::hint() const
{
    switch (m_kind) {
        case ToolKind::Coincident: return QObject::tr("Coincident: pick two points, or a point and a curve");
        case ToolKind::HorizontalVertical: return QObject::tr("Horizontal/Vertical: pick a line or two points");
        case ToolKind::Parallel: return QObject::tr("Parallel: pick two lines");
        case ToolKind::Perpendicular: return QObject::tr("Perpendicular: pick two lines");
        case ToolKind::Tangent: return QObject::tr("Tangent: pick a line and a circle/arc, or two circles/arcs");
        case ToolKind::Equal: return QObject::tr("Equal: pick two lines, or two circles/arcs");
        case ToolKind::Ratio:
            return QObject::tr("Ratio: pick two lines, or two circles/arcs; then type how many times the second the first is");
        case ToolKind::Midpoint: return QObject::tr("Midpoint: pick a point and a line");
        case ToolKind::Concentric: return QObject::tr("Concentric: pick two circles/arcs");
        case ToolKind::Symmetric: return QObject::tr("Symmetric: pick two points, then the line of symmetry");
        case ToolKind::Collinear: return QObject::tr("Collinear: pick two lines");
        case ToolKind::Fix: return QObject::tr("Fix/Unfix: click geometry to lock or unlock it");
        default: return {};
    }
}

size_t ConstraintTool::maxPicks() const
{
    switch (m_kind) {
        case ToolKind::Fix: return 1;
        case ToolKind::Symmetric: return 3;
        default: return 2;
    }
}

ConstraintTool::Outcome ConstraintTool::tryApply(const std::vector<Id>& refs)
{
    const Sketch& s = doc().sketch();
    if (refs.empty()) {
        return Outcome::Incomplete;
    }

    if (m_kind == ToolKind::Fix) {
        const std::vector<Id> ids = refs;
        if (std::ranges::all_of(ids, [&](Id id) { return s.isReference(id); })) {
            emit view().message(QObject::tr("The origin and axes are always fixed"));
            return Outcome::Rejected;
        }
        view().tryEdit(QObject::tr("Fix"), [&](Editor& ed) {
            for (Id id : ids) {
                if (!ed.sketch().isReference(id)) {
                    ed.setFixed(id, !ed.sketch().entity(id).fixed);
                }
            }
        });
        return Outcome::Applied;
    }

    ConstraintType type = typeFor(m_kind);
    if (m_kind == ToolKind::HorizontalVertical) {
        if (refs.size() == 1 && kindOf(s, refs[0]) == EntityKind::Line) {
            const core::Line& l = s.line(refs[0]);
            type = horizontalOrVertical(pos(s, l.p1), pos(s, l.p2));
        }
        else if (refs.size() == 2 && kindOf(s, refs[0]) == EntityKind::Point && kindOf(s, refs[1]) == EntityKind::Point) {
            type = horizontalOrVertical(pos(s, refs[0]), pos(s, refs[1]));
        }
    }

    if (!s.canAddConstraint(type, refs)) {
        return Outcome::Incomplete;
    }
    QString error;
    // A ratio starts as things are (nothing jumps), then asks for the value.
    const double value = type == ConstraintType::Ratio ? core::measure(s, type, refs) : 0.0;
    const Id made = doc().addConstraint(type, refs, value, &error);
    if (made == kNoId) {
        emit view().message(error);
        return Outcome::Rejected;
    }
    if (type == ConstraintType::Ratio) {
        QMetaObject::invokeMethod(&view(), [v = &view(), made] { v->editDimension(made); }, Qt::QueuedConnection);
    }
    if (doc().sketch().constraint(made).type != type) {
        emit view().message(QObject::tr("The point was already halfway along: it's now kept on the line"));
    }
    return Outcome::Applied;
}

std::vector<std::vector<Id>> ConstraintTool::groupsFor(const std::vector<Id>& items) const
{
    const Sketch& s = doc().sketch();
    std::vector<Id> points, curves;
    for (Id id : items) {
        (kindOf(s, id) == EntityKind::Point ? points : curves).push_back(id);
    }
    std::vector<std::vector<Id>> out;
    auto withFirst = [&](const std::vector<Id>& list) {  // the first paired with each of the rest
        for (size_t i = 1; i < list.size(); ++i) {
            out.push_back({list[0], list[i]});
        }
    };
    switch (m_kind) {
        case ToolKind::HorizontalVertical:
            for (Id c : curves) {
                if (kindOf(s, c) == EntityKind::Line) {
                    out.push_back({c});
                }
            }
            break;
        case ToolKind::Coincident:
            if (curves.size() == 1) {
                for (Id p : points) {
                    out.push_back({p, curves[0]});  // points onto the curve
                }
            }
            else if (curves.empty()) {
                withFirst(points);  // points together
            }
            break;
        case ToolKind::Parallel:
        case ToolKind::Equal:
        case ToolKind::Collinear:
        case ToolKind::Concentric:
        case ToolKind::Tangent:
            if (points.empty()) {
                withFirst(curves);
            }
            break;
        default: break;
    }
    return out;
}

bool ConstraintTool::applyToMany(const std::vector<Id>& items)
{
    const std::vector<std::vector<Id>> groups = groupsFor(items);
    if (groups.size() < 2) {
        return false;
    }
    // One undo step; pairs that would over-constrain are skipped.
    size_t applied = 0;
    doc().beginTransaction();
    for (const std::vector<Id>& refs : groups) {
        ConstraintType type = typeFor(m_kind);
        if (m_kind == ToolKind::HorizontalVertical) {
            const core::Line& l = doc().sketch().line(refs[0]);
            type = horizontalOrVertical(pos(doc().sketch(), l.p1), pos(doc().sketch(), l.p2));
        }
        if (doc().sketch().canAddConstraint(type, refs) && doc().addConstraint(type, refs) != kNoId) {
            ++applied;
        }
    }
    doc().endTransaction();
    if (applied < groups.size()) {
        emit view().message(QObject::tr("Constrained %1 of %2: the rest would over-constrain the sketch")
                                .arg(applied)
                                .arg(groups.size()));
    }
    return true;
}

void ConstraintTool::activate()
{
    // Selection first, then the tool: apply straight away if the selection fits.
    const Sketch& s = doc().sketch();
    std::vector<Id> selected;
    std::set<Id> ownPoints;  // ends and centres of selected curves (a box selection brings them)
    for (Id id : view().selection()) {
        if (s.hasEntity(id)) {
            selected.push_back(id);
            if (kindOf(s, id) != EntityKind::Point) {
                for (Id p : s.definingPoints(id)) {
                    ownPoints.insert(p);
                }
            }
        }
    }
    if (selected.empty()) {
        return;
    }
    std::vector<Id> curvesAndLonePoints;
    for (Id id : selected) {
        if (!ownPoints.contains(id)) {
            curvesAndLonePoints.push_back(id);
        }
    }
    auto done = [&] {
        view().setSelection({});
        // Hand back to the select tool once this tool switch has finished.
        QMetaObject::invokeMethod(&view(), [v = &view()] { v->setTool(ToolKind::Select); }, Qt::QueuedConnection);
    };
    for (const std::vector<Id>* items : {&selected, &curvesAndLonePoints}) {
        if (tryApply(*items) != Outcome::Incomplete) {
            done();
            return;
        }
    }
    // Several things at once: each line horizontal, all of them equal, ...
    if (applyToMany(curvesAndLonePoints) || applyToMany(selected)) {
        done();
        return;
    }
    if (curvesAndLonePoints.size() < maxPicks()) {
        m_picks = curvesAndLonePoints;  // partial: keep going from here
        view().setSelection({m_picks.begin(), m_picks.end()});
    }
    else {
        view().setSelection({});
    }
}

void ConstraintTool::press(const ToolEvent& e)
{
    if (e.button != Qt::LeftButton) {
        cancel();
        return;
    }
    // Coincident, a point picked, then where two curves cross: the point goes onto both.
    if (m_kind == ToolKind::Coincident && m_picks.size() == 1 && doc().sketch().hasEntity(m_picks[0])
        && kindOf(doc().sketch(), m_picks[0]) == EntityKind::Point) {
        // (not the axes: a curve clicked where it happens to cross one means just the curve)
        const Snap crossing = view().snap(e.screen);
        const Sketch& s = doc().sketch();
        if (crossing.onCurve2 != kNoId && crossing.point == kNoId && !s.isAxis(crossing.onCurve)
            && !s.isAxis(crossing.onCurve2)) {
            const Id p = m_picks[0];
            QString error;
            doc().beginTransaction();
            const bool first = doc().addConstraint(ConstraintType::Coincident, {p, crossing.onCurve}, 0.0, &error) != kNoId;
            const bool both = first && doc().addConstraint(ConstraintType::Coincident, {p, crossing.onCurve2}, 0.0, &error) != kNoId;
            doc().endTransaction();
            if (!both) {
                if (first) {
                    doc().undo();  // (all or nothing)
                }
                emit view().message(error);
            }
            m_picks.clear();
            view().setSelection({});
            refresh();
            return;
        }
    }
    const Id hit = view().pickAt(e.screen, false);
    if (hit == kNoId) {
        m_picks.clear();
        view().setSelection({});
        refresh();
        return;
    }
    if (std::ranges::find(m_picks, hit) == m_picks.end()) {
        m_picks.push_back(hit);
    }

    const Outcome outcome = tryApply(m_picks);
    if (outcome == Outcome::Incomplete && m_picks.size() >= maxPicks()) {
        std::string why;
        doc().sketch().canAddConstraint(typeFor(m_kind), m_picks, &why);
        emit view().message(QString::fromStdString(why));
        m_picks.clear();
    }
    else if (outcome != Outcome::Incomplete) {
        m_picks.clear();
    }
    view().setSelection({m_picks.begin(), m_picks.end()});
    refresh();
}

void ConstraintTool::move(const ToolEvent& e)
{
    view().setHovered(view().pickAt(e.screen, false));
}

bool ConstraintTool::cancel()
{
    if (m_picks.empty()) {
        return false;
    }
    m_picks.clear();
    view().setSelection({});
    refresh();
    return true;
}

// =============================================================================================
// DimensionTool
// =============================================================================================

QString DimensionTool::hint() const
{
    if (m_picks.empty()) {
        return QObject::tr("Dimension: pick a line, circle, arc, ellipse (near the axis to measure), or two points/lines");
    }
    bool incomplete = false;
    if (candidateFor(doc().sketch(), m_picks, m_mouse, &incomplete)) {
        return QObject::tr("Move to place the dimension and click; or pick a second entity");
    }
    return QObject::tr("Pick a second entity");
}

std::optional<DimensionTool::Candidate> DimensionTool::candidateFor(const Sketch& s, const std::vector<Id>& picks,
                                                                     Vec2 mouse, bool* incomplete)
{
    if (incomplete) {
        *incomplete = false;
    }
    for (Id id : picks) {
        if (!s.hasEntity(id)) {
            return std::nullopt;
        }
    }

    if (picks.size() == 1) {
        const Id a = picks[0];
        switch (kindOf(s, a)) {
            case EntityKind::Point:
                if (incomplete) {
                    *incomplete = true;
                }
                return std::nullopt;
            case EntityKind::Line: {
                const core::Line& l = s.line(a);
                return Candidate{linearTypeFor(pos(s, l.p1), pos(s, l.p2), mouse), {a}};
            }
            case EntityKind::Circle: return Candidate{ConstraintType::Diameter, {a}};
            case EntityKind::Arc: return Candidate{ConstraintType::Radius, {a}};
            case EntityKind::Ellipse:
            case EntityKind::EllipseArc: {
                // The semi-axis nearer the mouse: the major one as a radius, the minor one as the
                // distance from the centre to its point.
                const core::curve::EllipseFrame f = core::curve::ellipseFrame(s, a);
                const Vec2 d{mouse.x - f.c.x, mouse.y - f.c.y};
                const double along = std::abs(d.x * f.u.x + d.y * f.u.y) / std::max(f.a, 1e-9);
                const double across = std::abs(-d.x * f.u.y + d.y * f.u.x) / std::max(f.b, 1e-9);
                if (across > along) {
                    const std::array<Id, 3> ax = s.ellipseAxes(a);
                    return Candidate{ConstraintType::Distance, {ax[0], ax[2]}};
                }
                return Candidate{ConstraintType::Radius, {a}};
            }
            case EntityKind::Spline: return std::nullopt;
        }
    }

    if (picks.size() == 2) {
        Id a = picks[0], b = picks[1];
        const EntityKind ka = kindOf(s, a), kb = kindOf(s, b);
        if (ka == EntityKind::Point && kb == EntityKind::Point) {
            return Candidate{linearTypeFor(pos(s, a), pos(s, b), mouse), {a, b}};
        }
        if (ka == EntityKind::Line && kb == EntityKind::Point) {
            std::swap(a, b);
        }
        if (kindOf(s, a) == EntityKind::Point && kindOf(s, b) == EntityKind::Line) {
            return Candidate{ConstraintType::Distance, {a, b}};
        }
        if (s.isRound(a) && s.isRound(b)) {
            // Concentric circles/arcs: the gap between their radii.
            const core::Point& ca = s.point(kindOf(s, a) == EntityKind::Circle ? s.circle(a).center : s.arc(a).center);
            const core::Point& cb = s.point(kindOf(s, b) == EntityKind::Circle ? s.circle(b).center : s.arc(b).center);
            if (std::hypot(ca.x - cb.x, ca.y - cb.y) < 1e-6) {
                return Candidate{ConstraintType::Distance, {a, b}};
            }
            return std::nullopt;
        }
        if (ka == EntityKind::Line && kb == EntityKind::Line) {
            const core::Line& l1 = s.line(a);
            const core::Line& l2 = s.line(b);
            const Vec2 d1{pos(s, l1.p2).x - pos(s, l1.p1).x, pos(s, l1.p2).y - pos(s, l1.p1).y};
            const Vec2 d2{pos(s, l2.p2).x - pos(s, l2.p1).x, pos(s, l2.p2).y - pos(s, l2.p1).y};
            const double cross = d1.x * d2.y - d1.y * d2.x;
            if (std::abs(cross) > 1e-9 * std::hypot(d1.x, d1.y) * std::hypot(d2.x, d2.y)) {
                return Candidate{ConstraintType::Angle, {a, b}, core::angleFlipAt(s, a, b, mouse)};
            }
            return Candidate{ConstraintType::Distance, {l2.p1, a}};  // parallel: gap between them
        }
    }
    return std::nullopt;
}

void DimensionTool::reset()
{
    m_picks.clear();
    view().setSelection({});
    refresh();
}

void DimensionTool::press(const ToolEvent& e)
{
    if (e.button != Qt::LeftButton) {
        cancel();
        return;
    }
    m_mouse = e.world;
    const Sketch& s = doc().sketch();
    const Id hit = view().pickAt(e.screen, false);

    // Adding a pick, if it still makes (or could make) a dimension.
    if (hit != kNoId && m_picks.size() < 2 && std::ranges::find(m_picks, hit) == m_picks.end()) {
        std::vector<Id> tentative = m_picks;
        tentative.push_back(hit);
        bool incomplete = false;
        if (candidateFor(s, tentative, m_mouse, &incomplete) || incomplete) {
            m_picks = std::move(tentative);
            view().setSelection({m_picks.begin(), m_picks.end()});
            refresh();
            return;
        }
        if (m_picks.empty()) {
            emit view().message(QObject::tr("Can't dimension that"));
            return;
        }
    }

    // Otherwise: place the dimension we have.
    const auto candidate = candidateFor(s, m_picks, m_mouse);
    if (!candidate) {
        reset();
        return;
    }
    core::Constraint probe{kNoId, candidate->type, candidate->refs,
                           core::measure(s, candidate->type, candidate->refs, candidate->flip)};
    probe.flip = candidate->flip;
    const Vec2 offset = core::labelOffsetFor(s, probe, m_mouse);
    QString error;
    const Id id =
        doc().addConstraint(candidate->type, candidate->refs, probe.value, &error, offset.x, offset.y, candidate->flip);
    reset();
    if (id == kNoId) {
        emit view().message(error);
        return;
    }
    view().editDimension(id);
}

void DimensionTool::move(const ToolEvent& e)
{
    m_mouse = e.world;
    if (m_picks.empty()) {
        view().setHovered(view().pickAt(e.screen, false));
    }
    refresh();
}

bool DimensionTool::cancel()
{
    if (m_picks.empty()) {
        return false;
    }
    reset();
    return true;
}

void DimensionTool::paintOverlay(QPainter& painter)
{
    const Sketch& s = doc().sketch();
    const auto candidate = candidateFor(s, m_picks, m_mouse);
    if (!candidate) {
        return;
    }
    core::Constraint probe{kNoId, candidate->type, candidate->refs,
                           core::measure(s, candidate->type, candidate->refs, candidate->flip)};
    probe.flip = candidate->flip;
    const Vec2 offset = core::labelOffsetFor(s, probe, m_mouse);
    probe.labelX = offset.x;
    probe.labelY = offset.y;
    const core::DimensionLayout layout = core::layoutDimension(s, probe);
    if (layout.valid) {
        QColor color = theme::canvas().dimension;
        color.setAlpha(170);
        view().drawDimension(painter, layout, view().formatDimension(probe), color);
    }
}

// =============================================================================================
// GroupTool
// =============================================================================================

QString GroupTool::hint() const
{
    if (m_items.empty()) {
        return QObject::tr("Group: click the curves and points to hold together (drag a box for many), then Enter. "
                           "They'll keep their places relative to each other, moving and turning as one");
    }
    return QObject::tr("Group: %n gathered: click more (again: leave one out; a group's symbol: add to that group), "
                       "then Enter to group them; Esc: none",
                       nullptr, static_cast<int>(m_items.size()));
}

void GroupTool::activate()
{
    // Fusion's pre-selection: what's selected is grouped straight away.
    const Sketch& s = doc().sketch();
    std::vector<Id> selected;
    for (Id id : view().selection()) {
        const bool group = s.hasConstraint(id) && s.constraint(id).type == ConstraintType::Group;
        if ((s.hasEntity(id) && !s.isReference(id)) || group) {
            selected.push_back(id);  // (a group's symbol: added to that group)
        }
    }
    if (selected.empty()) {
        return;
    }
    m_items = selected;
    if (finish()) {
        view().setSelection({});
        QMetaObject::invokeMethod(&view(), [v = &view()] { v->setTool(ToolKind::Select); }, Qt::QueuedConnection);
    }
}

bool GroupTool::finish()
{
    if (m_items.empty()) {
        emit view().message(QObject::tr("Click what to group first"));
        return false;
    }
    QString error;
    const Id made = doc().addGroup(m_items, &error);
    if (made == kNoId) {
        emit view().message(error);
        return false;
    }
    m_items.clear();
    emit view().message(QObject::tr("Grouped: they now move as one (delete the group's symbol to ungroup)"));
    refresh();
    return true;
}

Id GroupTool::itemAt(QPointF screen) const
{
    const Sketch& s = doc().sketch();
    const Id hit = view().pickAt(screen, true, true);
    if (s.hasConstraint(hit)) {
        // A group's symbol: what's gathered joins that group.
        return s.constraint(hit).type == ConstraintType::Group ? hit : kNoId;
    }
    return hit != kNoId && s.hasEntity(hit) && !s.isReference(hit) ? hit : kNoId;
}

void GroupTool::move(const ToolEvent& e)
{
    if (m_pressed) {
        const QPointF d = e.screen - m_pressScreen;
        m_boxing = m_boxing || std::hypot(d.x(), d.y()) >= 4.0;
        m_boxEnd = e.screen;
    }
    m_hover = m_boxing ? kNoId : itemAt(e.screen);
    view().setHovered(m_hover);
    refresh();
}

void GroupTool::press(const ToolEvent& e)
{
    if (e.button != Qt::LeftButton) {
        return;
    }
    m_pressed = true;
    m_boxing = false;
    m_pressScreen = m_boxEnd = e.screen;
}

void GroupTool::release(const ToolEvent& e)
{
    if (e.button != Qt::LeftButton || !m_pressed) {
        return;
    }
    m_pressed = false;
    const Sketch& s = doc().sketch();
    if (m_boxing) {
        m_boxing = false;
        const bool crossing = e.screen.x() < m_pressScreen.x();
        for (Id id : core::boxSelect(s, view().toWorld(m_pressScreen), view().toWorld(e.screen), crossing,
                                     s.activeSublayer())) {
            if (!s.isReference(id) && std::ranges::find(m_items, id) == m_items.end()) {
                m_items.push_back(id);
            }
        }
    }
    else if (const Id hit = itemAt(e.screen); hit != kNoId) {
        if (const auto it = std::ranges::find(m_items, hit); it != m_items.end()) {
            m_items.erase(it);
        }
        else {
            m_items.push_back(hit);
        }
    }
    refresh();
}

bool GroupTool::keyPress(int key, const QString& /*text*/)
{
    if (key == Qt::Key_Return || key == Qt::Key_Enter) {
        finish();
        return true;
    }
    return false;
}

bool GroupTool::cancel()
{
    if (m_boxing || !m_items.empty()) {
        m_boxing = m_pressed = false;
        m_items.clear();
        refresh();
        return true;
    }
    return false;
}

void GroupTool::paintOverlay(QPainter& painter)
{
    const Sketch& s = doc().sketch();
    const QColor accent(0x4d, 0xa3, 0xff);
    painter.setBrush(Qt::NoBrush);
    for (Id id : m_items) {
        if (!s.hasEntity(id)) {
            continue;
        }
        if (s.entity(id).kind() == EntityKind::Point) {
            painter.setPen(QPen(accent, 2.0));
            painter.drawEllipse(view().toScreen(pos(s, id)), 5.0, 5.0);
            continue;
        }
        QPolygonF line;
        for (const Vec2& p : core::sample(s, id, 96)) {
            line << view().toScreen(p);
        }
        painter.setPen(QPen(accent, 3.0));
        painter.drawPolyline(line);
    }
    if (m_boxing) {
        const bool crossing = m_boxEnd.x() < m_pressScreen.x();
        QColor fill = crossing ? QColor(0x5a, 0xc8, 0x7a, 40) : QColor(0x4d, 0xa3, 0xff, 40);
        painter.setPen(QPen(crossing ? QColor(0x5a, 0xc8, 0x7a) : accent, 1.0, crossing ? Qt::DashLine : Qt::SolidLine));
        painter.setBrush(fill);
        painter.drawRect(QRectF(m_pressScreen, m_boxEnd).normalized());
    }
}