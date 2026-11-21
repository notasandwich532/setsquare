#pragma once

#include "Editor.h"

#include <core/Export.h>
#include <core/Serialize.h>
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
    // The next edit (only) merges into the last one if that had the same key and nothing has
    // happened since: one undo step and one recorded command for a run of them (each key typed
    // into a field is an edit).
    void setMergeKey(const QString& key) { m_nextMergeKey = key; }

    // Groups everything until endTransaction() into one undo step (e.g. a whole script).
    // Nests; undo/redo are refused while one is open.
    void beginTransaction();
    void endTransaction();
    bool inTransaction() const { return m_transactionDepth > 0; }

    // While alive, nothing is emitted through recorded() (it still goes into the history); used
    // for calls made from Python, which is already code.
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
    // A midpoint whose "halfway" part is already implied becomes point-on-line instead (check
    // the returned constraint's type).
    core::Id addConstraint(core::ConstraintType type, std::vector<core::Id> refs, double value = 0.0,
                           QString* error = nullptr, double labelX = 0.0, double labelY = 0.0,
                           std::uint8_t flip = 0);

    // Changes a dimension or feature value (pattern spacing/angle, offset distance), rejecting
    // values the solver can't satisfy. A feature's source geometry stays put.
    bool setDimensionValue(core::Id dimension, double value, QString* error = nullptr);  // (no longer an expression)
    // A dimension worked out from the parameters ("width / 2"; see core/Parameters.h). Without a
    // parameter in it, it's just a value. Recorded as d12.value = "width / 2".
    bool setDimensionExpression(core::Id dimension, const QString& expression, QString* error = nullptr);

    // The drawing's parameters. Each change works out again everything that uses them and
    // re-solves; false (with why, nothing changed) if that can't be done. Recorded as
    // s.parameter("width", "120"), s.rename_parameter(...), s.delete_parameter(...).
    bool setParameter(const QString& name, const QString& expression, QString* error = nullptr);  // (new: added)
    bool renameParameter(const QString& from, const QString& to, QString* error = nullptr);  // (and where it's used)
    bool deleteParameter(const QString& name, QString* error = nullptr);  // (not while something uses it)
    // Groups items (curves, points) so they keep their places relative to each other (see
    // core/Groups.h). Checked like a constraint; kNoId (and why) if it can't be.
    core::Id addGroup(const std::vector<core::Id>& items, QString* error = nullptr);

    // Sublayers: which one is being edited, and which are shown. Not undo steps (like scrolling
    // the view), but saved with the drawing. (Adding, renaming, deleting and moving things between
    // sublayers are edits: see Editor.)
    void setActiveSublayer(core::Id sublayer);
    void setSublayerVisible(core::Id sublayer, bool visible);
    // Layers likewise.
    void setActiveLayer(core::Id layer);
    void setLayerVisible(core::Id layer, bool visible);

    // Moving a dimension label (no solve); one undo step per drag.
    void beginLabelDrag();
    void moveLabel(core::Id dimension, double dx, double dy);
    void endLabelDrag();

    // Interactive drag; the whole drag is one undo step.
    void beginDrag(const std::vector<core::Id>& points);
    void beginDragEdge(core::Id roundEntity, double x, double y);
    void dragTo(double dx, double dy);
    // The drag is stuck on something that looks free but is held exactly (see
    // core::Solver::dragLocked).
    bool dragLocked() const { return m_dragSolver && m_dragSolver->dragLocked(); }
    // Moves the dragged points rigidly: turned by `angle` degrees about `about`, then shifted.
    // Recorded as s.move(...).
    void dragMove(double dx, double dy, double angle, core::Vec2 about);
    // The horizontal/vertical constraints the move under way has dropped: turned other than by
    // half turns, they'd stop it (ids as they were before the drag). Empty if none.
    const std::vector<core::Id>& droppedByTurn() const { return m_dragDropped; }
    // The horizontal and vertical constraints held entirely within these points.
    static std::vector<core::Id> levelConstraints(const core::Sketch& sketch, const std::vector<core::Id>& points);
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

    // --- command history ---
    // The Python commands that built the current sketch, oldest first, one per edit: what the
    // recorder writes for UI actions and for calls made from scripts and the console. Undo takes
    // the undone commands off (redo puts them back), so replaying the history on a new sketch
    // rebuilds this one. Saved in .setsq files.
    const QStringList& history() const { return m_history; }
    // The history (or its first part) was recorded by an older version of the app, whose commands
    // made other things: replayed now, it may not rebuild the drawing exactly (see kRecorderVersion).
    bool historyFromOlderVersion() const { return !m_history.isEmpty() && m_historyRecorder < core::kRecorderVersion; }

    // Changes with every committed edit (not while a drag is in progress).
    quint64 revision() const { return m_revision; }

    // --- files ---
    QString filePath() const { return m_filePath; }  // empty until saved or opened
    bool isModified() const { return m_revision != m_savedRevision; }
    bool save(const QString& path, QString* error = nullptr);
    bool open(const QString& path, QString* error = nullptr);
    // Writes the drawing to `path` as save does, but as a copy: the document's own file and its
    // saved state stay as they were (for autosave; see Recovery).
    bool saveCopy(const QString& path, QString* error = nullptr) const;
    // Opens a copy autosaved before a crash as unsaved changes to `original` (empty: untitled), so
    // saving goes to the original.
    bool openRecovered(const QString& copy, const QString& original, QString* error = nullptr);
    // Writes the drawing as SVG (see core/Export.h). Doesn't change the document.
    bool exportSvg(const QString& path, const core::exporter::SvgOptions& options, QString* error = nullptr);
    bool exportDxf(const QString& path, const core::exporter::DxfOptions& options, QString* error = nullptr);
    // A PDF of the drawing as the SVG export makes it (the page, at true size; or fitted round the
    // drawing): vectors, and text as text in its embedded font. Hatches are always their lines.
    bool exportPdf(const QString& path, const core::exporter::SvgOptions& options, QString* error = nullptr);
    // A PNG or JPEG (by the path's suffix) of the drawing as the PDF export draws it, at `dpi`
    // (saved in it, so it prints at true size). Text in its font; hatches as their lines.
    struct ImageOptions
    {
        double dpi = 300.0;
        bool transparent = false;  // PNG: nothing behind the drawing (else white; a page's colour shows anyway)
        int quality = 92;          // JPEG, 1-100
    };
    bool exportImage(const QString& path, const core::exporter::SvgOptions& options, const ImageOptions& image,
                     QString* error = nullptr);
    // How big exportImage would make it (pixels); empty if there's nothing to export.
    QSize exportImageSize(const core::exporter::SvgOptions& options, double dpi) const;
    static constexpr int kMaxImageSide = 32768;
    static constexpr qint64 kMaxImagePixels = 268'000'000;  // (about 1 GB while drawing it)
    void newSketch();

