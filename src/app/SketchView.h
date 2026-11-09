#pragma once

#include "tools/DrawTools.h"
#include "tools/Tool.h"

#include <core/Dimensions.h>
#include <core/Geometry.h>
#include <core/Gradient.h>
#include <core/Hatch.h>
#include <core/Regions.h>

#include "Images.h"

#include <QPainterPath>

#include <QElapsedTimer>
#include <QImage>
#include <QPen>
#include <QPointF>
#include <QPolygonF>
#include <QWidget>

#include <cstdint>
#include <deque>
#include <map>
#include <optional>
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
    ToolKind previousTool() const { return m_previousTool; }  // the one before this (not the eyedropper)
    QString hint() const;
    void refreshTool();  // repaint and re-publish the tool's hint

    // --- picking ---
    // Constraint symbols and dimension labels first (when annotations is true), then points,
    // then curves.
    // What's under the cursor. Hidden sublayers are never picked; `editable`: only what's in the
    // active sublayer (to select, drag, delete or change), not the others shown as references.
    core::Id pickAt(QPointF screen, bool annotations = true, bool editable = false) const;
    core::Id textAt(QPointF screen, bool editable = false) const;  // a text whose box is under the cursor
    // Pick options for changing things: only the active sublayer.
    core::PickOptions editableOptions(double pixels) const;
    // Where a drawing tool should put a point: an existing point, a line midpoint, a spot on a
    // curve, or free space (in that order of preference).
    Snap snap(QPointF screen) const;

    // --- dimensions ---
    void editDimension(core::Id dimension);  // inline value editor at the label
    // A dimension's or feature's value, in an inline editor centred at `at` (screen).
    void editValue(core::Id constraint, QPointF at);
    // Patterns: their command, editing them; offsets: the inline distance editor.
    void editFeature(core::Id feature);
    void editText(core::Id text);  // the Text tool, on it
    // The Gradient tool, editing an item's gradient (a fill, a curve's stroke or a text).
    void editGradient(core::Id item);
    // The Image tool, on a reference image.
    void editImage(core::Id image);
    // An image drawn in place of the one with its id (an edit in progress, e.g. while dragging it).
    void setImagePreview(std::optional<core::Image> image);
    const std::optional<core::Image>& imagePreview() const { return m_imagePreview; }
    core::Id imageAt(core::Vec2 world) const;  // the topmost shown image of the active layer there

    // What Paste places (set before switching to ToolKind::Paste).
    void setPasteFragment(core::Sketch fragment);
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
    // Draws the entities of `sketch` with ids above `after`, in the preview style.
    void drawPreviewSketch(QPainter& painter, const core::Sketch& sketch, core::Id after) const;
    // Draws the entities of `sketch` that `include` picks, in the preview style.
    void drawPreviewSketch(QPainter& painter, const core::Sketch& sketch,
                           const std::function<bool(core::Id)>& include) const;

    // --- tool options ---
    int polygonSides() const { return m_polygonSides; }
    void setPolygonSides(int sides);

    // Settings for the modify tools, from the toolbar: default sizes for fillets and chamfers
    // (each is then editable as a dimension), and the move/scale/pattern/offset options.
    struct ModifyOptions
    {
        double filletRadius = 5.0;
        double chamferDistance = 5.0;
        double chamferDistance2 = 5.0;
        double chamferAngle = 45.0;

        bool copy = false;  // move/rotate copy the selection rather than moving it
        double scaleFactor = 2.0;
        int patternCount = 3;
        double patternSpacing = 10.0;
        double patternAngle = 0.0;
        int patternCount2 = 1;
        double patternSpacing2 = 10.0;
        int circularCount = 6;
        double circularAngle = 360.0;
        double offsetDistance = 5.0;
        bool offsetBothSides = false;
        bool offsetCaps = true;
        bool divideByRatio = false;
        int divideCount = 3;
        QString divideRatio = QStringLiteral("3:2:1");
        QString textFont;  // empty: the default
        double textHeight = 5.0;
        bool textBold = false;
        bool textItalic = false;
        int textOutline = 0;  // core::TextOutline
        double textMinThickness = 0.0;
        double textSpacing = 0.0;
    };
    ModifyOptions& modifyOptions() { return m_modifyOptions; }
    const ModifyOptions& modifyOptions() const { return m_modifyOptions; }

    // --- appearance ---
    // Presentation view shows the drawing as it will look: strokes at full strength, with no
    // constraint symbols, dimensions or points (undecorated curves still show as geometry).
    // Sketch view, the default, fades strokes under the geometry. Picking a drawing or
    // constraint tool goes back to sketch view.
    bool presentation() const { return m_presentation; }
    void setPresentation(bool on);

    // The current paint, as in MS Paint: colour 1 for strokes, colour 2 for fills, plus the
    // line width and style that new strokes get.
    struct Paint
    {
        core::Color color1{0xff, 0xff, 0xff};
        core::Color color2{0x4d, 0xa3, 0xff};
        double width = 0.35;
        core::Dash dash = core::Dash::Solid;

        core::StrokeStyle stroke() const { return {color1, width, dash, {}}; }  // (plain: no gradient)
        bool operator==(const Paint&) const = default;
    };
    const Paint& paint() const { return m_paint; }
    void setPaint(const Paint& paint);
    // A colour picked in the colour bar: true if the current tool took it (see Tool::takeColor).
    bool offerColor(const core::Color& color, bool second);
    // The gradient tool's gradient (kept while switching tools).
    const core::Gradient& gradient() const { return m_gradient; }
    void setGradient(const core::Gradient& gradient);
    class GradientTool* gradientTool() const;  // the current tool, if it's the gradient tool

    // How a stroke style is drawn at the current zoom.
    QPen strokePen(const core::StrokeStyle& style) const;
    // A gradient placed by its sketch geometry (see core/Gradient.h), on screen; `alpha` scales
    // its opacity (faded while sketching). Its average colour while its geometry is missing.
    QBrush gradientBrush(const core::Gradient& g, double alpha = 1.0) const;
    // A closed area (with its holes) on screen, for filling.
    QPainterPath regionPath(const core::regions::Region& region) const;
    // A hatched fill's lines over its area (its shade, where they're too close together to show).
    void drawHatch(QPainter& painter, const core::Fill& fill, const core::regions::Region& region) const;
    QPainterPath textPath(const core::Text& text) const;  // its letters, on screen
    QPolygonF textBox(const core::Text& text) const;      // round its letters, on screen

    // Constraint symbols on the canvas (dimensions are always shown).
    bool showConstraints() const { return m_showConstraints; }
    void setShowConstraints(bool show);
    void drawLabel(QPainter& painter, core::Vec2 at, const QString& text) const;

