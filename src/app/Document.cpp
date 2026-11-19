#include "Document.h"

#include "TextShapes.h"

#include <core/Annotations.h>
#include <core/Features.h>
#include <core/Groups.h>
#include <core/Regions.h>
#include <core/Serialize.h>

#include <QDir>

#include <algorithm>
#include <cmath>
#include <map>
#include <set>
#include <QFile>
#include <QFontDatabase>
#include <QImageWriter>
#include <QFileInfo>
#include <QPageLayout>
#include <QPainter>
#include <QPdfWriter>
#include <QSaveFile>
#include <QSvgRenderer>

namespace
{
constexpr size_t kMaxUndo = 200;

// A Python string literal for a file path.
QString pyString(const QString& s)
{
    QString escaped = QDir::fromNativeSeparators(s);
    escaped.replace(QLatin1Char('\\'), QStringLiteral("\\\\")).replace(QLatin1Char('"'), QStringLiteral("\\\""));
    return QLatin1Char('"') + escaped + QLatin1Char('"');
}

QString errorText(const core::SketchError& e)
{
    return QString::fromStdString(e.what());
}
}  // namespace

Document::Document(QObject* parent)
    : QObject(parent)
{
    m_sketch.ensureOrigin();
    resolve();
}

Document::~Document() = default;

// ---------------------------------------------------------------------------------------------
// Edits
// ---------------------------------------------------------------------------------------------

void Document::modify(const QString& /*label*/, const std::function<void(core::Sketch&)>& fn)
{
    core::Sketch working = m_sketch;
    fn(working);  // may throw; m_sketch is untouched in that case
    const core::Sketch before = m_sketch;
    commit(std::move(working));
    resolve(&before);
    emit changed();
}

void Document::edit(const QString& /*label*/, const std::function<void(Editor&)>& fn)
{
    core::Sketch working = m_sketch;
    QStringList python;
    Editor editor(working, &python);
    try {
        fn(editor);  // may throw; m_sketch is untouched in that case
    }
    catch (...) {
        m_nextMergeKey.clear();  // (it was for this edit)
        throw;
    }
    const core::Sketch before = m_sketch;
    commit(std::move(working));
    resolve(&before);
    emit changed();
    record(python.join(QLatin1Char('\n')));
}

core::Id Document::addConstraint(core::ConstraintType type, std::vector<core::Id> refs, double value,
                                 QString* error, double labelX, double labelY, std::uint8_t flip)
{
    core::Sketch working = m_sketch;
    core::Id id = core::kNoId;
    const std::vector<core::Id> asked = refs;
    try {
        id = working.addConstraint(type, std::move(refs), value);
        if (labelX != 0.0 || labelY != 0.0) {
            working.setConstraintLabel(id, labelX, labelY);
        }
        if (flip != 0) {
            working.setConstraintFlip(id, flip);
        }
    }
    catch (const core::SketchError& e) {
        if (error) {
            *error = errorText(e);
        }
        return core::kNoId;
    }
    if (!commitChecked(std::move(working), error)) {
        if (type == core::ConstraintType::Midpoint) {
            return midpointOnLine(asked, error);
        }
        if (type == core::ConstraintType::Collinear) {
            return collinearByPoint(asked, error);
        }
        return core::kNoId;
    }
    record(Editor::constraintCall(m_sketch, id));
    return id;
}

core::Id Document::collinearByPoint(const std::vector<core::Id>& refs, QString* error)
{
    // Two lines already kept parallel (both horizontal, say: a rectangle's side and a text box's)
    // would have that said twice by collinear. Then an end of one kept on the other is enough to
    // put them in line; check they do end up so.
    if (refs.size() != 2 || m_sketch.entity(refs[0]).kind() != core::EntityKind::Line
        || m_sketch.entity(refs[1]).kind() != core::EntityKind::Line) {
        return core::kNoId;
    }
    for (int k = 0; k < 4; ++k) {
        const core::Id line = refs[k / 2], other = refs[1 - k / 2];
        const core::Id end = k % 2 == 0 ? m_sketch.line(line).p1 : m_sketch.line(line).p2;
        core::Sketch probe = m_sketch;
        core::Id id = core::kNoId;
        try {
            id = probe.addConstraint(core::ConstraintType::Coincident, {end, other});
        }
        catch (const core::SketchError&) {
            continue;
        }
        core::Sketch solved = probe;
        if (!core::solve(solved).ok) {
            continue;
        }
        auto off = [&](core::Id p) {  // from the other line, carried on
            const core::Line& l = solved.line(other);
            const core::Point& a = solved.point(l.p1);
            const core::Point& b = solved.point(l.p2);
            const core::Point& q = solved.point(p);
            const double len = std::max(std::hypot(b.x - a.x, b.y - a.y), 1e-12);
            return std::abs((b.x - a.x) * (q.y - a.y) - (b.y - a.y) * (q.x - a.x)) / len;
        };
        const core::Line& mine = solved.line(line);
        if (off(mine.p1) > 1e-6 || off(mine.p2) > 1e-6) {
            continue;  // not in line after all
        }
        QString unused;
        if (!commitChecked(std::move(probe), &unused)) {
            continue;
        }
        if (error) {
            error->clear();
        }
        record(Editor::constraintCall(m_sketch, id));
        return id;
    }
    return core::kNoId;  // (the original refusal stands)
}

