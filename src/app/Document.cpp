#include "Document.h"

#include <core/Serialize.h>

#include <QDir>
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
                                 QString* error, double labelX, double labelY)
{
    core::Sketch working = m_sketch;
    core::Id id = core::kNoId;
    try {
        id = working.addConstraint(type, std::move(refs), value);
        if (labelX != 0.0 || labelY != 0.0) {
            working.setConstraintLabel(id, labelX, labelY);
        }
    }
    catch (const core::SketchError& e) {
        if (error) {
            *error = errorText(e);
        }
        return core::kNoId;
    }
    if (!commitChecked(std::move(working), error)) {
        return core::kNoId;
    }
    record(Editor::constraintCall(m_sketch, id));
    return id;
}

bool Document::setDimensionValue(core::Id dimension, double value, QString* error)
{
    core::Sketch working = m_sketch;
    try {
        working.setConstraintValue(dimension, value);
    }
    catch (const core::SketchError& e) {
        if (error) {
            *error = errorText(e);
        }
        return false;
    }
    if (!commitChecked(std::move(working), error)) {
        return false;
    }
    record(QStringLiteral("%1.value = %2").arg(Editor::name(m_sketch, dimension), Editor::number(value)));
    return true;
}

bool Document::commitChecked(core::Sketch working, QString* error)
{
    core::SolveResult result = core::solve(working);  // writes back on success
    QString problem;
    if (result.conflicting.size() > m_result.conflicting.size()) {
        problem = tr("That would conflict with existing constraints");
    }
    else if (result.redundant.size() > m_result.redundant.size()) {
        problem = tr("That would over-constrain the sketch");
    }
    else if (!result.ok) {
        problem = tr("The sketch can't be solved like that");
    }
    if (!problem.isEmpty()) {
        if (error) {
            *error = problem;
        }
        return false;
    }
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
    m_dragSolver = std::make_unique<core::Solver>(m_sketch);
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
    m_dragSolver = std::make_unique<core::Solver>(m_sketch);
    m_dragSolver->beginDragEdge(roundEntity, x, y);
}

void Document::dragTo(double dx, double dy)
{
    if (!m_dragSolver) {
        return;
    }
    if (m_dragSolver->dragTo(dx, dy)) {
        m_dragSolver->writeBack(m_sketch);
        m_dragMoved = true;
        m_dragDx = dx;
        m_dragDy = dy;
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
        else {
            QStringList names;
            for (core::Id p : m_dragPoints) {
                names << Editor::name(before, p);
            }
            python = QStringLiteral("s.drag([%1], %2, %3)")
                         .arg(names.join(QStringLiteral(", ")), Editor::number(m_dragDx), Editor::number(m_dragDy));
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
    m_redo.push_back({std::move(m_sketch), m_revision});
    m_sketch = std::move(m_undo.back().sketch);
    m_revision = m_undo.back().revision;
    m_undo.pop_back();
    resolve();
    emit changed();
    emit undoStateChanged();
    emit fileStateChanged();
    record(QStringLiteral("doc.undo()"));
}

void Document::redo()
{
    if (m_redo.empty() || isDragging() || inTransaction()) {
        return;
    }
    m_undo.push_back({std::move(m_sketch), m_revision});
    m_sketch = std::move(m_redo.back().sketch);
    m_revision = m_redo.back().revision;
    m_redo.pop_back();
    resolve();
    emit changed();
    emit undoStateChanged();
    emit fileStateChanged();
    record(QStringLiteral("doc.redo()"));
}

void Document::beginTransaction()
{
    if (m_transactionDepth++ == 0) {
        m_beforeTransaction = m_sketch;
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
        pushUndo(std::move(m_beforeTransaction));  // the whole transaction is one step
    }
    m_beforeTransaction = {};
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
    pushUndo(std::move(before));
}

void Document::pushUndo(core::Sketch snapshot)
{
    m_undo.push_back({std::move(snapshot), m_revision});
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
    m_undo.clear();
    m_redo.clear();
    m_revision = m_savedRevision = m_nextRevision++;
    if (inTransaction()) {
        // A script opened a file / started over: the transaction continues from the new sketch.
        m_beforeTransaction = m_sketch;
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
}

void Document::record(const QString& python)
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
    const std::string json = core::toJson(m_sketch);
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
    record(QStringLiteral("doc.save(%1)").arg(pyString(path)));
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
    try {
        loaded = core::fromJson(file.readAll().toStdString());
    }
    catch (const core::SketchError& e) {
        if (error) {
            *error = tr("Couldn't read %1: %2").arg(path, errorText(e));
        }
        return false;
    }
    reset(std::move(loaded));
    m_filePath = path;
    emit fileStateChanged();
    record(QStringLiteral("doc.open(%1)").arg(pyString(path)));
    return true;
}

void Document::newSketch()
{
    reset();
    m_filePath.clear();
    emit fileStateChanged();
    record(QStringLiteral("doc.new()"));
}
