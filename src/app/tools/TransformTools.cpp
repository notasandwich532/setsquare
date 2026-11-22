#include "TransformTools.h"
#include "../NumberInput.h"

#include "../CommandPanel.h"
#include "../Document.h"
#include "../SketchView.h"
#include "../Theme.h"
#include "ShapeTools.h"

#include <core/Curves.h>
#include <core/Features.h>
#include <core/Geometry.h>
#include <core/Solver.h>
#include <core/Transform.h>

#include <QCheckBox>
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QLineEdit>
#include <QPainter>
#include <QSignalBlocker>
#include <QSpinBox>
#include <QTimer>

#include <algorithm>
#include <cmath>
#include <functional>
#include <numbers>

using core::Id;
using core::kNoId;
using core::Vec2;

namespace
{
constexpr double kRadToDeg = 180.0 / std::numbers::pi;
constexpr double kDegToRad = std::numbers::pi / 180.0;
constexpr double kHandleGap = 18.0;  // pixels between an origin and the arrows/knob around it

// Draws what `make` adds to a scratch copy of the sketch, in the preview style.
void drawGhost(SketchView& view, QPainter& painter, const std::function<void(core::Sketch&)>& make)
{
    core::Sketch scratch = view.document().sketch();
    const Id before = scratch.highestId();
    try {
        make(scratch);
    }
    catch (const core::SketchError&) {
        return;  // can't be done like this: nothing to show
    }
    view.drawPreviewSketch(painter, scratch, before);
}

// A pattern's copies after `edit` changes it, drawn over the sketch (for editing a pattern).
void drawPatternEdit(SketchView& view, QPainter& painter, const std::function<Id(core::Sketch&)>& edit)
{
    core::Sketch scratch = view.document().sketch();
    std::set<Id> copies;
    try {
        const Id k = edit(scratch);
        for (const auto& instance : core::feature::pattern(scratch.constraint(k)).copies) {
            for (Id e : instance) {
                copies.insert(e);
                for (Id p : scratch.definingPoints(e)) {
                    copies.insert(p);
                }
            }
        }
    }
    catch (const core::SketchError&) {
        return;
    }
    view.drawPreviewSketch(painter, scratch, [&](Id id) { return copies.contains(id); });
}

Vec2 operator+(Vec2 a, Vec2 b) { return {a.x + b.x, a.y + b.y}; }
Vec2 operator-(Vec2 a, Vec2 b) { return {a.x - b.x, a.y - b.y}; }
Vec2 operator*(double k, Vec2 v) { return {k * v.x, k * v.y}; }
double dot(Vec2 a, Vec2 b) { return a.x * b.x + a.y * b.y; }
double norm(Vec2 v) { return std::hypot(v.x, v.y); }

double wrapDegrees(double a)  // to (-180, 180]
{
    while (a > 180.0) {
        a -= 360.0;
    }
    while (a <= -180.0) {
        a += 360.0;
    }
    return a;
}

void setQuietly(QDoubleSpinBox* box, double value)
{
    const QSignalBlocker block(box);
    if (auto* e = qobject_cast<ExpressionSpinBox*>(box)) {
        e->clearExpression();  // (dragged: a value of its own now)
    }
    box->setValue(value);
}

QString length(double v)
{
    return QStringLiteral("%1 mm").arg(v, 0, 'f', 2);
}
}  // namespace

// =============================================================================================
// CommandTool
// =============================================================================================

CommandTool::CommandTool(SketchView& view)
    : Tool(view)
{}

CommandTool::~CommandTool()
{
    if (m_panel) {
        m_panel->hide();
        m_panel->deleteLater();  // it may be mid-signal (OK / Cancel)
    }
}

void CommandTool::activate()
{
    m_panel = new CommandPanel(title(), &view());
    buildPanel();
    m_panel->addButtons();
    QObject::connect(m_panel, &CommandPanel::accepted, m_panel, [this] { accept(); });
    QObject::connect(m_panel, &CommandPanel::rejected, m_panel, [this] {
        abandon();
        leave();
    });
    QObject::connect(m_panel, &CommandPanel::changed, m_panel, [this] {
        changed();
        refresh();
    });

    // What's already selected fills the first input that takes it (Fusion's pre-selection).
    if (!m_picks.empty() && m_picks[0].enabled && m_picks[0].ids.empty()) {
        std::vector<Id> pre;
        for (Id id : view().selection()) {
            if (m_picks[0].accepts(id)) {
                pre.push_back(id);
            }
        }
        m_picks[0].ids = std::move(pre);
    }
    int first = -1;
    for (size_t i = 0; i < m_picks.size(); ++i) {
        if (m_picks[i].enabled && m_picks[i].ids.empty()) {
            first = static_cast<int>(i);
            break;
        }
    }
    setCurrentPick(first);
    picksChanged();
    view().setFocus();
}

int CommandTool::addPick(const QString& label, const char* name, const QString& prompt,
                         std::function<bool(Id)> accepts, bool multiple)
{
    const int index = static_cast<int>(m_picks.size());
    Pick p;
    p.button = m_panel->addSelection(label, name);
    p.prompt = prompt;
    p.accepts = std::move(accepts);
    p.multiple = multiple;
    QObject::connect(p.button, &QPushButton::clicked, m_panel, [this, index] {
        setCurrentPick(index);
        view().setFocus();
    });
    QObject::connect(p.button, &SelectionButton::cleared, m_panel, [this, index] {
        setPicked(index, {});
        setCurrentPick(index);
    });
    m_picks.push_back(std::move(p));
    return index;
}

void CommandTool::setPicked(int input, std::vector<Id> ids)
{
    m_picks[static_cast<size_t>(input)].ids = std::move(ids);
    picksChanged();
}

void CommandTool::setCurrentPick(int input)
{
    m_current = input;
    for (size_t i = 0; i < m_picks.size(); ++i) {
        m_picks[i].button->setCurrent(static_cast<int>(i) == input);
    }
    refresh();
}

void CommandTool::setPickEnabled(int input, bool enabled)
{
    m_picks[static_cast<size_t>(input)].enabled = enabled;
    m_picks[static_cast<size_t>(input)].button->setEnabled(enabled);
    if (!enabled && m_current == input) {
        setCurrentPick(-1);
    }
}

void CommandTool::picksChanged()
{
    std::set<Id> shown;
    for (const Pick& p : m_picks) {
        p.button->setCount(static_cast<int>(p.ids.size()));
        shown.insert(p.ids.begin(), p.ids.end());
    }
    view().setSelection(std::move(shown));  // highlight what the command will use
    changed();
    refresh();
}

bool CommandTool::isGeometry(Id id) const
{
    const core::Sketch& s = doc().sketch();
    return s.hasEntity(id) && !s.isReference(id);
}

bool CommandTool::isText(Id id) const
{
    return doc().sketch().hasText(id);
}

bool CommandTool::isLine(Id id) const
{
    const core::Sketch& s = doc().sketch();
    return s.hasEntity(id) && s.entity(id).kind() == core::EntityKind::Line;
}