core::Id Document::midpointOnLine(const std::vector<core::Id>& refs, QString* error)
{
    // Other constraints may already put the point halfway along the line (a symmetric shape,
    // say), so a midpoint would say that twice. Then keeping the point on the line is enough;
    // check it does end up at the middle.
    core::Sketch probe = m_sketch;
    core::Id id = core::kNoId;
    try {
        std::vector<core::Id> ordered = refs;
        if (probe.entity(ordered[0]).kind() != core::EntityKind::Point) {
            std::swap(ordered[0], ordered[1]);
        }
        id = probe.addConstraint(core::ConstraintType::Coincident, ordered);
    }
    catch (const core::SketchError&) {
        return core::kNoId;
    }
    core::Sketch solved = probe;
    if (!core::solve(solved).ok) {
        return core::kNoId;
    }
    const core::Constraint& c = solved.constraint(id);
    const core::Line& l = solved.line(c.refs[1]);
    const core::Point& p = solved.point(c.refs[0]);
    const core::Point& a = solved.point(l.p1);
    const core::Point& b = solved.point(l.p2);
    const double len = std::hypot(b.x - a.x, b.y - a.y);
    if (std::hypot(p.x - (a.x + b.x) / 2, p.y - (a.y + b.y) / 2) > 1e-6 * std::max(1.0, len)) {
        return core::kNoId;  // not at the middle: the original refusal stands
    }
    QString unused;
    if (!commitChecked(std::move(probe), &unused)) {
        return core::kNoId;
    }
    if (error) {
        error->clear();
    }
    record(Editor::constraintCall(m_sketch, id));
    return id;
}

core::Id Document::addGroup(const std::vector<core::Id>& items, QString* error)
{
    // (named as they are now: a group among them is taken into the new one)
    QStringList names;
    for (core::Id item : items) {
        const bool group = m_sketch.hasConstraint(item) && m_sketch.constraint(item).type == core::ConstraintType::Group;
        if ((m_sketch.hasEntity(item) && !m_sketch.isReference(item)) || group) {
            names << Editor::name(m_sketch, item);
        }
    }
    core::Sketch working = m_sketch;
    core::Id id = core::kNoId;
    try {
        id = core::group::create(working, items);
    }
    catch (const core::SketchError& e) {
        if (error) {
            *error = errorText(e);
        }
        return core::kNoId;
    }
    if (!commitChecked(std::move(working), error, {}, {id})) {
        return core::kNoId;
    }
    record(QStringLiteral("%1 = s.group([%2])").arg(Editor::name(m_sketch, id), names.join(QStringLiteral(", "))));
    return id;
}

bool Document::setDimensionValue(core::Id dimension, double value, QString* error)
{
    if (core::group::holder(m_sketch, dimension) != core::kNoId) {
        if (error) {
            *error = tr("It's inside a group, which holds its size: ungroup it (delete the group) to change it");
        }
        return false;
    }
    core::Sketch working = m_sketch;
    std::vector<core::Id> hold;
    try {
        working.setConstraintValue(dimension, value);
        const core::Constraint& c = working.constraint(dimension);
        if (core::isFeature(c.type)) {
            hold = core::feature::sourcePoints(working, c);
            if (c.type != core::ConstraintType::Offset) {
                core::feature::placeCopies(working, dimension);
            }
        }
    }
    catch (const core::SketchError& e) {
        if (error) {
            *error = errorText(e);
        }
        return false;
    }
    if (!commitChecked(std::move(working), error, hold, {dimension})) {
        return false;
    }
    record(QStringLiteral("%1.value = %2").arg(Editor::name(m_sketch, dimension), Editor::number(value)));
    return true;
}

namespace
{
}  // namespace

std::vector<core::Id> Document::blockersOf(const core::Sketch& s, const std::vector<core::Id>& constraints)
{
    // The constraints, then the fixed geometry linked to them (through shared points and other
    // constraints): what holds things still when a change can't be made.
    std::vector<core::Id> out;
    auto add = [&](core::Id id) {
        if (std::ranges::find(out, id) == out.end()) {
            out.push_back(id);
        }
    };
    for (core::Id k : constraints) {
        if (s.hasConstraint(k)) {
            add(k);
        }
    }
    // Which entities each point belongs to, and which constraints reference each entity.
    std::map<core::Id, std::vector<core::Id>> owners, usedBy;
    for (const auto& [id, e] : s.entities()) {
        for (core::Id p : s.definingPoints(id)) {
            owners[p].push_back(id);
        }
    }
    for (const auto& [id, c] : s.constraints()) {
        for (core::Id r : c.refs) {
            usedBy[r].push_back(id);
        }
    }
    std::set<core::Id> seen;
    std::vector<core::Id> todo;
    for (core::Id k : constraints) {
        if (s.hasConstraint(k)) {
            for (core::Id r : s.constraint(k).refs) {
                todo.push_back(r);
            }
        }
    }
    while (!todo.empty()) {
        const core::Id id = todo.back();
        todo.pop_back();
        if (!s.hasEntity(id) || s.isReference(id) || !seen.insert(id).second) {
            continue;
        }
        if (s.entity(id).fixed) {
            add(id);
        }
        for (core::Id p : s.definingPoints(id)) {
            for (core::Id o : owners[p]) {
                todo.push_back(o);
            }
        }
        for (core::Id k : usedBy[id]) {
            for (core::Id r : s.constraint(k).refs) {
                todo.push_back(r);
            }
        }
    }
    return out;
}
QString Document::describeItem(const core::Sketch& s, core::Id id)
{
    if (s.hasConstraint(id)) {
        return QStringLiteral("%1 %2").arg(QString::fromLatin1(core::toString(s.constraint(id).type)),
                                           Editor::name(s, id));
    }
    if (s.hasEntity(id)) {
        return tr("fixed %1").arg(Editor::name(s, id));
    }
    return Editor::name(s, id);
}

