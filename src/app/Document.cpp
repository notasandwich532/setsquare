#include "Document.h"

#include "TextShapes.h"

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
#include <QSaveFile>

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
    commit(std::move(working));
    resolve();
    emit changed();
}

void Document::edit(const QString& /*label*/, const std::function<void(Editor&)>& fn)
{
    core::Sketch working = m_sketch;
    QStringList python;
    Editor editor(working, &python);
    fn(editor);  // may throw; m_sketch is untouched in that case
    commit(std::move(working));
    resolve();
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
        return core::kNoId;
    }
    record(Editor::constraintCall(m_sketch, id));
    return id;
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
    QStringList names;
    for (core::Id item : items) {
        if (m_sketch.hasEntity(item) && !m_sketch.isReference(item)) {
            names << Editor::name(m_sketch, item);
        }
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
    if (!m_sketch.hasConstraint(dimension)) {
        return;
    }
    m_sketch.setConstraintLabel(dimension, dx, dy);
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
    pushUndo(std::move(before), m_history);  // (the edit's own commands are recorded after this)
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

void Document::resolve()
{
    m_result = core::solve(m_sketch);
    textshapes::refreshPathTexts(m_sketch);  // text along a curve follows it
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
    announce(QStringLiteral("doc.export_svg(%1, fills=%2, undecorated=%3, construction=%4, margin=%5)")
                 .arg(pyString(path), pyBool(o.fills), pyBool(o.undecorated), pyBool(o.construction),
                      Editor::number(o.margin)));
    return true;
}

bool Document::exportDxf(const QString& path, const core::exporter::DxfOptions& o, QString* error)
{
    if (!writeExport(path, [&] { return core::exporter::toDxf(m_sketch, o); }, error)) {
        return false;
    }
    announce(QStringLiteral("doc.export_dxf(%1, styles=%2, construction=%3)")
                 .arg(pyString(path), pyBool(o.styles), pyBool(o.construction)));
    return true;
}

void Document::newSketch()
{
    reset();
    m_filePath.clear();
    emit fileStateChanged();
    announce(QStringLiteral("doc.new()"));
}