bool CommandTool::isPoint(Id id) const
{
    const core::Sketch& s = doc().sketch();
    return s.hasEntity(id) && s.entity(id).kind() == core::EntityKind::Point && !s.isHiddenReference(id);
}

bool CommandTool::isCurve(Id id) const
{
    const core::Sketch& s = doc().sketch();
    return isGeometry(id) && s.entity(id).kind() != core::EntityKind::Point;
}

Vec2 CommandTool::centreOf(const std::vector<Id>& items) const
{
    const core::Sketch& s = doc().sketch();
    double x0 = 1e300, y0 = 1e300, x1 = -1e300, y1 = -1e300;
    auto include = [&](Vec2 p) {
        x0 = std::min(x0, p.x);
        y0 = std::min(y0, p.y);
        x1 = std::max(x1, p.x);
        y1 = std::max(y1, p.y);
    };
    for (Id id : core::xform::expand(s, items)) {
        for (const Vec2& p : core::sample(s, id, 16)) {
            include(p);
        }
    }
    // Texts: their letters too (so one turns about its middle, not where it starts).
    for (Id id : items) {
        if (s.hasText(id) && s.hasEntity(s.text(id).anchor)) {
            const core::Text& t = s.text(id);
            for (const auto& contour : t.shape) {
                for (const core::Point& p : contour) {
                    const core::Point w = core::textToWorld(s.point(t.anchor), t.angle, p);
                    include({w.x, w.y});
                }
            }
        }
    }
    return x0 > x1 ? Vec2{} : Vec2{(x0 + x1) / 2, (y0 + y1) / 2};
}

double CommandTool::sizeOf(const std::vector<Id>& items) const
{
    const core::Sketch& s = doc().sketch();
    const Vec2 c = centreOf(items);
    double r = 0.0;
    for (Id id : core::xform::expand(s, items)) {
        for (const Vec2& p : core::sample(s, id, 16)) {
            r = std::max(r, norm(p - c));
        }
    }
    return r;
}

QString CommandTool::hint() const
{
    if (m_current >= 0) {
        return QStringLiteral("%1: %2, then OK (Enter)").arg(title(), m_picks[static_cast<size_t>(m_current)].prompt);
    }
    return QObject::tr("%1: adjust in the panel or drag the handles, then OK (Enter). Esc cancels").arg(title());
}

int CommandTool::handleAt(QPointF screen) const
{
    for (const Handle& h : handles()) {
        if (hitHandle(view(), h, screen)) {
            return h.id;
        }
    }
    return -1;
}

void CommandTool::press(const ToolEvent& e)
{
    if (e.button != Qt::LeftButton) {
        return;
    }
    m_mouse = e.world;
    if (const int h = handleAt(e.screen); h >= 0) {
        m_dragHandle = h;
        m_pressedAt = e.world;
        beginHandleDrag(h);
        refresh();
        return;
    }
    if (m_current < 0) {
        return;
    }
    Pick& pick = m_picks[static_cast<size_t>(m_current)];
    const Id hit = view().pickAt(e.screen, false);
    if (hit != kNoId && pick.accepts(hit)) {
        if (pick.multiple) {
            if (std::erase(pick.ids, hit) == 0) {
                pick.ids.push_back(hit);
            }
        }
        else {
            pick.ids = {hit};
            // On to the next input still waiting for something.
            int next = -1;
            for (size_t i = 0; i < m_picks.size(); ++i) {
                if (m_picks[i].enabled && m_picks[i].ids.empty()) {
                    next = static_cast<int>(i);
                    break;
                }
            }
            setCurrentPick(next);
        }
        picksChanged();
        return;
    }
    if (pickLocation(m_current, view().snap(e.screen))) {
        picksChanged();
    }
}

void CommandTool::move(const ToolEvent& e)
{
    m_mouse = e.world;
    if (m_dragHandle >= 0) {
        dragHandle(m_dragHandle, e.world, m_pressedAt, e.modifiers);
        refresh();
        return;
    }
    m_hotHandle = handleAt(e.screen);
    Id hover = kNoId;
    if (m_hotHandle < 0 && m_current >= 0) {
        const Id hit = view().pickAt(e.screen, false);
        if (hit != kNoId && m_picks[static_cast<size_t>(m_current)].accepts(hit)) {
            hover = hit;
        }
    }
    view().setHovered(hover);
    refresh();
}

void CommandTool::release(const ToolEvent& e)
{
    if (e.button == Qt::LeftButton && m_dragHandle >= 0) {
        m_dragHandle = -1;
        refresh();
    }
}

bool CommandTool::keyPress(int key, const QString& /*text*/)
{
    if (key == Qt::Key_Return || key == Qt::Key_Enter) {
        accept();
        return true;
    }
    return false;
}

bool CommandTool::cancel()
{
    // Only backs out of a handle drag; otherwise Esc leaves (and the destructor tidies up).
    if (m_dragHandle >= 0) {
        m_dragHandle = -1;
        refresh();
        return true;
    }
    return false;
}

void CommandTool::paintOverlay(QPainter& painter)
{
    paintPreview(painter);
    for (const Handle& h : handles()) {
        drawHandle(view(), painter, h, h.id == m_hotHandle || h.id == m_dragHandle);
    }
    if (m_dragHandle >= 0) {
        if (const QString label = dragLabel(m_dragHandle); !label.isEmpty()) {
            view().drawLabel(painter, m_mouse, label);
        }
    }
}

bool CommandTool::accept()
{
    if (m_committed) {
        return true;
    }
    if (!ready()) {
        emit view().message(QObject::tr("%1: fill in the highlighted inputs first").arg(title()));
        return false;
    }
    if (!commit()) {
        return false;
    }
    m_committed = true;
    view().setSelection({});
    leave();
    return true;
}

void CommandTool::leave()
{
    if (m_leaving) {
        return;
    }
    m_leaving = true;
    // Not now: we may be inside the panel's signal, and switching tools deletes this tool.
    SketchView* v = &view();
    const ToolKind kind = v->toolKind();
    QTimer::singleShot(0, v, [v, kind] {
        if (v->toolKind() == kind) {
            v->setTool(ToolKind::Select);
        }
    });
}

// =============================================================================================
// Move / Copy
// =============================================================================================

MoveTool::~MoveTool()
{
    if (!committed()) {
        stopLiveMove();  // put everything back
    }
}

void MoveTool::buildPanel()
{
    const SketchView::ModifyOptions& o = view().modifyOptions();
    m_objects = addPick(QObject::tr("Objects"), "objects", QObject::tr("click the objects (and texts) to move"),
                        [this](Id id) { return isGeometry(id) || isText(id); }, true);
    m_about = addPick(QObject::tr("Rotate about"), "about",
                      QObject::tr("click the point to rotate about, or anywhere (none: the objects' centre)"),
                      [this](Id id) { return isPoint(id); }, false);
    m_dx = panel()->addNumber(QObject::tr("X distance"), "dx", 0.0, -1e6, 1e6);
    m_dy = panel()->addNumber(QObject::tr("Y distance"), "dy", 0.0, -1e6, 1e6);
    m_angle = panel()->addNumber(QObject::tr("Angle"), "angle", 0.0, -360.0, 360.0, QStringLiteral(" °"));
    m_copy = panel()->addCheck(QObject::tr("Create copy"), "copy", o.copy);
    m_note = panel()->addNote("turnNote");
}