void Document::setActiveSublayer(core::Id sublayer)
{
    if (sublayer == m_sketch.activeSublayer() || !m_sketch.hasSublayer(sublayer)) {
        return;
    }
    m_sketch.setActiveSublayer(sublayer);
    emit changed();
    announce(QStringLiteral("s.sublayer = %1").arg(Editor::sublayerName(m_sketch, sublayer)));
}

void Document::setSublayerVisible(core::Id sublayer, bool visible)
{
    if (!m_sketch.hasSublayer(sublayer) || m_sketch.sublayer(sublayer).visible == visible) {
        return;
    }
    m_sketch.setSublayerVisible(sublayer, visible);
    emit changed();
    announce(QStringLiteral("s.show_sublayer(%1, %2)")
                 .arg(Editor::sublayerName(m_sketch, sublayer), visible ? QStringLiteral("True") : QStringLiteral("False")));
}

void Document::followProjections()
{
    // Projections in other layers follow what's being dragged (their layers solved again), and
    // offsets of ellipses and splines follow their sources.
    const bool projected = m_sketch.syncProjections();
    const bool placed = core::feature::placeOffsetPoints(m_sketch);
    if (projected || placed) {
        core::solve(m_sketch);
    }
}

void Document::setActiveLayer(core::Id layer)
{
    if (layer == m_sketch.activeLayer() || !m_sketch.hasLayer(layer)) {
        return;
    }
    m_sketch.setActiveLayer(layer);
    emit changed();
    announce(QStringLiteral("s.layer = %1").arg(Editor::layerName(m_sketch, layer)));
}

void Document::setLayerVisible(core::Id layer, bool visible)
{
    if (!m_sketch.hasLayer(layer) || m_sketch.layer(layer).visible == visible) {
        return;
    }
    m_sketch.setLayerVisible(layer, visible);
    emit changed();
    announce(QStringLiteral("s.show_layer(%1, %2)")
                 .arg(Editor::layerName(m_sketch, layer), visible ? QStringLiteral("True") : QStringLiteral("False")));
}

bool Document::commitChecked(core::Sketch working, QString* error, const std::vector<core::Id>& hold,
                             const std::vector<core::Id>& edited)
{
    // Writes back on success.
    core::SolveResult result = hold.empty() ? core::solve(working) : core::solveHolding(working, hold);
    QString problem;
    std::vector<core::Id> blamed;  // the constraints in the way
    auto newOnes = [](const std::vector<core::Id>& now, const std::vector<core::Id>& before) {
        std::vector<core::Id> out;
        for (core::Id id : now) {
            if (std::ranges::find(before, id) == before.end()) {
                out.push_back(id);
            }
        }
        return out;
    };
    // Judged by the layer being edited: other layers are solved apart, and what's amiss there
    // (or how their solver counts it) isn't this edit's doing.
    const core::Id layer = working.activeLayer();
    auto mine = [layer](const core::Sketch& s, const std::vector<core::Id>& ids) {
        std::vector<core::Id> out;
        for (core::Id id : ids) {
            if (s.layerOf(id) == layer) {
                out.push_back(id);
            }
        }
        return out;
    };
    const std::vector<core::Id> conflictingNow = mine(working, result.conflicting);
    const std::vector<core::Id> conflictingBefore = mine(m_sketch, m_result.conflicting);
    const std::vector<core::Id> redundantNow = mine(working, result.redundant);
    const std::vector<core::Id> redundantBefore = mine(m_sketch, m_result.redundant);
    if (conflictingNow.size() > conflictingBefore.size()) {
        problem = tr("That would conflict with existing constraints");
        blamed = newOnes(conflictingNow, conflictingBefore);
    }
    else if (redundantNow.size() > redundantBefore.size()) {
        problem = tr("That would over-constrain the sketch");
        blamed = newOnes(redundantNow, redundantBefore);
    }
    else if (result.failedLayers.contains(layer)) {
        problem = tr("The sketch can't be solved like that");
        blamed = edited;  // (nothing else to go on: what holds it still is linked to it)
    }
    if (!problem.isEmpty()) {
        const std::vector<core::Id> blockers = blockersOf(working, blamed);
        if (!blockers.empty()) {
            QStringList names;
            for (core::Id id : blockers) {
                names << describeItem(working, id);
            }
            problem += tr(": %1").arg(names.join(QStringLiteral(", ")));
        }
        if (error) {
            *error = problem;
        }
        emit blocked(blockers);
        return false;
    }
    textshapes::refreshPathTexts(working);
    if (textshapes::fitBoxes(working)) {
        result = core::solve(working);
        textshapes::refreshPathTexts(working);
    }
    core::regions::carryFills(working, m_sketch);  // (a filled shape moved: its fill with it)
    core::regions::refreshFills(working);
    commit(std::move(working));
    m_result = std::move(result);
    emit changed();
    return true;
}

