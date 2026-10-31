#pragma once

#include "tools/DrawTools.h"
#include "tools/Tool.h"

#include <core/Dimensions.h>
#include <core/Geometry.h>

#include <QElapsedTimer>
#include <QPointF>
#include <QWidget>

#include <deque>
#include <functional>
#include <memory>
#include <set>
#include <vector>

class Document;
class QPainter;

// The sketch canvas. World coordinates are millimetres with +Y up; the view keeps a
// world->screen transform made of a pan offset and a uniform zoom factor. Input is forwarded to
// the active tool; the view itself handles panning (middle mouse), zooming (wheel) and keys.
class SketchView : public QWidget
{
    Q_OBJECT

public:
    explicit SketchView(Document& document, QWidget* parent = nullptr);
    ~SketchView() override;

    Document& document() const { return m_doc; }

    // --- transform ---
    QPointF toScreen(core::Vec2 world) const;
    core::Vec2 toWorld(QPointF screen) const;
    double zoom() const { return m_zoom; }  // pixels per world unit
    void resetView();
    void fitAll();  // zoom to show the whole drawing (or reset when it's empty)

    // --- tools ---
    void setTool(ToolKind kind);
    ToolKind toolKind() const { return m_toolKind; }
    QString hint() const;
    void refreshTool();  // repaint and re-publish the tool's hint

    // --- picking ---
    // Constraint symbols and dimension labels first (when annotations is true), then points,
    // then curves.
    core::Id pickAt(QPointF screen, bool annotations = true) const;
    // Where a drawing tool should put a point: an existing point, a line midpoint, a spot on a
    // curve, or free space (in that order of preference).
    Snap snap(QPointF screen) const;

    // --- dimensions ---
    void editDimension(core::Id dimension);  // inline value editor at the label
    QString formatDimension(const core::Constraint& c) const;
    void drawDimension(QPainter& painter, const core::DimensionLayout& layout, const QString& text,
                       const QColor& color, QRectF* textRect = nullptr) const;

    // --- selection ---
    const std::set<core::Id>& selection() const { return m_selection; }
    void setSelection(std::set<core::Id> ids);
    void setHovered(core::Id id);
    void selectAll();
    void deleteSelection();

    // Performance overlay: paint rate, input rate and per-frame costs.
    void setShowStats(bool show);
    bool showStats() const { return m_showStats; }

    // Runs a document edit; on failure shows the error and returns false.
    bool tryEdit(const QString& label, const std::function<void(Editor&)>& edit);

    // --- overlay drawing helpers for tools ---
    void drawPreviewLine(QPainter& painter, core::Vec2 a, core::Vec2 b) const;
    void drawPreviewCircle(QPainter& painter, core::Vec2 center, double radius) const;
    void drawPreviewArc(QPainter& painter, const core::ArcShape& arc) const;
    void drawSnapMarker(QPainter& painter, const Snap& snap) const;
    void drawLabel(QPainter& painter, core::Vec2 at, const QString& text) const;

signals:
    void cursorMoved(QPointF world);
    void hintChanged(const QString& hint);
    void toolChanged(ToolKind kind);
    void selectionChanged();
    void message(const QString& text);

protected:
    void paintEvent(QPaintEvent* event) override;
    void wheelEvent(QWheelEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;
    void mouseDoubleClickEvent(QMouseEvent* event) override;
    void keyPressEvent(QKeyEvent* event) override;
    void resizeEvent(QResizeEvent* event) override;
    void leaveEvent(QEvent* event) override;

private:
    void drawGrid(QPainter& painter) const;
    void drawSketch(QPainter& painter) const;
    void drawAnnotations(QPainter& painter) const;  // constraint symbols + dimensions
    void drawArcPath(QPainter& painter, const core::ArcShape& arc) const;
    ToolEvent makeEvent(const QMouseEvent* event) const;
    void onDocumentChanged();

    Document& m_doc;

    QPointF m_pan;        // screen position of the world origin
    double m_zoom = 4.0;  // pixels per world unit
    bool m_panning = false;
    QPointF m_lastMouse;

    // Until the user pans or zooms, the last fit/reset is re-applied when the canvas resizes
    // (first layout, DPI changes, window resizes right after opening a file).
    enum class AutoView
    {
        None,
        Reset,
        Fit,
    } m_autoView = AutoView::Reset;

    std::unique_ptr<Tool> m_tool;
    ToolKind m_toolKind = ToolKind::Select;

    std::set<core::Id> m_selection;
    core::Id m_hovered = core::kNoId;

    // Constraint symbols and dimension labels, laid out in screen space. Computed on demand for
    // both painting and picking, so picking never sees a stale layout.
    struct Annotation
    {
        core::Id id = core::kNoId;
        bool isDimension = false;
        QRectF rect;  // symbol box or dimension text box
        QString text;
        core::DimensionLayout dimension;
    };
    std::vector<Annotation> layoutAnnotations() const;
    QColor annotationColor(core::Id constraint, const QColor& normal) const;

    class QLineEdit* m_editor = nullptr;

    // Events per second over a sliding one-second window.
    struct RateCounter
    {
        std::deque<qint64> stamps;
        void tick(qint64 nowMs);
        int rate(qint64 nowMs);
    };
    void drawStats(QPainter& painter);

    // Repaints are paced to the display's vblank rather than painted as input arrives.
    void scheduleRepaint();
    class FrameClock* m_frameClock = nullptr;
    bool m_inPaint = false;

    bool m_showStats = false;
    QElapsedTimer m_clock;
    RateCounter m_paintRate;
    RateCounter m_inputRate;
    double m_paintMs = 0.0;  // last paint
    double m_inputMs = 0.0;  // last mouse-move handling (tool + solve)
};