namespace
{
QString constraintCount(size_t n)
{
    return n == 1 ? QObject::tr("1 horizontal/vertical constraint")
                  : QObject::tr("%1 horizontal/vertical constraints").arg(n);
}
}  // namespace

QString MoveTool::turnNote() const
{
    const std::vector<Id>& objects = picked(m_objects);
    if (objects.empty()) {
        return {};
    }
    if (m_copy->isChecked()) {
        // (A copy turned by right angles has them swapped; by anything else, they're left out.)
        if (std::abs(std::remainder(m_angle->value(), 90.0)) <= 1e-9) {
            return {};
        }
        const std::vector<Id> ids = core::xform::expand(doc().sketch(), objects);
        const std::set<Id> in(ids.begin(), ids.end());
        size_t n = 0;
        for (const auto& [id, c] : doc().sketch().constraints()) {
            const bool axial = c.type == core::ConstraintType::Horizontal || c.type == core::ConstraintType::Vertical
                            || c.type == core::ConstraintType::HorizontalDistance
                            || c.type == core::ConstraintType::VerticalDistance;
            n += axial && std::ranges::all_of(c.refs, [&](Id r) { return in.contains(r); });
        }
        return n == 0 ? QString()
                      : QObject::tr("⚠ The copy leaves out %1: turned, they wouldn't hold.").arg(constraintCount(n));
    }
    const size_t n = doc().droppedByTurn().size();
    return n == 0 ? QString()
                  : QObject::tr("⚠ Turning drops %1: they'd stop it turning.").arg(constraintCount(n));
}

MoveTool::Motion MoveTool::motion() const
{
    return {m_dx->value(), m_dy->value(), m_angle->value()};
}

bool MoveTool::ready() const
{
    return !picked(m_objects).empty();
}

void MoveTool::stopLiveMove()
{
    if (!m_moving.empty()) {
        doc().cancelDrag();
        m_moving.clear();
    }
}

void MoveTool::abandon()
{
    stopLiveMove();
}

void MoveTool::changed()
{
    if (!m_note) {
        return;  // still building the panel
    }
    liveMove();
    panel()->setNote(m_note, turnNote());
}

void MoveTool::liveMove()
{
    view().modifyOptions().copy = m_copy->isChecked();
    const std::vector<Id>& objects = picked(m_objects);
    // What it turns about: the point picked, or the place clicked, or else the objects' centre.
    // Found with the geometry where it was (the point may be one of those moving).
    const Id aboutPoint = picked(m_about).empty() ? kNoId : picked(m_about).front();
    if (aboutPoint != kNoId) {
        m_hasLocation = false;
    }
    if (m_hasLocation) {
        pickButton(m_about)->setText(QObject::tr("▸ (%1, %2)").arg(m_location.x, 0, 'f', 1).arg(m_location.y, 0, 'f', 1));
    }
    if (objects != m_moving || aboutPoint != m_pivotPoint || m_hasLocation != m_pivotLocated
        || (m_hasLocation && (m_location.x != m_pivotLocation.x || m_location.y != m_pivotLocation.y))
        || m_moving.empty()) {
        stopLiveMove();  // the geometry goes back before measuring it again
        if (aboutPoint != kNoId) {
            const core::Point& p = doc().sketch().point(aboutPoint);
            m_pivot = {p.x, p.y};
        }
        else if (m_hasLocation) {
            m_pivot = m_location;
        }
        else {
            m_pivot = centreOf(objects);
        }
        m_pivotPoint = aboutPoint;
        m_pivotLocated = m_hasLocation;
        m_pivotLocation = m_location;
    }
    const Motion m = motion();
    const bool still = m.dx == 0.0 && m.dy == 0.0 && m.angle == 0.0;
    if (m_copy->isChecked() || objects.empty()) {
        stopLiveMove();
        return;
    }
    if (m_moving.empty()) {
        if (still) {
            return;
        }
        // Move the real geometry, as a drag: whatever is attached follows.
        std::vector<Id> points;
        for (Id id : core::xform::expand(doc().sketch(), objects)) {
            if (doc().sketch().entity(id).kind() == core::EntityKind::Point) {
                points.push_back(id);
            }
        }
        doc().beginDrag(points);
        m_moving = objects;
    }
    doc().dragMove(m.dx, m.dy, m.angle, m_pivot);
}

bool MoveTool::commit()
{
    const Motion m = motion();
    const QString note = turnNote();  // (said again once the panel's gone)
    if (m_copy->isChecked()) {
        stopLiveMove();
        const std::vector<Id> objects = picked(m_objects);
        const Vec2 about = m_pivot;
        if (!view().tryEdit(QObject::tr("Copy"), [&](Editor& ed) { ed.copy(objects, m.dx, m.dy, m.angle, about); })) {
            return false;
        }
    }
    else if (!m_moving.empty()) {
        doc().endDrag();  // one undo step, recorded as s.move(...)
        m_moving.clear();
    }
    if (!note.isEmpty()) {
        emit view().message(note);
    }
    return true;
}

bool MoveTool::pickLocation(int input, const Snap& snap)
{
    if (input != m_about) {
        return false;
    }
    m_location = snap.pos;
    m_hasLocation = true;
    setCurrentPick(-1);
    return true;
}

void MoveTool::paintPreview(QPainter& painter)
{
    const std::vector<Id>& objects = picked(m_objects);
    if (!objects.empty() && (m_pivotPoint != kNoId || m_pivotLocated)) {
        view().drawSnapMarker(painter, Snap{m_pivot});  // (where it turns about)
    }
    if (!m_copy->isChecked() || objects.empty()) {
        return;
    }
    const Motion m = motion();
    drawGhost(view(), painter, [&](core::Sketch& s) {
        core::xform::copy(s, objects, Editor::moveMap(m.dx, m.dy, m.angle, m_pivot));
    });
}

std::vector<Handle> MoveTool::handles() const
{
    if (picked(m_objects).empty()) {
        return {};
    }
    const Motion m = motion();
    const Vec2 at = m_pivot + Vec2{m.dx, m.dy};
    const double gap = kHandleGap / view().zoom();
    const double turn = (45.0 + m.angle) * kDegToRad;
    return {
        {Handle::Kind::Arrow, 1, at + Vec2{gap, 0}, {1, 0}},
        {Handle::Kind::Arrow, 2, at + Vec2{0, gap}, {0, 1}},
        {Handle::Kind::Rotate, 3, at + (4.0 * gap) * Vec2{std::cos(turn), std::sin(turn)}, {}},
        {Handle::Kind::Move, 4, at, {}},
    };
}

void MoveTool::beginHandleDrag(int /*id*/)
{
    m_dragStart = motion();
}