// ---------------------------------------------------------------------------------------------
// Dimension labels
// ---------------------------------------------------------------------------------------------

void Document::beginLabelDrag()
{
    m_beforeDrag = m_sketch;
    m_labelDragging = true;
    m_dragMoved = false;
    m_labelDimension = core::kNoId;
}

void Document::moveLabel(core::Id dimension, double dx, double dy)
{
    if (m_sketch.hasDrawnDimension(dimension)) {
        core::DrawnDimension d = m_sketch.drawnDimension(dimension);
        d.labelX = dx;
        d.labelY = dy;
        m_sketch.setDrawnDimension(d);
    }
    else if (m_sketch.hasConstraint(dimension)) {
        m_sketch.setConstraintLabel(dimension, dx, dy);
    }
    else {
        return;
    }
    m_dragMoved = true;
    m_labelDimension = dimension;
    emit changed();
}

void Document::endLabelDrag()
{
    if (m_labelDragging && m_dragMoved) {
        core::Sketch before = std::move(m_beforeDrag);
        core::Sketch now = std::move(m_sketch);
        m_sketch = std::move(before);
        commit(std::move(now));
        if (m_sketch.hasConstraint(m_labelDimension)) {
            const core::Constraint& c = m_sketch.constraint(m_labelDimension);
            record(QStringLiteral("%1.label = (%2, %3)")
                       .arg(Editor::name(m_sketch, c.id), Editor::number(c.labelX), Editor::number(c.labelY)));
        }
        else if (m_sketch.hasDrawnDimension(m_labelDimension)) {
            const core::annotation::Graphics g =
                core::annotation::graphics(m_sketch, m_sketch.drawnDimension(m_labelDimension));
            record(QStringLiteral("s.edit_annotation(%1, at=%2)")
                       .arg(Editor::name(m_sketch, m_labelDimension), Editor::pair(g.label)));
        }
    }
    m_beforeDrag = {};
    m_labelDragging = false;
}

// ---------------------------------------------------------------------------------------------
// Dragging geometry
// ---------------------------------------------------------------------------------------------

void Document::beginDrag(const std::vector<core::Id>& points)
{
    m_beforeDrag = m_sketch;
    m_dragMoved = false;
    m_dragPoints = points;
    m_dragEdge = core::kNoId;
    m_dragDx = m_dragDy = 0.0;
    m_dragIsMove = false;
    m_dragAngle = 0.0;
    m_dragAbout = {};
    m_dragSolver = std::make_unique<core::Solver>(m_sketch, m_sketch.activeLayer());
    m_dragSolver->beginDrag(points);
}

void Document::beginDragEdge(core::Id roundEntity, double x, double y)
{
    m_beforeDrag = m_sketch;
    m_dragMoved = false;
    m_dragPoints.clear();
    m_dragEdge = roundEntity;
    m_dragGrabX = x;
    m_dragGrabY = y;
    m_dragDx = m_dragDy = 0.0;
    m_dragIsMove = false;
    m_dragSolver = std::make_unique<core::Solver>(m_sketch, m_sketch.activeLayer());
    m_dragSolver->beginDragEdge(roundEntity, x, y);
}

void Document::dragTo(double dx, double dy)
{
    if (!m_dragSolver) {
        return;
    }
    if (m_dragSolver->dragTo(dx, dy)) {
        m_dragSolver->writeBack(m_sketch);
        core::regions::carryFills(m_sketch, m_beforeDrag);  // (a filled shape: its fill along, as it goes)
        followProjections();
        textshapes::refreshPathTexts(m_sketch);
        m_dragMoved = true;
        m_dragDx = dx;
        m_dragDy = dy;
        emit changed();
    }
}

void Document::dragMove(double dx, double dy, double angle, core::Vec2 about)
{
    if (!m_dragSolver) {
        return;
    }
    const core::Affine t = Editor::moveMap(dx, dy, angle, about);
    if (m_dragSolver->dragMap([&](core::Vec2 p) { return t.apply(p); })) {
        m_dragSolver->writeBack(m_sketch);
        core::regions::carryFills(m_sketch, m_beforeDrag);
        followProjections();
        textshapes::refreshPathTexts(m_sketch);
        m_dragMoved = true;
        m_dragIsMove = true;
        m_dragDx = dx;
        m_dragDy = dy;
        m_dragAngle = angle;
        m_dragAbout = about;
        emit changed();
    }
}