signals:
    void cursorMoved(QPointF world);
    void hintChanged(const QString& hint);
    void toolChanged(ToolKind kind);
    void selectionChanged();
    void message(const QString& text);
    void polygonSidesChanged(int sides);
    void recentToolsRequested(QPoint globalPos);  // right-click with nothing to cancel
    void presentationChanged(bool on);
    void paintChanged();
    void gradientChanged();
    void colorSampled(const core::Color& color);  // taken with the eyedropper

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
    void drawPage(QPainter& painter) const;  // the sheet (see core::Page)
    void drawImages(QPainter& painter, core::Id layer) const;  // a layer's reference images
    // `only`: just that item (drawn over the cached picture as it's hovered), not the rest.
    void drawSketch(QPainter& painter, core::Id only = core::kNoId) const;
    void drawAnnotations(QPainter& painter, core::Id only = core::kNoId) const;  // constraint symbols + dimensions
    void drawArcPath(QPainter& painter, const core::ArcShape& arc) const;
    // Ellipses, elliptical arcs and splines (any curve, really), as their path.
    QPainterPath curvePath(const core::Sketch& sketch, core::Id curve) const;
    // A control-point spline's control polygon (faint), while sketching.
    // (`always`: however small it is on screen; else not when it would only be a smudge)
    void drawControlPolygon(QPainter& painter, const core::Sketch& sketch, core::Id spline, bool always = false) const;
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
    ToolKind m_previousTool = ToolKind::Select;

    std::set<core::Id> m_selection;
    core::Id m_hovered = core::kNoId;
    int m_polygonSides = 6;
    ModifyOptions m_modifyOptions;
    bool m_showConstraints = true;
    core::Sketch m_pasteFragment;
    bool m_presentation = false;
    Paint m_paint;
    core::Gradient m_gradient;
    mutable images::Cache m_imageCache;
    // Each layer's fill areas as last found, so a paint needn't find them again while its
    // geometry stays as it was (or only moves as a whole: then they're moved with it).
    struct FillCache
    {
        std::uint64_t key = 0;  // the layer's geometry and fills, measured in that frame
        core::Vec2 anchor;      // its first point, when the areas were found
        core::Vec2 axis{1, 0};  // from there towards its farthest point
        std::map<core::Id, std::optional<core::regions::Region>> regions;
    };
    mutable std::map<core::Id, FillCache> m_fillCache;
    const std::map<core::Id, std::optional<core::regions::Region>>& fillRegions(core::Id layer) const;
    std::optional<core::Image> m_imagePreview;
    // The drawing as last painted (background, images, grid, page, sketch: all but the tool's
    // overlay), with nothing hovered: while only the hover changes (the mouse moving over it), a
    // paint copies it and draws the hovered item over it. Painted again when anything it shows
    // changes: the document, the view, the selection, the view's settings.
    struct BaseKey
    {
        quint64 document = 0;
        quint64 preview = 0;
        double zoom = 0.0;
        QPointF pan;
        QSize size;
        qreal ratio = 0.0;
        bool presentation = false;
        bool showConstraints = false;
        std::set<core::Id> selection;
        bool operator==(const BaseKey&) const = default;
    };
    QImage m_base;
    std::optional<BaseKey> m_baseKey;
    quint64 m_documentStamp = 0;  // counts the document's changes
    quint64 m_previewStamp = 0;   // and the image preview's
    core::Id m_editFeature = core::kNoId;  // the pattern the next pattern command edits
    // Settings panel for the tools that have settings but no command (fillet, polygon, ...).
    class CommandPanel* m_optionsPanel = nullptr;
    void showOptionsPanel(ToolKind kind);

    // Constraint symbols and dimension labels, laid out in screen space. Computed on demand for
    // both painting and picking, so picking never sees a stale layout.
    struct Annotation
    {
        core::Id id = core::kNoId;
        bool isDimension = false;
        QRectF rect;  // symbol box or dimension text box
        QString text;
        core::DimensionLayout dimension;
        QString icon;  // symbols drawn as a tool icon (features, fixed items) rather than text
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