void MoveTool::dragHandle(int id, Vec2 world, Vec2 pressedAt, Qt::KeyboardModifiers modifiers)
{
    const Vec2 d = world - pressedAt;
    switch (id) {
        case 1: setQuietly(m_dx, m_dragStart.dx + d.x); break;
        case 2: setQuietly(m_dy, m_dragStart.dy + d.y); break;
        case 4:
            setQuietly(m_dx, m_dragStart.dx + d.x);
            setQuietly(m_dy, m_dragStart.dy + d.y);
            break;
        case 3: {
            const Vec2 c = m_pivot + Vec2{m_dragStart.dx, m_dragStart.dy};
            double a = m_dragStart.angle
                + (std::atan2(world.y - c.y, world.x - c.x) - std::atan2(pressedAt.y - c.y, pressedAt.x - c.x))
                    * kRadToDeg;
            a = wrapDegrees(a);
            if (modifiers & Qt::ShiftModifier) {
                a = std::round(a / 15.0) * 15.0;
            }
            setQuietly(m_angle, a);
            break;
        }
        default: return;
    }
    changed();
}

QString MoveTool::dragLabel(int id) const
{
    const Motion m = motion();
    if (id == 3) {
        return QStringLiteral("%1°").arg(m.angle, 0, 'f', 1);
    }
    return QStringLiteral("X %1   Y %2").arg(length(m.dx), length(m.dy));
}

// =============================================================================================
// Scale
// =============================================================================================

void ScaleTool::buildPanel()
{
    m_objects = addPick(QObject::tr("Objects"), "objects", QObject::tr("click the objects to scale"),
                        [this](Id id) { return isGeometry(id); }, true);
    m_point = addPick(QObject::tr("Point"), "point", QObject::tr("click the point to scale about (or anywhere)"),
                      [this](Id id) { return isPoint(id); }, false);
    m_factor = panel()->addNumber(QObject::tr("Scale factor"), "factor", view().modifyOptions().scaleFactor, 0.001,
                                  1000.0, QString());
}

Vec2 ScaleTool::about() const
{
    if (!picked(m_point).empty()) {
        const core::Point& p = doc().sketch().point(picked(m_point).front());
        return {p.x, p.y};
    }
    return m_location;
}

bool ScaleTool::ready() const
{
    return !picked(m_objects).empty() && (!picked(m_point).empty() || m_hasLocation);
}

bool ScaleTool::pickLocation(int input, const Snap& snap)
{
    if (input != m_point) {
        return false;
    }
    m_location = snap.pos;
    m_hasLocation = true;
    setCurrentPick(-1);
    return true;
}

void ScaleTool::changed()
{
    if (!m_factor) {
        return;
    }
    if (!picked(m_point).empty()) {
        m_hasLocation = false;
    }
    if (m_hasLocation) {
        pickButton(m_point)->setText(QObject::tr("▸ (%1, %2)").arg(m_location.x, 0, 'f', 1).arg(m_location.y, 0, 'f', 1));
    }
    view().modifyOptions().scaleFactor = m_factor->value();
}

double ScaleTool::reach() const
{
    return std::max(sizeOf(picked(m_objects)), 40.0 / view().zoom());
}

bool ScaleTool::commit()
{
    const std::vector<Id> objects = picked(m_objects);
    const Vec2 centre = about();
    const double factor = m_factor->value();
    return view().tryEdit(QObject::tr("Scale"), [&](Editor& ed) { ed.scale(objects, factor, centre); });
}

void ScaleTool::paintPreview(QPainter& painter)
{
    if (!ready()) {
        return;
    }
    // The real outcome: scaled in a scratch copy and solved (fixed geometry and the origin stay,
    // so what's attached to them may not simply grow).
    const std::vector<Id>& objects = picked(m_objects);
    const Vec2 centre = about();
    core::Sketch scratch = doc().sketch();
    try {
        core::xform::apply(scratch, objects, core::Affine::scaling(centre, m_factor->value()));
        core::solve(scratch);
    }
    catch (const core::SketchError&) {
        return;
    }
    const std::vector<Id> shown = core::xform::expand(scratch, objects);
    view().drawPreviewSketch(painter, scratch, [&](Id id) { return std::ranges::find(shown, id) != shown.end(); });
    view().drawSnapMarker(painter, Snap{centre});
}

std::vector<Handle> ScaleTool::handles() const
{
    if (!ready()) {
        return {};
    }
    return {{Handle::Kind::Arrow, 1, about() + Vec2{reach() * m_factor->value(), 0}, {1, 0}}};
}

void ScaleTool::beginHandleDrag(int /*id*/)
{
    m_dragStart = m_factor->value();
}

void ScaleTool::dragHandle(int /*id*/, Vec2 world, Vec2 pressedAt, Qt::KeyboardModifiers /*modifiers*/)
{
    setQuietly(m_factor, std::max(0.001, m_dragStart + (world.x - pressedAt.x) / reach()));
    changed();
}

QString ScaleTool::dragLabel(int /*id*/) const
{
    return QStringLiteral("× %1").arg(m_factor->value(), 0, 'f', 3);
}

// =============================================================================================
// Mirror
// =============================================================================================

void MirrorTool::buildPanel()
{
    m_objects = addPick(QObject::tr("Objects"), "objects", QObject::tr("click the objects (and texts) to mirror"),
                        [this](Id id) { return isGeometry(id) || isText(id); }, true);
    m_line = addPick(QObject::tr("Mirror line"), "line", QObject::tr("click the line (or axis) to mirror across"),
                     [this](Id id) { return isLine(id); }, false);
}

bool MirrorTool::ready() const
{
    return !picked(m_objects).empty() && !picked(m_line).empty();
}

bool MirrorTool::commit()
{
    const std::vector<Id> objects = picked(m_objects);
    const Id line = picked(m_line).front();
    return view().tryEdit(QObject::tr("Mirror"), [&](Editor& ed) { ed.mirror(objects, line); });
}

void MirrorTool::paintPreview(QPainter& painter)
{
    if (!ready()) {
        return;
    }
    const std::vector<Id>& objects = picked(m_objects);
    const Id line = picked(m_line).front();
    drawGhost(view(), painter, [&](core::Sketch& s) { core::xform::mirror(s, objects, line); });
}

// =============================================================================================
// Rectangular pattern
// =============================================================================================