void Document::endDrag()
{
    if (!m_dragSolver) {
        return;
    }
    m_dragSolver.reset();
    if (m_dragMoved) {
        core::Sketch now = std::move(m_sketch);
        m_sketch = std::move(m_beforeDrag);
        const core::Sketch& before = m_sketch;  // name things as they were when the drag began
        QString python;
        if (m_dragEdge != core::kNoId) {
            python = QStringLiteral("s.drag_edge(%1, (%2, %3), %4, %5)")
                         .arg(Editor::name(before, m_dragEdge), Editor::number(m_dragGrabX),
                              Editor::number(m_dragGrabY), Editor::number(m_dragDx), Editor::number(m_dragDy));
        }
        else if (m_dragIsMove) {
            python = QStringLiteral("s.move(%1%2)")
                         .arg(Editor::list(before, m_dragPoints),
                              Editor::moveArgs(m_dragDx, m_dragDy, m_dragAngle, m_dragAbout));
        }
        else {
            python = QStringLiteral("s.drag(%1, %2, %3)")
                         .arg(Editor::list(before, m_dragPoints), Editor::number(m_dragDx), Editor::number(m_dragDy));
        }
        commit(std::move(now));
        record(python);
    }
    m_beforeDrag = {};
    resolve();
    emit changed();
}

void Document::cancelDrag()
{
    if (!m_dragSolver) {
        return;
    }
    m_dragSolver.reset();
    m_sketch = std::move(m_beforeDrag);
    m_beforeDrag = {};
    resolve();
    emit changed();
}

// ---------------------------------------------------------------------------------------------
// History
// ---------------------------------------------------------------------------------------------

void Document::undo()
{
    if (m_undo.empty() || isDragging() || inTransaction()) {
        return;
    }
    m_redo.push_back({std::move(m_sketch), m_revision, std::move(m_history)});
    m_sketch = std::move(m_undo.back().sketch);
    m_revision = m_undo.back().revision;
    m_history = std::move(m_undo.back().history);  // the undone commands come off the history
    m_undo.pop_back();
    resolve();
    emit changed();
    emit undoStateChanged();
    emit fileStateChanged();
    announce(QStringLiteral("doc.undo()"));
}

void Document::redo()
{
    if (m_redo.empty() || isDragging() || inTransaction()) {
        return;
    }
    m_undo.push_back({std::move(m_sketch), m_revision, std::move(m_history)});
    m_sketch = std::move(m_redo.back().sketch);
    m_revision = m_redo.back().revision;
    m_history = std::move(m_redo.back().history);
    m_redo.pop_back();
    resolve();
    emit changed();
    emit undoStateChanged();
    emit fileStateChanged();
    announce(QStringLiteral("doc.redo()"));
}

void Document::beginTransaction()
{
    if (m_transactionDepth++ == 0) {
        m_beforeTransaction = m_sketch;
        m_historyBeforeTransaction = m_history;
        m_revisionBeforeTransaction = m_revision;
        m_transactionChanged = false;
    }
}

void Document::endTransaction()
{
    if (m_transactionDepth == 0 || --m_transactionDepth > 0) {
        return;
    }
    if (m_transactionChanged) {
        m_revision = m_revisionBeforeTransaction;  // undo returns to the pre-transaction state
        pushUndo(std::move(m_beforeTransaction), std::move(m_historyBeforeTransaction));  // one step
    }
    m_beforeTransaction = {};
    m_historyBeforeTransaction = {};
    m_transactionChanged = false;
}

void Document::commit(core::Sketch next)
{
    core::Sketch before = std::move(m_sketch);
    m_sketch = std::move(next);
    if (inTransaction()) {
        m_transactionChanged = true;
        return;
    }
    const QString key = std::exchange(m_nextMergeKey, QString());
    if (!key.isEmpty() && key == m_lastMergeKey && m_revision == m_lastMergeRevision && !m_undo.empty()) {
        // The same edit again, straight after (typing on in a field): one step, one command. The
        // step before it stays what undo goes back to; its command replaces the last one's.
        m_history = m_undo.back().history;
        m_redo.clear();
        m_revision = m_nextRevision++;
        emit fileStateChanged();
    }
    else {
        pushUndo(std::move(before), m_history);  // (the edit's own commands are recorded after this)
    }
    m_lastMergeKey = key;
    m_lastMergeRevision = m_revision;
}

void Document::pushUndo(core::Sketch snapshot, QStringList history)
{
    m_undo.push_back({std::move(snapshot), m_revision, std::move(history)});
    if (m_undo.size() > kMaxUndo) {
        m_undo.erase(m_undo.begin());
    }
    m_redo.clear();
    m_revision = m_nextRevision++;  // pushUndo always precedes a new current state
    emit undoStateChanged();
    emit fileStateChanged();
}

void Document::reset(core::Sketch sketch)
{
    m_dragSolver.reset();
    m_sketch = std::move(sketch);
    m_sketch.ensureOrigin();
    m_history.clear();
    m_undo.clear();
    m_redo.clear();
    m_revision = m_savedRevision = m_nextRevision++;
    if (inTransaction()) {
        // A script opened a file / started over: the transaction continues from the new sketch.
        m_beforeTransaction = m_sketch;
        m_historyBeforeTransaction = m_history;
        m_revisionBeforeTransaction = m_revision;
        m_transactionChanged = false;
    }
    resolve();
    emit changed();
    emit undoStateChanged();
    emit fileStateChanged();
}