signals:
    // Geometry and/or structure changed; listeners should repaint and re-query.
    void changed();
    void undoStateChanged();
    void fileStateChanged();  // path or modified flag
    // Python equivalent of a user action that just happened (one or more lines).
    void recorded(const QString& python);
    // An edit was refused: these constraints, and fixed geometry they act on, are in its way.
    void blocked(const std::vector<core::Id>& items);

private:
    void pushUndo(core::Sketch snapshot, QStringList history);
    // Replaces the current sketch with a new state as one undo step (or folds it into the
    // open transaction).
    void commit(core::Sketch next);
    void record(const QString& python);      // an edit: recorded and kept in the history
    void announce(const QString& python);    // recorded but not history (undo, save, ...)
    // Commits a solved working copy if it isn't more over-constrained than the current sketch.
    // `hold`: points to keep in place where the sketch leaves a choice.
    // edited: what the change was to (blamed, with what holds it, if the solve just fails).
    bool commitChecked(core::Sketch working, QString* error, const std::vector<core::Id>& hold = {},
                       const std::vector<core::Id>& edited = {});
    static std::vector<core::Id> blockersOf(const core::Sketch& s, const std::vector<core::Id>& constraints);
    static QString describeItem(const core::Sketch& s, core::Id id);
    core::Id midpointOnLine(const std::vector<core::Id>& refs, QString* error);
    core::Id collinearByPoint(const std::vector<core::Id>& refs, QString* error);
    // Writes an export (made by `make`, which may throw SketchError) to `path`.
    bool writeExport(const QString& path, const std::function<std::string()>& make, QString* error);
    // Solves and brings what follows the geometry up to date (texts on curves, fitted boxes,
    // fills); given how it was before an edit, fills go along with shapes that moved.
    void resolve(const core::Sketch* before = nullptr);
    void followProjections();  // while dragging

    core::Sketch m_sketch;
    core::SolveResult m_result;

    // Each history entry remembers its revision so undoing back to the saved state counts as
    // unmodified.
    struct Snapshot
    {
        core::Sketch sketch;
        quint64 revision;
        QStringList history;  // the commands that built `sketch`
    };
    std::vector<Snapshot> m_undo;
    std::vector<Snapshot> m_redo;
    quint64 m_revision = 0;       // identifies the current state
    quint64 m_nextRevision = 1;
    quint64 m_savedRevision = 0;  // state last saved/opened
    static constexpr quint64 kNeverSaved = ~quint64(0);  // (no state is: recovered work is unsaved)
    QString m_filePath;

    std::unique_ptr<core::Solver> m_dragSolver;
    core::Sketch m_beforeDrag;
    bool m_dragMoved = false;
    bool m_labelDragging = false;
    QString m_nextMergeKey;
    QString m_lastMergeKey;
    quint64 m_lastMergeRevision = 0;

    // What the current drag is, for recording it when it ends.
    std::vector<core::Id> m_dragPoints;
    core::Id m_dragEdge = core::kNoId;
    double m_dragGrabX = 0.0, m_dragGrabY = 0.0;
    double m_dragDx = 0.0, m_dragDy = 0.0;
    bool m_dragIsMove = false;  // dragMove() rather than dragTo()
    double m_dragAngle = 0.0;
    core::Vec2 m_dragAbout;
    std::vector<core::Id> m_dragDropped;
    core::Id m_labelDimension = core::kNoId;

    int m_transactionDepth = 0;
    core::Sketch m_beforeTransaction;
    QStringList m_historyBeforeTransaction;
    quint64 m_revisionBeforeTransaction = 0;
    bool m_transactionChanged = false;

    int m_silence = 0;
    QStringList m_history;
    int m_historyRecorder = core::kRecorderVersion;  // (saved with it; older: see historyFromOlderVersion)
};