void RectangularPatternTool::buildPanel()
{
    const core::Sketch& s = doc().sketch();
    const SketchView::ModifyOptions& o = view().modifyOptions();
    core::feature::LinearPatternParams p{o.patternCount, o.patternSpacing, o.patternAngle, o.patternCount2,
                                         o.patternSpacing2};
    std::vector<Id> sources;
    if (m_editing != kNoId && s.hasConstraint(m_editing)) {
        p = core::feature::linearParams(s.constraint(m_editing));
        sources = core::feature::pattern(s.constraint(m_editing)).sources;
    }
    else {
        m_editing = kNoId;
    }
    m_angle = p.angle;

    m_objects = addPick(QObject::tr("Objects"), "objects", QObject::tr("click the objects (and texts) to pattern"),
                        [this](Id id) { return isGeometry(id) || isText(id); }, true);
    m_direction = addPick(QObject::tr("Direction/s"), "direction",
                          QObject::tr("click a line for the first direction (or OK for the X/Y axes)"),
                          [this](Id id) { return isLine(id); }, false);
    m_distribution = panel()->addChoice(QObject::tr("Distribution"), "distribution",
                                        {QObject::tr("Extent"), QObject::tr("Spacing")});
    const QStringList ways{QObject::tr("One Direction"), QObject::tr("Symmetric")};
    const int counts[2] = {p.count, p.count2};
    const double spacings[2] = {p.spacing, std::abs(p.spacing2)};
    const double signs[2] = {1.0, p.spacing2 < 0 ? -1.0 : 1.0};
    const bool symmetric[2] = {p.symmetric, p.symmetric2};
    for (int d = 0; d < 2; ++d) {
        m_count[d] = panel()->addCount(QObject::tr("Quantity"), d == 0 ? "quantity" : "quantity2", counts[d], 1);
        const double extent = spacings[d] * std::max(1, counts[d] - 1);
        m_distance[d] = panel()->addNumber(QObject::tr("Distance"), d == 0 ? "distance" : "distance2",
                                           signs[d] * extent, -1e6, 1e6);
        m_symmetric[d] = panel()->addChoice(QObject::tr("Direction"), d == 0 ? "symmetric" : "symmetric2", ways,
                                            symmetric[d] ? 1 : 0);
    }
    if (m_editing != kNoId) {
        setPicked(m_objects, sources);
        setPickEnabled(m_objects, false);
        // What it's worked out from, shown to change: a spacing as itself (Spacing, not Extent).
        const auto& settings = s.constraint(m_editing).settings;
        const char* countKeys[2] = {"count", "count2"};
        const char* spacingKeys[2] = {"spacing", "spacing2"};
        for (int d = 0; d < 2; ++d) {
            if (const auto it = settings.find(countKeys[d]); it != settings.end()) {
                static_cast<ExpressionCountBox*>(m_count[d])->setExpression(QString::fromStdString(it->second));
            }
            if (const auto it = settings.find(spacingKeys[d]); it != settings.end()) {
                m_distribution->setCurrentIndex(1);
                static_cast<ExpressionSpinBox*>(m_distance[d])->setExpression(QString::fromStdString(it->second));
            }
        }
    }
}

Editor::Settings RectangularPatternTool::settings() const
{
    Editor::Settings out;
    QString counts[2];
    for (int d = 0; d < 2; ++d) {
        const QString e = static_cast<ExpressionCountBox*>(m_count[d])->expression();
        counts[d] = e.isEmpty() ? QString::number(m_count[d]->value()) : QStringLiteral("(%1)").arg(e);
        if (!e.isEmpty()) {
            out[d == 0 ? "count" : "count2"] = e.toStdString();
        }
    }
    // A distance worked out: the spacing from it as params() makes it (its size; an extent shared
    // between the gaps; the second direction's sign as the flips make it).
    const bool flip1 = m_distance[0]->value() < 0, flip2 = m_distance[1]->value() < 0;
    for (int d = 0; d < 2; ++d) {
        const QString e = static_cast<ExpressionSpinBox*>(m_distance[d])->expression();
        if (e.isEmpty()) {
            continue;
        }
        QString size = m_distance[d]->value() < 0 ? QStringLiteral("-(%1)").arg(e) : e;
        if (m_distribution->currentIndex() == 0) {
            size = QStringLiteral("(%1) / max(1, %2 - 1)").arg(size, counts[d]);
        }
        if (d == 1 && flip1 != flip2) {
            size = QStringLiteral("-(%1)").arg(size);
        }
        out[d == 0 ? "spacing" : "spacing2"] = size.toStdString();
    }
    return out;
}

double RectangularPatternTool::baseAngle() const
{
    if (!picked(m_direction).empty()) {
        const core::Sketch& s = doc().sketch();
        const core::Line& l = s.line(picked(m_direction).front());
        const core::Point& a = s.point(l.p1);
        const core::Point& b = s.point(l.p2);
        return std::atan2(b.y - a.y, b.x - a.x) * kRadToDeg;
    }
    return m_angle;
}

Vec2 RectangularPatternTool::axis(int d) const
{
    const double a = (baseAngle() + (d == 1 ? 90.0 : 0.0)) * kDegToRad;
    return {std::cos(a), std::sin(a)};
}

double RectangularPatternTool::extent(int d) const
{
    const double distance = m_distance[d]->value();
    const int steps = std::max(1, m_count[d]->value() - 1);
    return m_distribution->currentIndex() == 0 ? distance : distance * steps;
}

core::feature::LinearPatternParams RectangularPatternTool::params() const
{
    core::feature::LinearPatternParams p;
    double spacing[2];
    for (int d = 0; d < 2; ++d) {
        const int steps = std::max(1, m_count[d]->value() - 1);
        const double distance = std::abs(m_distance[d]->value());
        spacing[d] = std::max(1e-3, m_distribution->currentIndex() == 0 ? distance / steps : distance);
    }
    const bool flip1 = m_distance[0]->value() < 0, flip2 = m_distance[1]->value() < 0;
    p.count = m_count[0]->value();
    p.count2 = m_count[1]->value();
    p.spacing = spacing[0];
    // A negative first distance turns the whole grid round; the second direction then follows.
    p.angle = baseAngle() + (flip1 ? 180.0 : 0.0);
    p.spacing2 = flip1 != flip2 ? -spacing[1] : spacing[1];
    p.symmetric = m_symmetric[0]->currentIndex() == 1;
    p.symmetric2 = m_symmetric[1]->currentIndex() == 1;
    return p;
}

bool RectangularPatternTool::ready() const
{
    return !picked(m_objects).empty() && m_count[0]->value() * m_count[1]->value() > 1;
}

void RectangularPatternTool::changed()
{
    if (!m_count[1]) {
        return;
    }
    for (int d = 0; d < 2; ++d) {
        m_distance[d]->setEnabled(m_count[d]->value() > 1);
    }
    if (m_editing == kNoId) {
        const auto p = params();
        SketchView::ModifyOptions& o = view().modifyOptions();
        o.patternCount = p.count;
        o.patternSpacing = p.spacing;
        o.patternCount2 = p.count2;
        o.patternSpacing2 = std::abs(p.spacing2);
    }
}

bool RectangularPatternTool::commit()
{
    const auto p = params();
    const Editor::Settings worked = settings();
    const std::vector<Id> objects = picked(m_objects);
    return view().tryEdit(QObject::tr("Pattern"), [&](Editor& ed) {
        if (m_editing != kNoId) {
            ed.setPattern(m_editing, p, worked);
        }
        else {
            ed.rectangularPattern(objects, p, worked);
        }
    });
}

void RectangularPatternTool::paintPreview(QPainter& painter)
{
    if (!ready()) {
        return;
    }
    const auto p = params();
    if (m_editing != kNoId) {
        drawPatternEdit(view(), painter,
                        [&](core::Sketch& s) { return core::feature::setLinearPattern(s, m_editing, p); });
        return;
    }
    const std::vector<Id> objects = picked(m_objects);
    drawPreviewEdit(view(), painter, [&](Editor& ed) { ed.rectangularPattern(objects, p); });
}

