#pragma once

#include <core/Sketch.h>
#include <core/Solver.h>

#include <QObject>
#include <QString>

#include <functional>
#include <memory>
#include <vector>

// Owns the sketch being edited. Every change goes through modify() (or a drag session), which
// snapshots the sketch for undo, re-solves, and notifies listeners.
class Document : public QObject
{
    Q_OBJECT

public:
    explicit Document(QObject* parent = nullptr);
    ~Document() override;

    const core::Sketch& sketch() const { return m_sketch; }
    const core::SolveResult& solveResult() const { return m_result; }

    // Applies an edit as one undoable step. Exceptions from the edit (e.g. SketchError) leave
    // the sketch untouched and propagate to the caller.
    void modify(const QString& label, const std::function<void(core::Sketch&)>& edit);

    // Interactive drag; the whole drag is one undo step.
    void beginDrag(const std::vector<core::Id>& points);
    void beginDragEdge(core::Id roundEntity, double x, double y);
    void dragTo(double dx, double dy);
    void endDrag();
    void cancelDrag();  // restores the sketch to how it was when the drag began
    bool isDragging() const { return m_dragSolver != nullptr; }

    bool canUndo() const { return !m_undo.empty(); }
    bool canRedo() const { return !m_redo.empty(); }
    void undo();
    void redo();

    // Replaces the sketch wholesale (new document / load); clears history.
    void reset(core::Sketch sketch = {});

signals:
    // Geometry and/or structure changed; listeners should repaint and re-query.
    void changed();
    void undoStateChanged();

private:
    void pushUndo(core::Sketch snapshot);
    void resolve();

    core::Sketch m_sketch;
    core::SolveResult m_result;

    std::vector<core::Sketch> m_undo;
    std::vector<core::Sketch> m_redo;

    std::unique_ptr<core::Solver> m_dragSolver;
    core::Sketch m_beforeDrag;
    bool m_dragMoved = false;
};