void Document::resolve(const core::Sketch* before)
{
    m_result = core::solve(m_sketch);
    textshapes::refreshPathTexts(m_sketch);  // text along a curve follows it
    if (textshapes::fitBoxes(m_sketch)) {    // boxes that grow with their text (a new width: new lines)
        m_result = core::solve(m_sketch);
        textshapes::refreshPathTexts(m_sketch);
    }
    if (before) {
        core::regions::carryFills(m_sketch, *before);  // a filled shape moved: its fill with it
    }
    core::regions::refreshFills(m_sketch);   // fills keep track of their areas as edits go on
}

void Document::record(const QString& python)
{
    if (python.isEmpty()) {
        return;
    }
    // Calls made from Python are kept too (in the same form): it's what built the sketch.
    m_history << python;
    if (m_silence == 0) {
        emit recorded(python);
    }
}

void Document::announce(const QString& python)
{
    if (m_silence == 0 && !python.isEmpty()) {
        emit recorded(python);
    }
}
// ---------------------------------------------------------------------------------------------
// Files
// ---------------------------------------------------------------------------------------------

bool Document::save(const QString& path, QString* error)
{
    QSaveFile file(path);  // writes to a temp file and renames, so a failed save can't truncate
    std::vector<std::string> history;
    for (const QString& entry : m_history) {
        history.push_back(entry.toStdString());
    }
    const std::string json = core::toJson(m_sketch, false, &history);
    if (!file.open(QIODevice::WriteOnly) || file.write(json.data(), qint64(json.size())) != qint64(json.size())
        || !file.commit()) {
        if (error) {
            *error = tr("Couldn't save %1: %2").arg(path, file.errorString());
        }
        return false;
    }
    m_filePath = path;
    m_savedRevision = m_revision;
    emit fileStateChanged();
    announce(QStringLiteral("doc.save(%1)").arg(pyString(path)));
    return true;
}

bool Document::open(const QString& path, QString* error)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        if (error) {
            *error = tr("Couldn't open %1: %2").arg(path, file.errorString());
        }
        return false;
    }
    core::Sketch loaded;
    std::vector<std::string> history;
    try {
        loaded = core::fromJson(file.readAll().toStdString(), &history);
    }
    catch (const core::SketchError& e) {
        if (error) {
            *error = tr("Couldn't read %1: %2").arg(path, errorText(e));
        }
        return false;
    }
    // The letters' outlines are kept in the file for where the font isn't installed; where it is,
    // they're made afresh.
    std::vector<core::Text> remade;
    for (const auto& [id, t] : loaded.texts()) {
        if (QFontDatabase::hasFamily(QString::fromStdString(t.font))) {
            remade.push_back(t);
        }
    }
    for (core::Text& t : remade) {
        textshapes::update(t, &loaded);
        loaded.setText(t);
    }
    reset(std::move(loaded));
    for (const std::string& entry : history) {
        m_history << QString::fromStdString(entry);
    }
    if (inTransaction()) {
        m_historyBeforeTransaction = m_history;
    }
    m_filePath = path;
    emit fileStateChanged();
    announce(QStringLiteral("doc.open(%1)").arg(pyString(path)));
    return true;
}

bool Document::writeExport(const QString& path, const std::function<std::string()>& make, QString* error)
{
    std::string text;
    try {
        text = make();
    }
    catch (const core::SketchError& e) {
        if (error) {
            *error = tr("Couldn't export: %1").arg(errorText(e));
        }
        return false;
    }
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly) || file.write(text.data(), qint64(text.size())) != qint64(text.size())
        || !file.commit()) {
        if (error) {
            *error = tr("Couldn't write %1: %2").arg(path, file.errorString());
        }
        return false;
    }
    return true;
}

namespace
{
QString pyBool(bool b)
{
    return b ? QStringLiteral("True") : QStringLiteral("False");
}
}  // namespace

bool Document::exportSvg(const QString& path, const core::exporter::SvgOptions& o, QString* error)
{
    if (!writeExport(path, [&] { return core::exporter::toSvg(m_sketch, o); }, error)) {
        return false;
    }
    announce(QStringLiteral("doc.export_svg(%1, fills=%2, undecorated=%3, construction=%4, margin=%5%6%7%8)")
                 .arg(pyString(path), pyBool(o.fills), pyBool(o.undecorated), pyBool(o.construction),
                      Editor::number(o.margin), o.textAsOutlines ? QStringLiteral(", text_as_outlines=True") : QString(),
                      o.hatchLines ? QStringLiteral(", hatch_lines=True") : QString(),
                      o.dimensions ? QString() : QStringLiteral(", dimensions=False")));
    return true;
}

bool Document::exportDxf(const QString& path, const core::exporter::DxfOptions& o, QString* error)
{
    if (!writeExport(path, [&] { return core::exporter::toDxf(m_sketch, o); }, error)) {
        return false;
    }
    announce(QStringLiteral("doc.export_dxf(%1, styles=%2, construction=%3%4%5%6)")
                 .arg(pyString(path), pyBool(o.styles), pyBool(o.construction),
                      o.fills ? QString() : QStringLiteral(", fills=False"),
                      o.textAsOutlines ? QStringLiteral(", text_as_outlines=True") : QString(),
                      o.dimensions ? QString() : QStringLiteral(", dimensions=False")));
    return true;
}