std::vector<Handle> RectangularPatternTool::handles() const
{
    if (picked(m_objects).empty()) {
        return {};
    }
    const Vec2 origin = centreOf(picked(m_objects));
    const double gap = kHandleGap / view().zoom();
    std::vector<Handle> out;
    for (int d = 0; d < 2; ++d) {
        const double e = extent(d);
        const double side = e < 0 ? -1.0 : 1.0;
        const Vec2 u = axis(d);
        out.push_back({Handle::Kind::Arrow, d + 1, origin + (e + side * gap) * u, side * u});
    }
    return out;
}

void RectangularPatternTool::beginHandleDrag(int id)
{
    m_dragStart = extent(id - 1);
}

void RectangularPatternTool::dragHandle(int id, Vec2 world, Vec2 pressedAt, Qt::KeyboardModifiers /*modifiers*/)
{
    const int d = id - 1;
    const double e = m_dragStart + dot(world - pressedAt, axis(d));
    const int steps = std::max(1, m_count[d]->value() - 1);
    setQuietly(m_distance[d], m_distribution->currentIndex() == 0 ? e : e / steps);
    changed();
}

QString RectangularPatternTool::dragLabel(int id) const
{
    const int d = id - 1;
    return QStringLiteral("%1   × %2").arg(length(m_distance[d]->value())).arg(m_count[d]->value());
}

// =============================================================================================
// Circular pattern
// =============================================================================================

void CircularPatternTool::buildPanel()
{
    const core::Sketch& s = doc().sketch();
    const SketchView::ModifyOptions& o = view().modifyOptions();
    core::feature::CircularPatternParams p{o.circularCount, o.circularAngle};
    core::feature::Pattern existing;
    if (m_editing != kNoId && s.hasConstraint(m_editing)) {
        p = core::feature::circularParams(s.constraint(m_editing));
        existing = core::feature::pattern(s.constraint(m_editing));
    }
    else {
        m_editing = kNoId;
    }
    m_objects = addPick(QObject::tr("Objects"), "objects", QObject::tr("click the objects (and texts) to pattern"),
                        [this](Id id) { return isGeometry(id) || isText(id); }, true);
    m_center = addPick(QObject::tr("Center point"), "center", QObject::tr("click the centre point"),
                       [this](Id id) { return isPoint(id); }, false);
    const bool full = p.angle >= 360.0 - 1e-9;
    m_type = panel()->addChoice(QObject::tr("Type"), "type", {QObject::tr("Full"), QObject::tr("Angle")}, full ? 0 : 1);
    m_count = panel()->addCount(QObject::tr("Quantity"), "quantity", p.count, 2);
    m_angle = panel()->addNumber(QObject::tr("Total angle"), "angle", full ? 180.0 : p.angle, 0.001, 360.0,
                                 QStringLiteral(" °"));
    if (m_editing != kNoId) {
        setPicked(m_objects, existing.sources);
        setPicked(m_center, {existing.center});
        setPickEnabled(m_objects, false);
        setPickEnabled(m_center, false);
        const auto& settings = s.constraint(m_editing).settings;
        if (const auto it = settings.find("count"); it != settings.end()) {
            static_cast<ExpressionCountBox*>(m_count)->setExpression(QString::fromStdString(it->second));
        }
        if (const auto it = settings.find("angle"); it != settings.end()) {
            m_type->setCurrentIndex(1);
            static_cast<ExpressionSpinBox*>(m_angle)->setExpression(QString::fromStdString(it->second));
        }
    }
}

Editor::Settings CircularPatternTool::settings() const
{
    Editor::Settings out;
    if (const QString e = static_cast<ExpressionCountBox*>(m_count)->expression(); !e.isEmpty()) {
        out["count"] = e.toStdString();
    }
    if (m_type->currentIndex() == 1) {
        if (const QString e = static_cast<ExpressionSpinBox*>(m_angle)->expression(); !e.isEmpty()) {
            out["angle"] = e.toStdString();
        }
    }
    return out;
}

core::feature::CircularPatternParams CircularPatternTool::params() const
{
    return {m_count->value(), m_type->currentIndex() == 0 ? 360.0 : m_angle->value()};
}

bool CircularPatternTool::ready() const
{
    return !picked(m_objects).empty() && !picked(m_center).empty();
}

void CircularPatternTool::changed()
{
    if (!m_angle) {
        return;
    }
    panel()->setRowVisible(m_angle, m_type->currentIndex() == 1);
    if (m_editing == kNoId) {
        view().modifyOptions().circularCount = m_count->value();
        view().modifyOptions().circularAngle = params().angle;
    }
}

bool CircularPatternTool::commit()
{
    const auto p = params();
    const Editor::Settings worked = settings();
    const std::vector<Id> objects = picked(m_objects);
    const Id center = picked(m_center).front();
    return view().tryEdit(QObject::tr("Pattern"), [&](Editor& ed) {
        if (m_editing != kNoId) {
            ed.setPattern(m_editing, p, worked);
        }
        else {
            ed.circularPattern(objects, center, p, worked);
        }
    });
}

void CircularPatternTool::paintPreview(QPainter& painter)
{
    if (!ready()) {
        return;
    }
    const auto p = params();
    if (m_editing != kNoId) {
        drawPatternEdit(view(), painter,
                        [&](core::Sketch& s) { return core::feature::setCircularPattern(s, m_editing, p); });
        return;
    }
    const std::vector<Id> objects = picked(m_objects);
    const Id center = picked(m_center).front();
    drawPreviewEdit(view(), painter, [&](Editor& ed) { ed.circularPattern(objects, center, p); });
}

std::vector<Handle> CircularPatternTool::handles() const
{
    if (!ready()) {
        return {};
    }
    const core::Point& cp = doc().sketch().point(picked(m_center).front());
    const Vec2 c{cp.x, cp.y};
    const Vec2 from = centreOf(picked(m_objects)) - c;
    const double r = std::max(norm(from), 40.0 / view().zoom());
    const auto p = params();
    const double last = p.angle >= 360.0 - 1e-9 ? 360.0 * (p.count - 1) / p.count : p.angle;
    const double a = std::atan2(from.y, from.x) + last * kDegToRad;
    return {{Handle::Kind::Rotate, 1, c + r * Vec2{std::cos(a), std::sin(a)}, {}}};
}

void CircularPatternTool::dragHandle(int /*id*/, Vec2 world, Vec2 /*pressedAt*/, Qt::KeyboardModifiers modifiers)
{
    const core::Point& cp = doc().sketch().point(picked(m_center).front());
    const Vec2 c{cp.x, cp.y};
    const Vec2 from = centreOf(picked(m_objects)) - c;
    double a = (std::atan2(world.y - c.y, world.x - c.x) - std::atan2(from.y, from.x)) * kRadToDeg;
    while (a <= 0.0) {
        a += 360.0;
    }
    while (a > 360.0) {
        a -= 360.0;
    }
    if (modifiers & Qt::ShiftModifier) {
        a = std::max(15.0, std::round(a / 15.0) * 15.0);
    }
    {
        const QSignalBlocker block(m_type);
        m_type->setCurrentIndex(1);
    }
    setQuietly(m_angle, a);
    changed();
}

