#pragma once

#include "Editor.h"

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
    // the sketch untouched and propagate to the caller. Not recorded: for tests and internals.
    void modify(const QString& label, const std::function<void(core::Sketch&)>& edit);

    // Same, through the recording Editor: the UI's way of changing geometry. The Python
    // equivalent of the edit is emitted via recorded().
    void edit(const QString& label, const std::function<void(Editor&)>& fn);

    // Groups everything until endTransaction() into one undo step (e.g. a whole script).
    // Nests; undo/redo are refused while one is open.
    void beginTransaction();
    void endTransaction();
    bool inTransaction() const { return m_transactionDepth > 0; }

    // While alive, nothing is emitted through recorded(); used for calls made from Python,
    // which is already code.
    class SilentRecording
    {
    public:
        explicit SilentRecording(Document& d)
            : m_doc(d)
        {
            ++m_doc.m_silence;
        }
        ~SilentRecording() { --m_doc.m_silence; }
        SilentRecording(const SilentRecording&) = delete;
        SilentRecording& operator=(const SilentRecording&) = delete;

    private:
        Document& m_doc;
    };

    // Adds a constraint unless it would over-constrain the sketch (conflicting or redundant
    // with what's there), as Fusion does. Returns the new id, or kNoId with *error set.
    core::Id addConstraint(core::ConstraintType type, std::vector<core::Id> refs, double value = 0.0,
                           QString* error = nullptr, double labelX = 0.0, double labelY = 0.0,
                           std::uint8_t flip = 0);

    // Changes a dimension, rejecting values the solver can't satisfy.
    bool setDimensionValue(core::Id dimension, double value, QString* error = nullptr);

    // Moving a dimension label (no solve); one undo step per drag.
    void beginLabelDrag();
    void moveLabel(core::Id dimension, double dx, double dy);
    void endLabelDrag();

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

    // Replaces the sketch wholesale (new document / load); clears history. Every sketch gets
    // an origin point.
    void reset(core::Sketch sketch = {});

    // --- files ---
    QString filePath() const { return m_filePath; }  // empty until saved or opened
    bool isModified() const { return m_revision != m_savedRevision; }
    bool save(const QString& path, QString* error = nullptr);
    bool open(const QString& path, QString* error = nullptr);
    void newSketch();

signals:
    // Geometry and/or structure changed; listeners should repaint and re-query.
    void changed();
    void undoStateChanged();
    void fileStateChanged();  // path or modified flag
    // Python equivalent of a user action that just happened (one or more lines).
    void recorded(const QString& python);

private:
    void pushUndo(core::Sketch snapshot);
    // Replaces the current sketch with a new state as one undo step (or folds it into the
    // open transaction).
    void commit(core::Sketch next);
    void record(const QString& python);
    // Commits a solved working copy if it isn't more over-constrained than the current sketch.
    bool commitChecked(core::Sketch working, QString* error);
    void resolve();

    core::Sketch m_sketch;
    core::SolveResult m_result;

    // Each history entry remembers its revision so undoing back to the saved state counts as
    // unmodified.
    struct Snapshot
    {
        core::Sketch sketch;
        quint64 revision;
    };
    std::vector<Snapshot> m_undo;
    std::vector<Snapshot> m_redo;
    quint64 m_revision = 0;       // identifies the current state
    quint64 m_nextRevision = 1;
    quint64 m_savedRevision = 0;  // state last saved/opened
    QString m_filePath;

    std::unique_ptr<core::Solver> m_dragSolver;
    core::Sketch m_beforeDrag;
    bool m_dragMoved = false;
    bool m_labelDragging = false;

    // What the current drag is, for recording it when it ends.
    std::vector<core::Id> m_dragPoints;
    core::Id m_dragEdge = core::kNoId;
    double m_dragGrabX = 0.0, m_dragGrabY = 0.0;
    double m_dragDx = 0.0, m_dragDy = 0.0;
    core::Id m_labelDimension = core::kNoId;

    int m_transactionDepth = 0;
    core::Sketch m_beforeTransaction;
    quint64 m_revisionBeforeTransaction = 0;
    bool m_transactionChanged = false;

    int m_silence = 0;
};
