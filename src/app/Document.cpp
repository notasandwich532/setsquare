#include "Document.h"

namespace
{
constexpr size_t kMaxUndo = 200;
}

Document::Document(QObject* parent)
    : QObject(parent)
{
    resolve();
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
    m_redo.push_back(std::move(m_sketch));
    m_sketch = std::move(m_undo.back());
    m_undo.pop_back();
    resolve();
    emit changed();
    emit undoStateChanged();
}

void Document::redo()
{
    if (m_redo.empty() || isDragging()) {
        return;
    }
    m_undo.push_back(std::move(m_sketch));
    m_sketch = std::move(m_redo.back());
    m_redo.pop_back();
    resolve();
    emit changed();
    emit undoStateChanged();
}

void Document::reset(core::Sketch sketch)
{
    m_dragSolver.reset();
    m_sketch = std::move(sketch);
    m_undo.clear();
    m_redo.clear();
    resolve();
    emit changed();
    emit undoStateChanged();
}

void Document::pushUndo(core::Sketch snapshot)
{
    m_undo.push_back(std::move(snapshot));
    if (m_undo.size() > kMaxUndo) {
        m_undo.erase(m_undo.begin());
    }
    m_redo.clear();
    emit undoStateChanged();
}

void Document::resolve()
{
    m_result = core::solve(m_sketch);
}