QString CircularPatternTool::dragLabel(int /*id*/) const
{
    return QStringLiteral("%1°   × %2").arg(m_angle->value(), 0, 'f', 1).arg(m_count->value());
}

// =============================================================================================
// Offset
// =============================================================================================

void OffsetTool::buildPanel()
{
    m_curves = addPick(QObject::tr("Curves"), "curves", QObject::tr("click a curve to offset"),
                       [this](Id id) { return isCurve(id); }, true);
    const SketchView::ModifyOptions& o = view().modifyOptions();
    m_distance = panel()->addNumber(QObject::tr("Offset position"), "distance", o.offsetDistance, -1e6, 1e6);
    m_sides = panel()->addChoice(QObject::tr("Direction"), "sides", {QObject::tr("One Side"), QObject::tr("Both Sides")},
                                 o.offsetBothSides ? 1 : 0);
    m_caps = panel()->addCheck(QObject::tr("Cap ends"), "caps", o.offsetCaps);
    m_chain = panel()->addCheck(QObject::tr("Chain selection"), "chain", true);
}

bool OffsetTool::bothSides() const
{
    return m_sides->currentIndex() == 1;
}

bool OffsetTool::caps() const
{
    return bothSides() && m_caps->isChecked();
}

void OffsetTool::changed()
{
    if (!m_chain) {
        return;
    }
    panel()->setRowVisible(m_caps, bothSides());
    view().modifyOptions().offsetBothSides = bothSides();
    view().modifyOptions().offsetCaps = m_caps->isChecked();
    std::vector<Id> curves = picked(m_curves);
    if (m_lastPick.empty() && !curves.empty()) {
        m_clickedAt = mouse();  // the side of the first curve picked is the positive side
    }
    if (m_chain->isChecked() && curves.size() > m_lastPick.size()) {
        // A curve was added: bring its whole chain.
        const Id added = curves.back();
        try {
            for (Id c : core::feature::chainAt(doc().sketch(), added)) {
                if (std::ranges::find(curves, c) == curves.end()) {
                    curves.push_back(c);
                }
            }
        }
        catch (const core::SketchError&) {
        }
        if (curves != picked(m_curves)) {
            m_lastPick = curves;
            setPicked(m_curves, curves);  // calls changed() again
            return;
        }
    }
    m_lastPick = curves;
    if (m_distance->value() != 0.0) {
        view().modifyOptions().offsetDistance = std::abs(m_distance->value());
    }
}

bool OffsetTool::frame(Vec2& foot, Vec2& normal) const
{
    const std::vector<Id>& curves = picked(m_curves);
    if (curves.empty()) {
        return false;
    }
    const core::Sketch& s = doc().sketch();
    Id nearest = curves.front();
    double best = 1e300;
    for (Id c : curves) {
        if (const double d = core::distanceTo(s, c, m_clickedAt); d < best) {
            best = d;
            nearest = c;
        }
    }
    foot = core::closestPoint(s, nearest, m_clickedAt);
    // The curve's normal there, turned towards where it was clicked.
    const core::Entity& e = s.entity(nearest);
    Vec2 n{0, 1};
    if (const auto* l = std::get_if<core::Line>(&e.geom)) {
        const core::Point& a = s.point(l->p1);
        const core::Point& b = s.point(l->p2);
        n = {-(b.y - a.y), b.x - a.x};
    }
    else if (e.kind() == core::EntityKind::Circle || e.kind() == core::EntityKind::Arc) {
        const Id centre = e.kind() == core::EntityKind::Circle ? s.circle(nearest).center : s.arc(nearest).center;
        const core::Point& c = s.point(centre);
        n = {foot.x - c.x, foot.y - c.y};
    }
    else {  // (ellipses, splines: square to the curve there)
        const core::curve::Path path = core::curve::pathOf(s, nearest);
        const Vec2 d = path.derivative(path.param(foot));
        n = {-d.y, d.x};
    }
    const double len = norm(n);
    if (len == 0.0) {
        return false;
    }
    normal = (1.0 / len) * n;
    if (dot(m_clickedAt - foot, normal) < 0) {
        normal = -1.0 * normal;
    }
    return true;
}

Vec2 OffsetTool::toward() const
{
    Vec2 foot, normal;
    frame(foot, normal);
    const double side = m_distance->value() < 0 ? -1.0 : 1.0;
    // Just off the curve on the chosen side (well inside any arc it might be).
    return foot + (side * 1e-3 * std::max(1.0, std::abs(m_distance->value()))) * normal;
}

bool OffsetTool::ready() const
{
    return !picked(m_curves).empty() && m_distance->value() != 0.0;
}

bool OffsetTool::commit()
{
    const std::vector<Id> curves = picked(m_curves);
    const double distance = std::abs(m_distance->value());
    const Vec2 side = toward();
    const bool both = bothSides(), capped = caps();
    Id made = kNoId;
    if (!view().tryEdit(QObject::tr("Offset"), [&](Editor& ed) { made = ed.offset(curves, distance, side, both, capped); })) {
        return false;
    }
    // Its distance worked out from parameters: kept so (as typing it into the offset afterwards).
    const QString e = static_cast<ExpressionSpinBox*>(m_distance)->expression();
    if (!e.isEmpty() && made != kNoId) {
        const QString size = m_distance->value() < 0 ? QStringLiteral("-(%1)").arg(e) : e;
        QString error;
        if (!doc().setDimensionExpression(made, size, &error)) {
            emit view().message(error);
        }
    }
    return true;
}

void OffsetTool::paintPreview(QPainter& painter)
{
    if (!ready()) {
        return;
    }
    const std::vector<Id>& curves = picked(m_curves);
    const double distance = std::abs(m_distance->value());
    const Vec2 side = toward();
    const bool both = bothSides(), capped = caps();
    drawPreviewEdit(view(), painter, [&](Editor& ed) { ed.offset(curves, distance, side, both, capped); });
}

std::vector<Handle> OffsetTool::handles() const
{
    Vec2 foot, normal;
    if (!frame(foot, normal)) {
        return {};
    }
    const double d = m_distance->value();
    const double side = d < 0 ? -1.0 : 1.0;
    return {{Handle::Kind::Arrow, 1, foot + d * normal, side * normal}};
}

void OffsetTool::beginHandleDrag(int /*id*/)
{
    m_dragStart = m_distance->value();
}

void OffsetTool::dragHandle(int /*id*/, Vec2 world, Vec2 pressedAt, Qt::KeyboardModifiers /*modifiers*/)
{
    Vec2 foot, normal;
    if (!frame(foot, normal)) {
        return;
    }
    double d = m_dragStart + dot(world - pressedAt, normal);
    if (std::abs(d) < 1e-6) {
        d = m_dragStart < 0 ? -1e-6 : 1e-6;
    }
    setQuietly(m_distance, d);
    changed();
}

QString OffsetTool::dragLabel(int /*id*/) const
{
    return length(m_distance->value());
}

// =============================================================================================
// Divide
// =============================================================================================

