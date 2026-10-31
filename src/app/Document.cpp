#include "Document.h"

#include <core/Serialize.h>

#include <QFile>
#include <QSaveFile>

namespace
{
constexpr size_t kMaxUndo = 200;
}

Document::Document(QObject* parent)
    : QObject(parent)
{
    m_sketch.ensureOrigin();
    resolve();
}

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
            *error = tr("Couldn't read %1: %2").arg(path, QString::fromStdString(e.what()));
        }
        return false;
    }
    reset(std::move(loaded));
    m_filePath = path;
    emit fileStateChanged();
    return true;
}

void Document::newSketch()
{
    reset();
    m_filePath.clear();
    emit fileStateChanged();
}

Document::~Document() = default;

void Document::modify(const QString& /*label*/, const std::function<void(core::Sketch&)>& edit)
{
    core::Sketch working = m_sketch;
    edit(working);  // may throw; m_sketch is untouched in that case

    pushUndo(std::move(m_sketch));
    m_sketch = std::move(working);
    resolve();
    emit changed();
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
            *error = QString::fromStdString(e.what());
        }
        return core::kNoId;
    }
    return commitChecked(std::move(working), error) ? id : core::kNoId;
}

bool Document::setDimensionValue(core::Id dimension, double value, QString* error)
{
    core::Sketch working = m_sketch;
    try {
        working.setConstraintValue(dimension, value);
    }
    catch (const core::SketchError& e) {
        if (error) {
            *error = QString::fromStdString(e.what());
        }
        return false;
    }
    return commitChecked(std::move(working), error);
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
    pushUndo(std::move(m_sketch));
    m_sketch = std::move(working);
    m_result = std::move(result);
    emit changed();
    return true;
}

void Document::beginLabelDrag()
{
    m_beforeDrag = m_sketch;
    m_labelDragging = true;
    m_dragMoved = false;
}

void Document::moveLabel(core::Id dimension, double dx, double dy)
{
    if (!m_sketch.hasConstraint(dimension)) {
        return;
    }
    m_sketch.setConstraintLabel(dimension, dx, dy);
    m_dragMoved = true;
    emit changed();
}

void Document::endLabelDrag()
{
    if (m_labelDragging && m_dragMoved) {
        pushUndo(std::move(m_beforeDrag));
    }
    m_beforeDrag = {};
    m_labelDragging = false;
}

void Document::beginDrag(const std::vector<core::Id>& points)
{
    m_beforeDrag = m_sketch;
    m_dragMoved = false;
    m_dragSolver = std::make_unique<core::Solver>(m_sketch);
    m_dragSolver->beginDrag(points);
}

void Document::beginDragEdge(core::Id roundEntity, double x, double y)
{
    m_beforeDrag = m_sketch;
    m_dragMoved = false;
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
        pushUndo(std::move(m_beforeDrag));
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

void Document::undo()
{
    if (m_undo.empty() || isDragging()) {
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
}

void Document::redo()
{
    if (m_redo.empty() || isDragging()) {
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
}

void Document::reset(core::Sketch sketch)
{
    m_dragSolver.reset();
    m_sketch = std::move(sketch);
    m_sketch.ensureOrigin();
    m_undo.clear();
    m_redo.clear();
    m_revision = m_savedRevision = m_nextRevision++;
    resolve();
    emit changed();
    emit undoStateChanged();
    emit fileStateChanged();
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

void Document::resolve()
{
    m_result = core::solve(m_sketch);
}