namespace
{
// The SVG export in pieces, its text cut out to be drawn in its font (see SvgPieces), and its size
// in mm. Hatches as lines: Qt's SVG renderer doesn't draw <pattern>s.
bool exportPieces(const core::Sketch& sketch, const core::exporter::SvgOptions& options,
                  core::exporter::SvgPieces& pieces, QSizeF& mm, QString* error)
{
    core::exporter::SvgOptions o = options;
    o.hatchLines = true;
    o.canvas.reset();
    try {
        pieces = core::exporter::toSvgPieces(sketch, o);
    }
    catch (const core::SketchError& e) {
        if (error) {
            *error = Document::tr("Couldn't export: %1").arg(errorText(e));
        }
        return false;
    }
    mm = QSvgRenderer(QByteArray::fromStdString(pieces.svgs.front())).viewBoxF().size();
    if (mm.isEmpty()) {
        if (error) {
            *error = Document::tr("Couldn't export: there's nothing to put on the page");
        }
        return false;
    }
    return true;
}

// Draws the pieces, each followed by its texts in their fonts, `perMm` device pixels to the mm
// (from the painter's top left).
void paintPieces(QPainter& painter, const core::exporter::SvgPieces& pieces, const QSizeF& mm, double perMm)
{
    painter.setRenderHint(QPainter::Antialiasing);
    painter.setRenderHint(QPainter::TextAntialiasing);
    const QRectF pageRect(0, 0, mm.width() * perMm, mm.height() * perMm);
    for (size_t i = 0; i < pieces.svgs.size(); ++i) {
        QSvgRenderer(QByteArray::fromStdString(pieces.svgs[i])).render(&painter, pageRect);
        for (const core::exporter::PlacedTextRun& r : pieces.texts[i]) {
            if (r.size <= 0.0 || r.text.empty()) {
                continue;
            }
            // At a 100 px font, scaled down to its size (fonts take whole pixel sizes).
            painter.save();
            painter.scale(perMm, perMm);
            painter.translate(r.x, r.y);
            painter.rotate(r.rotation);
            painter.scale(r.size / 100.0, r.size / 100.0);
            QFont font(QString::fromStdString(r.font));
            font.setPixelSize(100);
            font.setBold(r.bold);
            font.setItalic(r.italic);
            painter.setFont(font);
            painter.setPen(QColor(r.color.r, r.color.g, r.color.b, r.color.a));
            const QString text = QString::fromStdString(r.text);
            const QFontMetricsF metrics(font, painter.device());
            const double extra = r.tracking * 100.0 / r.size;
            // Where it starts: its left end at (x, y), or its middle, or its right end.
            double x = 0.0;
            if (r.align != 0) {
                const QList<uint> letters = text.toUcs4();
                const double width = metrics.horizontalAdvance(text) + extra * std::max<qsizetype>(letters.size() - 1, 0);
                x = -width * r.align / 2.0;
            }
            if (std::abs(r.tracking) < 1e-9) {
                painter.drawText(QPointF(x, 0), text);
            }
            else {
                // Letter by letter, each moved on by its advance and the tracking (QFont's own
                // letter spacing goes by the device's pixels, not the font's).
                for (char32_t c : text.toUcs4()) {
                    const QString letter = QString::fromUcs4(&c, 1);
                    painter.drawText(QPointF(x, 0), letter);
                    x += metrics.horizontalAdvance(letter) + extra;
                }
            }
            painter.restore();
        }
    }
}

QSize pixelsFor(const QSizeF& mm, double dpi)
{
    return QSize(std::max(1, static_cast<int>(std::lround(mm.width() * dpi / 25.4))),
                 std::max(1, static_cast<int>(std::lround(mm.height() * dpi / 25.4))));
}
}  // namespace

bool Document::exportPdf(const QString& path, const core::exporter::SvgOptions& o, QString* error)
{
    core::exporter::SvgPieces pieces;
    QSizeF mm;
    if (!exportPieces(m_sketch, o, pieces, mm, error)) {
        return false;
    }
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly)) {
        if (error) {
            *error = tr("Couldn't write %1: %2").arg(path, file.errorString());
        }
        return false;
    }
    {
        QPdfWriter pdf(&file);
        pdf.setTitle(QFileInfo(path).completeBaseName());
        pdf.setCreator(QStringLiteral("Setsquare"));
        pdf.setResolution(1200);
        // The page exactly the drawing's size (a landscape one as it is, not turned), no margins.
        pdf.setPageLayout(QPageLayout(QPageSize(mm, QPageSize::Millimeter, QString(), QPageSize::ExactMatch),
                                      QPageLayout::Portrait, QMarginsF()));
        QPainter painter;
        if (!painter.begin(&pdf)) {
            if (error) {
                *error = tr("Couldn't write %1").arg(path);
            }
            return false;
        }
        // True size: millimetres from the resolution (the page itself is whole points, as Qt writes
        // it, so up to half a point off the drawing's size; the drawing isn't stretched to it).
        paintPieces(painter, pieces, mm, pdf.resolution() / 25.4);
        painter.end();
    }
    if (!file.commit()) {
        if (error) {
            *error = tr("Couldn't write %1: %2").arg(path, file.errorString());
        }
        return false;
    }
    announce(QStringLiteral("doc.export_pdf(%1, fills=%2, undecorated=%3, construction=%4, margin=%5%6%7%8)")
                 .arg(pyString(path), pyBool(o.fills), pyBool(o.undecorated), pyBool(o.construction),
                      Editor::number(o.margin), o.page ? QString() : QStringLiteral(", page=False"),
                      o.textAsOutlines ? QStringLiteral(", text_as_outlines=True") : QString(),
                      o.dimensions ? QString() : QStringLiteral(", dimensions=False")));
    return true;
}

