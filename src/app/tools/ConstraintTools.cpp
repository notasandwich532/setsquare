#include "ConstraintTools.h"

#include "../Document.h"
#include "../SketchView.h"
#include "../Theme.h"

#include <core/Dimensions.h>

#include <QPainter>

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
        case ToolKind::Midpoint: return ConstraintType::Midpoint;
        case ToolKind::Concentric: return ConstraintType::Concentric;
        case ToolKind::Symmetric: return ConstraintType::Symmetric;
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
        case ToolKind::Midpoint: return QObject::tr("Midpoint: pick a point and a line");
        case ToolKind::Concentric: return QObject::tr("Concentric: pick two circles/arcs");
        case ToolKind::Symmetric: return QObject::tr("Symmetric: pick two points, then the line of symmetry");
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
        if (std::ranges::all_of(ids, [&](Id id) { return s.isOrigin(id); })) {
            emit view().message(QObject::tr("The origin is always fixed"));
            return Outcome::Rejected;
        }
        view().tryModify(QObject::tr("Fix"), [&](Sketch& sk) {
            for (Id id : ids) {
                if (!sk.isOrigin(id)) {
                    sk.entity(id).fixed = !sk.entity(id).fixed;
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
    if (doc().addConstraint(type, refs, 0.0, &error) == kNoId) {
        emit view().message(error);
        return Outcome::Rejected;
    }
    return Outcome::Applied;
}

void ConstraintTool::activate()
{
    // Selection first, then the tool: apply straight away if the selection fits.
    std::vector<Id> selected;
    for (Id id : view().selection()) {
        if (doc().sketch().hasEntity(id)) {
            selected.push_back(id);
        }
    }
    if (selected.empty()) {
        return;
    }
    if (tryApply(selected) != Outcome::Incomplete) {
        view().setSelection({});
        // Done: hand back to the select tool once this tool switch has finished.
        QMetaObject::invokeMethod(&view(), [v = &view()] { v->setTool(ToolKind::Select); }, Qt::QueuedConnection);
        return;
    }
    if (selected.size() < maxPicks()) {
        m_picks = selected;  // partial: keep going from here
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
        return QObject::tr("Dimension: pick a line, circle, arc, or two points/lines");
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
        if (ka == EntityKind::Line && kb == EntityKind::Line) {
            const core::Line& l1 = s.line(a);
            const core::Line& l2 = s.line(b);
            const Vec2 d1{pos(s, l1.p2).x - pos(s, l1.p1).x, pos(s, l1.p2).y - pos(s, l1.p1).y};
            const Vec2 d2{pos(s, l2.p2).x - pos(s, l2.p1).x, pos(s, l2.p2).y - pos(s, l2.p1).y};
            const double cross = d1.x * d2.y - d1.y * d2.x;
            if (std::abs(cross) > 1e-9 * std::hypot(d1.x, d1.y) * std::hypot(d2.x, d2.y)) {
                return Candidate{ConstraintType::Angle, {a, b}};
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
    core::Constraint probe{kNoId, candidate->type, candidate->refs, core::measure(s, candidate->type, candidate->refs)};
    const Vec2 offset = core::labelOffsetFor(s, probe, m_mouse);
    QString error;
    const Id id = doc().addConstraint(candidate->type, candidate->refs, probe.value, &error, offset.x, offset.y);
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
    core::Constraint probe{kNoId, candidate->type, candidate->refs, core::measure(s, candidate->type, candidate->refs)};
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