void DivideTool::buildPanel()
{
    const core::Sketch& s = doc().sketch();
    SketchView::ModifyOptions& o = view().modifyOptions();
    bool ratio = o.divideByRatio;
    int count = o.divideCount;
    QString ratioText = o.divideRatio;
    bool reversed = false;
    Id line = kNoId;
    if (m_editing != kNoId && s.hasConstraint(m_editing) && s.constraint(m_editing).type == core::ConstraintType::Divide) {
        const core::Constraint& c = s.constraint(m_editing);
        const auto p = core::feature::divideParams(c);
        ratio = p.byRatio;
        reversed = p.reversed;
        line = c.refs.front();
        if (ratio) {
            ratioText = QString::fromStdString(core::feature::ratioText(p.parts));
        }
        else {
            count = static_cast<int>(p.parts.size()) - 1;
        }
    }
    else {
        m_editing = kNoId;
    }

    m_line = addPick(QObject::tr("Line"), "line", QObject::tr("click the line to put points on"),
                     [this](Id id) { return isLine(id); }, false);
    m_type = panel()->addChoice(QObject::tr("Spacing"), "spacing", {QObject::tr("Evenly"), QObject::tr("Ratio")},
                                ratio ? 1 : 0);
    m_count = panel()->addCount(QObject::tr("Points"), "points", count, 1);
    m_ratio = panel()->addText(QObject::tr("Ratio"), "ratio", ratioText, QStringLiteral("3:2:1"));
    m_ratio->setToolTip(QObject::tr("The gaps along the line, from the arrow's end: 3:2:1 makes two points, "
                                    "with the biggest gap first"));
    m_reverse = panel()->addCheck(QObject::tr("Reverse"), "reverse", reversed);
    m_reverse->setToolTip(QObject::tr("Start from the other end (or click the arrow)"));
    if (m_editing != kNoId) {
        setPicked(m_line, {line});
        setPickEnabled(m_line, false);
    }
}

bool DivideTool::byRatio() const
{
    return m_type && m_type->currentIndex() == 1;
}

bool DivideTool::params(core::feature::DivideParams& out, QString* why) const
{
    try {
        if (byRatio()) {
            out.parts = core::feature::parseRatio(m_ratio->text().toStdString());
            out.byRatio = true;
            out.reversed = m_reverse->isChecked();
        }
        else {
            out = core::feature::evenly(m_count->value());
        }
    }
    catch (const core::SketchError& e) {
        if (why) {
            *why = QString::fromStdString(e.what());
        }
        return false;
    }
    return true;
}

bool DivideTool::ready() const
{
    return !picked(m_line).empty();  // (a ratio that doesn't read is reported on OK)
}

void DivideTool::changed()
{
    if (!m_reverse) {
        return;
    }
    panel()->setRowVisible(m_count, !byRatio());
    panel()->setRowVisible(m_ratio, byRatio());
    panel()->setRowVisible(m_reverse, byRatio());
    core::feature::DivideParams p;
    QString why;
    const bool ok = params(p, &why);
    m_ratio->setStyleSheet(ok || !byRatio() ? QString() : QStringLiteral("color: #ff5555;"));
    m_ratio->setToolTip(ok || !byRatio() ? QObject::tr("The gaps along the line, from the arrow's end: 3:2:1 makes "
                                                       "two points, with the biggest gap first")
                                         : why);
    if (m_editing == kNoId) {
        SketchView::ModifyOptions& o = view().modifyOptions();
        o.divideByRatio = byRatio();
        o.divideCount = m_count->value();
        o.divideRatio = m_ratio->text();
    }
}

bool DivideTool::commit()
{
    core::feature::DivideParams p;
    QString why;
    if (!params(p, &why)) {
        emit view().message(why);
        return false;
    }
    const Id line = picked(m_line).front();
    return view().tryEdit(QObject::tr("Divide"), [&](Editor& ed) {
        if (m_editing != kNoId) {
            ed.setDivide(m_editing, p);
        }
        else {
            ed.divide(line, p);
        }
    });
}

void DivideTool::paintPreview(QPainter& painter)
{
    core::feature::DivideParams p;
    if (picked(m_line).empty() || !params(p)) {
        return;
    }
    // The points as they'd be, on a scratch copy: new ones, or the edited divide's moved ones.
    core::Sketch scratch = doc().sketch();
    std::set<Id> shown;
    try {
        const Id k = m_editing != kNoId ? core::feature::setDivide(scratch, m_editing, p)
                                        : core::feature::divide(scratch, picked(m_line).front(), p);
        const core::Constraint& c = scratch.constraint(k);
        shown.insert(c.refs.begin() + 1, c.refs.end());
    }
    catch (const core::SketchError&) {
        return;
    }
    view().drawPreviewSketch(painter, scratch, [&](Id id) { return shown.contains(id); });
    // Rings round them, so they show on the (highlighted) line.
    painter.save();
    painter.setRenderHint(QPainter::Antialiasing);
    painter.setPen(QPen(theme::canvas().hover, 1.5));
    painter.setBrush(Qt::NoBrush);
    for (Id point : shown) {
        painter.drawEllipse(view().toScreen({scratch.point(point).x, scratch.point(point).y}), 5.0, 5.0);
    }
    painter.restore();
}

std::vector<Handle> DivideTool::handles() const
{
    // A ratio's start: an arrow beside the line at the end it's measured from, pointing along it.
    if (picked(m_line).empty() || !byRatio()) {
        return {};
    }
    const core::Sketch& s = doc().sketch();
    const core::Line& l = s.line(picked(m_line).front());
    Vec2 a{s.point(l.p1).x, s.point(l.p1).y}, b{s.point(l.p2).x, s.point(l.p2).y};
    if (m_reverse->isChecked()) {
        std::swap(a, b);
    }
    const double len = norm(b - a);
    if (len <= 0.0) {
        return {};
    }
    const Vec2 u = (1.0 / len) * (b - a);
    const Vec2 side{-u.y, u.x};
    const double gap = kHandleGap / view().zoom();
    return {{Handle::Kind::Arrow, 1, a + gap * side, u}};
}

void DivideTool::beginHandleDrag(int /*id*/)
{
    m_reverse->toggle();  // clicking the arrow turns it round
}

// =============================================================================================
// Paste
// =============================================================================================

QString PasteTool::hint() const
{
    return QObject::tr("Paste: click to place (Esc when done)");
}

void PasteTool::move(const ToolEvent& e)
{
    m_cursor = view().snap(e.screen);
    refresh();
}

void PasteTool::press(const ToolEvent& e)
{
    if (e.button != Qt::LeftButton) {
        return;
    }
    const Vec2 at = view().snap(e.screen).pos;
    std::map<Id, Id> made;
    if (view().tryEdit(QObject::tr("Paste"), [&](Editor& ed) { made = ed.paste(m_fragment, at); })) {
        std::set<Id> pasted;
        for (const auto& [from, to] : made) {
            if (m_fragment.hasEntity(from)) {
                pasted.insert(to);
            }
        }
        view().setSelection(std::move(pasted));
    }
    refresh();
}

void PasteTool::paintOverlay(QPainter& painter)
{
    drawGhost(view(), painter, [&](core::Sketch& s) { core::xform::paste(s, m_fragment, m_cursor.pos); });
    view().drawSnapMarker(painter, m_cursor);
}