QSize Document::exportImageSize(const core::exporter::SvgOptions& o, double dpi) const
{
    core::exporter::SvgPieces pieces;
    QSizeF mm;
    if (!exportPieces(m_sketch, o, pieces, mm, nullptr)) {
        return {};
    }
    return pixelsFor(mm, dpi);
}

bool Document::exportImage(const QString& path, const core::exporter::SvgOptions& o, const ImageOptions& image,
                           QString* error)
{
    const QString suffix = QFileInfo(path).suffix().toLower();
    const bool jpeg = suffix == QStringLiteral("jpg") || suffix == QStringLiteral("jpeg");
    if (!jpeg && suffix != QStringLiteral("png")) {
        if (error) {
            *error = tr("Couldn't export: an image is .png, .jpg or .jpeg, not .%1").arg(suffix);
        }
        return false;
    }
    if (!(image.dpi >= 1.0 && image.dpi <= 10000.0)) {
        if (error) {
            *error = tr("Couldn't export: the resolution has to be between 1 and 10000 dpi");
        }
        return false;
    }
    core::exporter::SvgPieces pieces;
    QSizeF mm;
    if (!exportPieces(m_sketch, o, pieces, mm, error)) {
        return false;
    }
    const QSize pixels = pixelsFor(mm, image.dpi);
    if (pixels.width() > kMaxImageSide || pixels.height() > kMaxImageSide
        || qint64(pixels.width()) * pixels.height() > kMaxImagePixels) {
        if (error) {
            *error = tr("Couldn't export: %1 × %2 pixels is too big (at most %3 pixels a side, and %4 million in all). "
                        "Try a lower resolution.")
                         .arg(pixels.width())
                         .arg(pixels.height())
                         .arg(kMaxImageSide)
                         .arg(kMaxImagePixels / 1000000);
        }
        return false;
    }
    QImage picture(pixels, QImage::Format_ARGB32_Premultiplied);
    if (picture.isNull()) {
        if (error) {
            *error = tr("Couldn't export: not enough memory for %1 × %2 pixels").arg(pixels.width()).arg(pixels.height());
        }
        return false;
    }
    // (A JPEG has no transparency: white behind it. A page's own colour is drawn over this anyway.)
    picture.fill(image.transparent && !jpeg ? QColor(Qt::transparent) : QColor(Qt::white));
    const int perMetre = static_cast<int>(std::lround(image.dpi / 0.0254));
    picture.setDotsPerMeterX(perMetre);  // (so it prints at true size)
    picture.setDotsPerMeterY(perMetre);
    {
        QPainter painter(&picture);
        paintPieces(painter, pieces, mm, image.dpi / 25.4);
    }
    if (jpeg) {
        picture = picture.convertToFormat(QImage::Format_RGB32);
    }
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly)) {
        if (error) {
            *error = tr("Couldn't write %1: %2").arg(path, file.errorString());
        }
        return false;
    }
    QImageWriter writer(&file, jpeg ? QByteArrayLiteral("jpg") : QByteArrayLiteral("png"));
    if (jpeg) {
        writer.setQuality(std::clamp(image.quality, 1, 100));
    }
    if (!writer.write(picture) || !file.commit()) {
        if (error) {
            *error = tr("Couldn't write %1: %2")
                         .arg(path, writer.error() != QImageWriter::UnknownError ? writer.errorString() : file.errorString());
        }
        return false;
    }
    announce(QStringLiteral("doc.export_image(%1, dpi=%2, fills=%3, undecorated=%4, construction=%5, margin=%6%7%8%9)")
                 .arg(pyString(path), Editor::number(image.dpi), pyBool(o.fills), pyBool(o.undecorated),
                      pyBool(o.construction), Editor::number(o.margin),
                      o.page ? QString() : QStringLiteral(", page=False"),
                      o.dimensions ? QString() : QStringLiteral(", dimensions=False"),
                      (image.transparent && !jpeg ? QStringLiteral(", transparent=True") : QString())
                          + (jpeg && image.quality != 92 ? QStringLiteral(", quality=%1").arg(image.quality) : QString())));
    return true;
}
void Document::newSketch()
{
    reset();
    m_filePath.clear();
    emit fileStateChanged();
    announce(QStringLiteral("doc.new()"));
}
