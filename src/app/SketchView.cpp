#include "SketchView.h"

#include "CommandPanel.h"
#include "Document.h"
#include "FrameClock.h"
#include "Theme.h"
#include "tools/ConstraintTools.h"
#include "tools/ModifyTools.h"
#include "tools/SelectTool.h"
#include "tools/ShapeTools.h"
#include "tools/StyleTools.h"
#include "tools/TransformTools.h"

#include <QDoubleSpinBox>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QSignalBlocker>
#include <QSpinBox>
#include <QPainter>
#include <QPainterPath>
#include <QScreen>
#include <QWheelEvent>

#include <algorithm>
#include <cmath>
#include <numbers>

namespace
{
constexpr double kMinZoom = 1e-3;
constexpr double kMaxZoom = 1e5;
constexpr double kZoomStep = 1.15;             // per wheel notch
constexpr double kMinGridPixelSpacing = 12.0;  // minor grid lines closer than this are hidden
constexpr double kPickPixels = 7.0;
constexpr double kSnapPixels = 10.0;
constexpr double kCurveWidth = 1.6;
constexpr double kHighlightWidth = 2.6;
constexpr double kPointRadius = 2.6;
constexpr double kRadToDeg = 180.0 / std::numbers::pi;
const QColor kConstrainedPink(0xff, 0x2b, 0xd6);  // geometry of the selected constraints
}  // namespace

// Tool helpers that need the full view definition.
Document& Tool::doc() const
{
    return m_view.document();
}

void Tool::refresh() const
{
    m_view.refreshTool();
}

SketchView::SketchView(Document& document, QWidget* parent)
    : QWidget(parent)
    , m_doc(document)
{
    setMouseTracking(true);
    setFocusPolicy(Qt::StrongFocus);
    setAttribute(Qt::WA_OpaquePaintEvent);
    setMinimumSize(200, 150);
    connect(&m_doc, &Document::changed, this, &SketchView::onDocumentChanged);
    // A refused edit: show what's in its way.
    connect(&m_doc, &Document::blocked, this, [this](const std::vector<core::Id>& items) {
        if (!items.empty()) {
            setSelection(std::set<core::Id>(items.begin(), items.end()));
        }
    });
    m_clock.start();

    m_frameClock = new FrameClock(this);
    connect(m_frameClock, &FrameClock::frame, this, [this] {
        if (isVisible()) {
            repaint();  // paint now, right after the vblank
        }
    });

    setTool(ToolKind::Select);
}

void SketchView::scheduleRepaint()
{
    if (!m_inPaint) {  // changes made while painting are already in this frame
        m_frameClock->request();
    }
}

SketchView::~SketchView() = default;

// ---------------------------------------------------------------------------------------------
// Transform
// ---------------------------------------------------------------------------------------------

QPointF SketchView::toScreen(core::Vec2 world) const
{
    return {m_pan.x() + world.x * m_zoom, m_pan.y() - world.y * m_zoom};
}

core::Vec2 SketchView::toWorld(QPointF screen) const
{
    return {(screen.x() - m_pan.x()) / m_zoom, (m_pan.y() - screen.y()) / m_zoom};
}

void SketchView::resetView()
{
    m_zoom = 4.0;
    m_pan = QPointF(width() / 2.0, height() / 2.0);
    m_autoView = AutoView::Reset;
    scheduleRepaint();
}

// ---------------------------------------------------------------------------------------------
// Tools
// ---------------------------------------------------------------------------------------------

void SketchView::setTool(ToolKind kind)
{
    if (m_tool) {
        m_tool->cancel();
        m_tool.reset();  // before the new one starts (a command tidies up as it goes)
    }
    if (kind != ToolKind::Select && kind != ToolKind::StrokeBrush && kind != ToolKind::Fill) {
        setPresentation(false);  // editing geometry: see it (and its constraints) properly
    }
    showOptionsPanel(kind);
    switch (kind) {
        case ToolKind::Select: m_tool = std::make_unique<SelectTool>(*this); break;
        case ToolKind::Line: m_tool = std::make_unique<LineTool>(*this); break;
        case ToolKind::Rectangle: m_tool = std::make_unique<RectangleTool>(*this); break;
        case ToolKind::Circle: m_tool = std::make_unique<CircleTool>(*this); break;
        case ToolKind::ArcCenter: m_tool = std::make_unique<CenterArcTool>(*this); break;
        case ToolKind::Arc3Point: m_tool = std::make_unique<ThreePointArcTool>(*this); break;
        case ToolKind::Point: m_tool = std::make_unique<PointTool>(*this); break;
        case ToolKind::Rectangle3Point: m_tool = std::make_unique<Rectangle3Tool>(*this); break;
        case ToolKind::RectangleCenter: m_tool = std::make_unique<RectangleCenterTool>(*this); break;
        case ToolKind::Circle2Point: m_tool = std::make_unique<Circle2Tool>(*this); break;
        case ToolKind::Circle3Point: m_tool = std::make_unique<Circle3Tool>(*this); break;
        case ToolKind::CircleTangent2: m_tool = std::make_unique<CircleTangentTool>(*this, 2); break;
        case ToolKind::CircleTangent3: m_tool = std::make_unique<CircleTangentTool>(*this, 3); break;
        case ToolKind::ArcTangent: m_tool = std::make_unique<ArcTangentTool>(*this); break;
        case ToolKind::PolygonInscribed:
            m_tool = std::make_unique<PolygonTool>(*this, core::build::PolygonKind::Inscribed);
            break;
        case ToolKind::PolygonCircumscribed:
            m_tool = std::make_unique<PolygonTool>(*this, core::build::PolygonKind::Circumscribed);
            break;
        case ToolKind::PolygonEdge: m_tool = std::make_unique<PolygonEdgeTool>(*this); break;
        case ToolKind::SlotCenterToCenter:
            m_tool = std::make_unique<SlotTool>(*this, SlotTool::Kind::CenterToCenter);
            break;
        case ToolKind::SlotOverall: m_tool = std::make_unique<SlotTool>(*this, SlotTool::Kind::Overall); break;
        case ToolKind::SlotCenterPoint: m_tool = std::make_unique<SlotTool>(*this, SlotTool::Kind::CenterPoint); break;
        case ToolKind::SlotArc3Point: m_tool = std::make_unique<ArcSlotTool>(*this, true); break;
        case ToolKind::SlotArcCenter: m_tool = std::make_unique<ArcSlotTool>(*this, false); break;
        case ToolKind::Trim: m_tool = std::make_unique<CurveOpTool>(*this, CurveOpTool::Op::Trim); break;
        case ToolKind::TrimToConstruction:
            m_tool = std::make_unique<CurveOpTool>(*this, CurveOpTool::Op::TrimToConstruction);
            break;
        case ToolKind::Extend: m_tool = std::make_unique<CurveOpTool>(*this, CurveOpTool::Op::Extend); break;
        case ToolKind::Break: m_tool = std::make_unique<CurveOpTool>(*this, CurveOpTool::Op::Break); break;
        case ToolKind::Fillet: m_tool = std::make_unique<CornerTool>(*this, CornerTool::Kind::Fillet); break;
        case ToolKind::ChamferEqual: m_tool = std::make_unique<CornerTool>(*this, CornerTool::Kind::ChamferEqual); break;
        case ToolKind::ChamferTwoDistances:
            m_tool = std::make_unique<CornerTool>(*this, CornerTool::Kind::ChamferTwoDistances);
            break;
        case ToolKind::ChamferDistanceAngle:
            m_tool = std::make_unique<CornerTool>(*this, CornerTool::Kind::ChamferDistanceAngle);
            break;
        case ToolKind::Move: m_tool = std::make_unique<MoveTool>(*this); break;
        case ToolKind::Scale: m_tool = std::make_unique<ScaleTool>(*this); break;
        case ToolKind::Mirror: m_tool = std::make_unique<MirrorTool>(*this); break;
        case ToolKind::RectangularPattern:
            m_tool = std::make_unique<RectangularPatternTool>(*this, std::exchange(m_editFeature, core::kNoId));
            break;
        case ToolKind::CircularPattern:
            m_tool = std::make_unique<CircularPatternTool>(*this, std::exchange(m_editFeature, core::kNoId));
            break;
        case ToolKind::Offset: m_tool = std::make_unique<OffsetTool>(*this); break;
        case ToolKind::Divide:
            m_tool = std::make_unique<DivideTool>(*this, std::exchange(m_editFeature, core::kNoId));
            break;
        case ToolKind::Paste: m_tool = std::make_unique<PasteTool>(*this, m_pasteFragment); break;
        case ToolKind::StrokeBrush: m_tool = std::make_unique<StrokeBrushTool>(*this); break;
        case ToolKind::Fill: m_tool = std::make_unique<FillTool>(*this); break;
        case ToolKind::Dimension: m_tool = std::make_unique<DimensionTool>(*this); break;
        default: m_tool = std::make_unique<ConstraintTool>(*this, kind); break;
    }
    m_toolKind = kind;
    setCursor(kind == ToolKind::Select ? Qt::ArrowCursor : Qt::CrossCursor);
    setHovered(core::kNoId);
    emit toolChanged(kind);
    m_tool->activate();
    refreshTool();
}

void SketchView::showOptionsPanel(ToolKind kind)
{
    if (m_optionsPanel) {
        m_optionsPanel->hide();
        m_optionsPanel->deleteLater();
        m_optionsPanel = nullptr;
    }
    QString title;
    switch (kind) {
        case ToolKind::Fillet: title = tr("Fillet"); break;
        case ToolKind::ChamferEqual:
        case ToolKind::ChamferTwoDistances:
        case ToolKind::ChamferDistanceAngle: title = tr("Chamfer"); break;
        case ToolKind::PolygonInscribed:
        case ToolKind::PolygonCircumscribed:
        case ToolKind::PolygonEdge: title = tr("Polygon"); break;
        default: return;  // no settings, or a command with its own panel
    }
    auto* panel = new CommandPanel(title, this);
    m_optionsPanel = panel;
    auto number = [&](const QString& label, const char* name, double* field, double max, const QString& suffix) {
        QDoubleSpinBox* box = panel->addNumber(label, name, *field, 0.001, max, suffix);
        connect(box, &QDoubleSpinBox::valueChanged, this, [this, field](double v) {
            *field = v;
            refreshTool();
        });
    };
    ModifyOptions& o = m_modifyOptions;
    const QString mm = QStringLiteral(" mm"), degrees = QStringLiteral(" °");
    switch (kind) {
        case ToolKind::Fillet: number(tr("Radius"), "radius", &o.filletRadius, 1e6, mm); break;
        case ToolKind::ChamferEqual: number(tr("Distance"), "distance", &o.chamferDistance, 1e6, mm); break;
        case ToolKind::ChamferTwoDistances:
            number(tr("Distance 1"), "distance", &o.chamferDistance, 1e6, mm);
            number(tr("Distance 2"), "distance2", &o.chamferDistance2, 1e6, mm);
            break;
        case ToolKind::ChamferDistanceAngle:
            number(tr("Distance"), "distance", &o.chamferDistance, 1e6, mm);
            number(tr("Angle"), "angle", &o.chamferAngle, 179.0, degrees);
            break;
        default: {
            QSpinBox* sides = panel->addCount(tr("Sides"), "sides", m_polygonSides, 3, 64);
            connect(sides, &QSpinBox::valueChanged, this, &SketchView::setPolygonSides);
            connect(this, &SketchView::polygonSidesChanged, sides, [sides](int n) {
                const QSignalBlocker block(sides);  // typed on the canvas: just mirror it
                sides->setValue(n);
            });
            break;
        }
    }
    // Keys typed in the panel go to it; Enter / Esc hand the keyboard back to the canvas.
    connect(panel, &CommandPanel::accepted, this, [this] { setFocus(); });
    connect(panel, &CommandPanel::rejected, this, [this] { setFocus(); });
}

void SketchView::setPresentation(bool on)
{
    if (on == m_presentation) {
        return;
    }
    m_presentation = on;
    setHovered(core::kNoId);
    scheduleRepaint();
    emit presentationChanged(on);
}

void SketchView::setPaint(const Paint& paint)
{
    if (paint == m_paint) {
        return;
    }
    m_paint = paint;
    refreshTool();  // e.g. the stroke brush's preview
    emit paintChanged();
}

QPainterPath SketchView::regionPath(const core::regions::Region& region) const
{
    QPainterPath path;
    path.setFillRule(Qt::OddEvenFill);  // holes cut out
    auto loop = [&](const std::vector<core::Vec2>& points) {
        QPolygonF poly;
        for (const core::Vec2& p : points) {
            poly << toScreen(p);
        }
        path.addPolygon(poly);
        path.closeSubpath();
    };
    loop(region.outer);
    for (const auto& hole : region.holes) {
        loop(hole);
    }
    return path;
}

QPen SketchView::strokePen(const core::StrokeStyle& style) const
{
    const core::Color& c = style.color;
    // Width is on the drawing, so it zooms; never thinner than a pixel. 0 is a 1-pixel hairline.
    const double width = style.width > 0.0 ? std::max(style.width * m_zoom, 1.0) : 1.0;
    QPen pen(QColor(c.r, c.g, c.b, c.a), width, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin);
    if (style.dash != core::Dash::Solid) {
        pen.setCapStyle(Qt::FlatCap);
        // In multiples of the width, so thicker lines get longer dashes.
        switch (style.dash) {
            case core::Dash::Dashed: pen.setDashPattern({5, 3}); break;
            case core::Dash::Dotted: pen.setDashPattern({1, 2}); break;
            case core::Dash::DashDot: pen.setDashPattern({8, 3, 1, 3}); break;
            case core::Dash::DashDotDot: pen.setDashPattern({8, 3, 1, 3, 1, 3}); break;
            case core::Dash::Solid: break;
        }
    }
    return pen;
}

void SketchView::setPasteFragment(core::Sketch fragment)
{
    m_pasteFragment = std::move(fragment);
}

QString SketchView::hint() const
{
    return m_tool ? m_tool->hint() : QString();
}

void SketchView::refreshTool()
{
    scheduleRepaint();
    emit hintChanged(hint());
}

// ---------------------------------------------------------------------------------------------
// Picking and selection
// ---------------------------------------------------------------------------------------------

core::Id SketchView::pickAt(QPointF screen, bool annotations) const
{
    if (annotations && !m_presentation) {  // (not drawn in presentation view)
        // Topmost (last drawn) first.
        const auto items = layoutAnnotations();
        for (auto it = items.rbegin(); it != items.rend(); ++it) {
            if (it->rect.adjusted(-2, -2, 2, 2).contains(screen)) {
                return it->id;
            }
        }
    }
    core::PickOptions options;
    options.tolerance = kPickPixels / m_zoom;
    if (m_presentation) {
        // Only what's shown can be picked: no points, no construction (axes included).
        options.points = false;
        for (const auto& [id, e] : m_doc.sketch().entities()) {
            if (e.construction) {
                options.exclude.insert(id);
            }
        }
    }
    return core::pick(m_doc.sketch(), toWorld(screen), options);
}

Snap SketchView::snap(QPointF screen) const
{
    const core::Sketch& sketch = m_doc.sketch();
    const core::Vec2 world = toWorld(screen);

    core::PickOptions options;
    options.tolerance = kSnapPixels / m_zoom;
    options.curves = false;
    if (const core::Id id = core::pick(sketch, world, options); id != core::kNoId) {
        const core::Point& p = sketch.point(id);
        return Snap{{p.x, p.y}, id};
    }

    // Line midpoints.
    for (const auto& [id, e] : sketch.entities()) {
        if (const auto* l = std::get_if<core::Line>(&e.geom); l && !sketch.isAxis(id)) {
            const core::Point& a = sketch.point(l->p1);
            const core::Point& b = sketch.point(l->p2);
            const core::Vec2 m{(a.x + b.x) / 2, (a.y + b.y) / 2};
            if (std::hypot(m.x - world.x, m.y - world.y) * m_zoom <= kSnapPixels) {
                Snap s{m};
                s.midpointOf = id;
                return s;
            }
        }
    }

    // Onto a curve.
    options.tolerance = kPickPixels / m_zoom;
    options.points = false;
    options.curves = true;
    if (const core::Id id = core::pick(sketch, world, options); id != core::kNoId) {
        Snap s{core::closestPoint(sketch, id, world)};
        s.onCurve = id;
        return s;
    }
    return Snap{world};
}

void SketchView::setSelection(std::set<core::Id> ids)
{
    if (ids == m_selection) {
        return;
    }
    m_selection = std::move(ids);
    scheduleRepaint();
    emit selectionChanged();
}

void SketchView::setHovered(core::Id id)
{
    if (id != m_hovered) {
        m_hovered = id;
        scheduleRepaint();
    }
}

void SketchView::selectAll()
{
    std::set<core::Id> all;
    for (const auto& [id, e] : m_doc.sketch().entities()) {
        if (!m_doc.sketch().isReference(id)) {
            all.insert(id);
        }
    }
    setSelection(std::move(all));
}

void SketchView::deleteSelection()
{
    std::set<core::Id> doomed = m_selection;
    std::erase_if(doomed, [&](core::Id id) { return m_doc.sketch().isReference(id); });  // permanent
    if (doomed.empty()) {
        return;
    }
    tryEdit(tr("Delete"), [&](Editor& ed) {
        for (core::Id id : doomed) {
            if (ed.sketch().hasEntity(id) || ed.sketch().hasConstraint(id)) {
                ed.remove(id);  // may already be gone via an earlier cascade
            }
        }
    });
}

void SketchView::fitAll()
{
    const core::Sketch& s = m_doc.sketch();
    double minX = 0, minY = 0, maxX = 0, maxY = 0;  // always include the origin
    auto include = [&](core::Vec2 p) {
        minX = std::min(minX, p.x);
        minY = std::min(minY, p.y);
        maxX = std::max(maxX, p.x);
        maxY = std::max(maxY, p.y);
    };
    for (const auto& [id, e] : s.entities()) {
        if (s.isReference(id)) {
            continue;
        }
        for (const core::Vec2& p : core::sample(s, id, 32)) {
            include(p);
        }
    }
    for (const auto& [id, c] : s.constraints()) {
        if (core::isDimension(c.type)) {
            if (const core::DimensionLayout L = core::layoutDimension(s, c); L.valid) {
                include(L.label);  // dimensions often sit outside the geometry
            }
        }
    }
    const double w = maxX - minX, h = maxY - minY;
    if (w < 1e-9 && h < 1e-9) {
        resetView();
        return;
    }
    const double margin = 0.8;  // leave room for label text and symbols
    m_zoom = std::clamp(std::min(width() * margin / std::max(w, 1e-9), height() * margin / std::max(h, 1e-9)),
                        kMinZoom, kMaxZoom);
    const core::Vec2 centre{(minX + maxX) / 2, (minY + maxY) / 2};
    m_pan = QPointF(width() / 2.0 - centre.x * m_zoom, height() / 2.0 + centre.y * m_zoom);
    m_autoView = AutoView::Fit;
    scheduleRepaint();
}

bool SketchView::tryEdit(const QString& label, const std::function<void(Editor&)>& edit)
{
    try {
        m_doc.edit(label, edit);
        return true;
    }
    catch (const core::SketchError& e) {
        emit message(QString::fromStdString(e.what()));
        return false;
    }
}

void SketchView::onDocumentChanged()
{
    // Drop references to entities that no longer exist (undo, delete cascades).
    const core::Sketch& s = m_doc.sketch();
    std::set<core::Id> kept;
    for (core::Id id : m_selection) {
        if (s.hasEntity(id) || s.hasConstraint(id)) {
            kept.insert(id);
        }
    }
    if (m_hovered != core::kNoId && !s.hasEntity(m_hovered) && !s.hasConstraint(m_hovered)) {
        m_hovered = core::kNoId;
    }
    setSelection(std::move(kept));
    scheduleRepaint();
}

// ---------------------------------------------------------------------------------------------
// Painting
// ---------------------------------------------------------------------------------------------

void SketchView::resizeEvent(QResizeEvent* event)
{
    QWidget::resizeEvent(event);
    if (m_autoView == AutoView::Reset) {
        resetView();
    }
    else if (m_autoView == AutoView::Fit) {
        fitAll();
    }
}

void SketchView::paintEvent(QPaintEvent*)
{
    const qint64 start = m_clock.nsecsElapsed();
    m_inPaint = true;
    if (m_tool) {
        m_tool->beforePaint();  // e.g. run this frame's drag solve with the latest mouse position
    }
    QPainter painter(this);
    painter.fillRect(rect(), theme::canvas().background);
    drawGrid(painter);
    painter.setRenderHint(QPainter::Antialiasing);
    drawSketch(painter);
    if (m_tool) {
        m_tool->paintOverlay(painter);
    }
    m_paintMs = (m_clock.nsecsElapsed() - start) / 1e6;
    if (m_showStats) {
        m_paintRate.tick(m_clock.elapsed());
        drawStats(painter);
    }
    m_inPaint = false;
}

void SketchView::setShowStats(bool show)
{
    m_showStats = show;
    m_paintRate.stamps.clear();
    m_inputRate.stamps.clear();
    scheduleRepaint();
}

void SketchView::RateCounter::tick(qint64 nowMs)
{
    stamps.push_back(nowMs);
    rate(nowMs);
}

int SketchView::RateCounter::rate(qint64 nowMs)
{
    while (!stamps.empty() && stamps.front() <= nowMs - 1000) {
        stamps.pop_front();
    }
    return static_cast<int>(stamps.size());
}

void SketchView::drawStats(QPainter& painter)
{
    const qint64 now = m_clock.elapsed();
    const QString text = QStringLiteral("paint %1 fps  %2 ms\ninput %3 Hz  %4 ms\nscreen %5 Hz")
                             .arg(m_paintRate.rate(now))
                             .arg(m_paintMs, 0, 'f', 2)
                             .arg(m_inputRate.rate(now))
                             .arg(m_inputMs, 0, 'f', 2)
                             .arg(screen() ? screen()->refreshRate() : 0.0, 0, 'f', 0);
    QFont mono(QStringLiteral("Consolas"));
    mono.setStyleHint(QFont::Monospace);
    painter.setFont(mono);
    const QRectF box = QRectF(8, 8, 220, 56);
    painter.setPen(Qt::NoPen);
    painter.setBrush(QColor(0, 0, 0, 170));
    painter.drawRoundedRect(box, 4, 4);
    painter.setBrush(Qt::NoBrush);
    painter.setPen(QColor(0x9f, 0xff, 0x9f));
    painter.drawText(box.adjusted(8, 4, -8, -4), Qt::AlignLeft | Qt::AlignVCenter, text);
}

void SketchView::drawGrid(QPainter& painter) const
{
    const auto& colors = theme::canvas();

    // Pick a minor spacing from the 1-2-5 series so lines stay at least kMinGridPixelSpacing apart.
    double spacing = 1.0;
    const double minWorld = kMinGridPixelSpacing / m_zoom;
    const double decade = std::pow(10.0, std::floor(std::log10(minWorld)));
    for (double factor : {1.0, 2.0, 5.0, 10.0}) {
        spacing = decade * factor;
        if (spacing >= minWorld) {
            break;
        }
    }
    const double majorSpacing = spacing * 5.0;

    const core::Vec2 topLeft = toWorld(QPointF(0, 0));
    const core::Vec2 bottomRight = toWorld(QPointF(width(), height()));

    auto drawLines = [&](double step, const QColor& color) {
        painter.setPen(QPen(color, 0));
        for (double x = std::floor(topLeft.x / step) * step; x <= bottomRight.x; x += step) {
            const double sx = toScreen({x, 0}).x();
            painter.drawLine(QPointF(sx, 0), QPointF(sx, height()));
        }
        for (double y = std::floor(bottomRight.y / step) * step; y <= topLeft.y; y += step) {
            const double sy = toScreen({0, y}).y();
            painter.drawLine(QPointF(0, sy), QPointF(width(), sy));
        }
    };

    drawLines(spacing, colors.gridMinor);
    drawLines(majorSpacing, colors.gridMajor);

    if (m_presentation) {
        return;  // the axes are construction: not part of the drawing
    }
    const QPointF origin = toScreen({0, 0});
    painter.setPen(QPen(colors.axisX, 1.0));
    painter.drawLine(QPointF(0, origin.y()), QPointF(width(), origin.y()));
    painter.setPen(QPen(colors.axisY, 1.0));
    painter.drawLine(QPointF(origin.x(), 0), QPointF(origin.x(), height()));
}

void SketchView::drawArcPath(QPainter& painter, const core::ArcShape& arc) const
{
    const QPointF c = toScreen(arc.center);
    const double r = arc.radius * m_zoom;
    const QRectF box(c.x() - r, c.y() - r, 2 * r, 2 * r);
    // Qt angles are counter-clockwise on screen, which matches our y-up world.
    QPainterPath path;
    path.arcMoveTo(box, arc.startAngle * kRadToDeg);
    path.arcTo(box, arc.startAngle * kRadToDeg, arc.sweep * kRadToDeg);
    painter.drawPath(path);
}

void SketchView::drawSketch(QPainter& painter) const
{
    const auto& colors = theme::canvas();
    const core::Sketch& sketch = m_doc.sketch();
    const core::SolveResult& result = m_doc.solveResult();

    std::set<core::Id> conflicted;
    for (core::Id cid : result.conflicting) {
        if (sketch.hasConstraint(cid)) {
            for (core::Id r : sketch.constraint(cid).refs) {
                conflicted.insert(r);
            }
        }
    }

    // What the selected constraints and dimensions act on, shown bright pink. Constraints that
    // chain (equal, parallel, concentric, collinear) show the whole chain: three lines made equal
    // by two constraints are all equal, and the symbol on them stands for both.
    std::set<core::Id> constrained;
    auto chains = [](core::ConstraintType t) {
        return t == core::ConstraintType::Equal || t == core::ConstraintType::Parallel
            || t == core::ConstraintType::Concentric || t == core::ConstraintType::Collinear;
    };
    for (core::Id id : m_selection) {
        if (!sketch.hasConstraint(id)) {
            continue;
        }
        const core::Constraint& c = sketch.constraint(id);
        constrained.insert(c.refs.begin(), c.refs.end());
        if (!chains(c.type)) {
            continue;
        }
        std::set<core::Id> group(c.refs.begin(), c.refs.end());
        for (bool grew = true; grew;) {
            grew = false;
            for (const auto& [oid, other] : sketch.constraints()) {
                if (other.type == c.type && std::ranges::any_of(other.refs, [&](core::Id r) { return group.contains(r); })) {
                    for (core::Id r : other.refs) {
                        grew |= group.insert(r).second;
                    }
                }
            }
        }
        constrained.insert(group.begin(), group.end());
    }

    auto styleFor = [&](const core::Entity& e) {
        QColor color = result.fullyConstrained.contains(e.id) ? colors.fullyConstrained : colors.underConstrained;
        if (e.construction) {
            color = colors.construction;
        }
        if (conflicted.contains(e.id)) {
            color = colors.conflict;
        }
        double width = kCurveWidth;
        if (constrained.contains(e.id)) {
            color = kConstrainedPink;
            width = kHighlightWidth;
        }
        else if (m_selection.contains(e.id)) {
            color = colors.selection;
            width = kHighlightWidth;
        }
        else if (e.id == m_hovered) {
            color = colors.hover;
            width = kHighlightWidth;
        }
        QPen pen(color, width, e.construction ? Qt::DashLine : Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin);
        return pen;
    };

    // The axes are drawn with the grid; here they only light up when hovered or selected.
    for (core::Id axis : {sketch.xAxis(), sketch.yAxis()}) {
        const bool pink = constrained.contains(axis);
        if (axis == core::kNoId || m_presentation || !(pink || m_selection.contains(axis) || axis == m_hovered)) {
            continue;
        }
        painter.setPen(QPen(pink ? kConstrainedPink : m_selection.contains(axis) ? colors.selection : colors.hover,
                            kHighlightWidth));
        const QPointF o = toScreen({0, 0});
        if (axis == sketch.xAxis()) {
            painter.drawLine(QPointF(0, o.y()), QPointF(width(), o.y()));
        }
        else {
            painter.drawLine(QPointF(o.x(), 0), QPointF(o.x(), height()));
        }
    }

    // Draws a curve with whatever pen is set.
    auto drawCurve = [&](core::Id id, const core::Entity& e) {
        switch (e.kind()) {
            case core::EntityKind::Point: break;
            case core::EntityKind::Line: {
                const auto& l = std::get<core::Line>(e.geom);
                const core::Point& a = sketch.point(l.p1);
                const core::Point& b = sketch.point(l.p2);
                painter.drawLine(toScreen({a.x, a.y}), toScreen({b.x, b.y}));
                break;
            }
            case core::EntityKind::Circle: {
                const auto& c = std::get<core::Circle>(e.geom);
                const core::Point& centre = sketch.point(c.center);
                const double r = c.radius * m_zoom;
                painter.drawEllipse(toScreen({centre.x, centre.y}), r, r);
                break;
            }
            case core::EntityKind::Arc: drawArcPath(painter, core::arcShape(sketch, id)); break;
        }
    };

    // Fills, underneath everything: each found afresh from the geometry (see core/Regions.h).
    if (!sketch.fills().empty()) {
        const core::regions::Arrangement arrangement(sketch);
        for (const auto& [id, f] : sketch.fills()) {
            const auto region = core::regions::resolve(arrangement, f);
            if (!region) {
                continue;  // its outline is open: nothing to fill until it's closed again
            }
            QColor color(f.color.r, f.color.g, f.color.b, f.color.a);
            if (!m_presentation) {
                color.setAlphaF(color.alphaF() * 0.35);
            }
            painter.fillPath(regionPath(*region), color);
        }
    }

    // Strokes: the drawing's look. Faded under the geometry while sketching; in presentation
    // view they replace the geometry they decorate. (Construction geometry is never decorated.)
    painter.setBrush(Qt::NoBrush);
    std::set<core::Id> decorated;
    for (const auto& [id, style] : sketch.strokes()) {
        if (!sketch.hasEntity(id) || sketch.entity(id).construction) {
            continue;
        }
        QPen pen = strokePen(style);
        if (!m_presentation) {
            QColor faded = pen.color();
            faded.setAlphaF(faded.alphaF() * 0.3);
            pen.setColor(faded);
        }
        painter.setPen(pen);
        drawCurve(id, sketch.entity(id));
        decorated.insert(id);
    }

    // Curves first, points on top.
    for (const auto& [id, e] : sketch.entities()) {
        if (sketch.isAxis(id) || e.kind() == core::EntityKind::Point) {
            continue;
        }
        if (m_presentation && e.construction) {
            continue;  // reference geometry isn't part of the drawing
        }
        if (m_presentation && decorated.contains(id)) {
            if (!m_selection.contains(id) && id != m_hovered && !constrained.contains(id)) {
                continue;  // its stroke is what shows
            }
            // Selected or hovered: a thin highlight over the stroke.
            QPen pen = styleFor(e);
            pen.setWidthF(1.0);
            painter.setPen(pen);
            drawCurve(id, e);
            continue;
        }
        painter.setPen(styleFor(e));
        drawCurve(id, e);
    }

    if (m_presentation) {
        return;  // no constraint symbols, dimensions or points: just the drawing
    }
    drawAnnotations(painter);

    for (const auto& [id, e] : sketch.entities()) {
        if (e.kind() != core::EntityKind::Point || sketch.isHiddenReference(id)) {
            continue;
        }
        const core::Point& p = std::get<core::Point>(e.geom);
        QPen pen = styleFor(e);
        const bool highlighted = m_selection.contains(id) || id == m_hovered || constrained.contains(id);
        const double r = highlighted ? kPointRadius + 1.2 : kPointRadius;
        painter.setPen(Qt::NoPen);
        painter.setBrush(pen.color());
        painter.drawEllipse(toScreen({p.x, p.y}), r, r);
        if (sketch.isOrigin(id)) {
            // The origin: a ring around the dot.
            painter.setBrush(Qt::NoBrush);
            painter.setPen(QPen(pen.color(), 1.5));
            painter.drawEllipse(toScreen({p.x, p.y}), 6.0, 6.0);
        }
        else if (e.fixed) {
            // Fixed points get a small square outline, a stand-in for Fusion's lock glyph.
            painter.setBrush(Qt::NoBrush);
            painter.setPen(QPen(pen.color(), 1.0));
            const QPointF s = toScreen({p.x, p.y});
            painter.drawRect(QRectF(s.x() - 5, s.y() - 5, 10, 10));
        }
    }
    painter.setBrush(Qt::NoBrush);
}

void SketchView::drawPreviewLine(QPainter& painter, core::Vec2 a, core::Vec2 b) const
{
    painter.setPen(QPen(theme::canvas().underConstrained, kCurveWidth, Qt::DashLine));
    painter.drawLine(toScreen(a), toScreen(b));
}

void SketchView::drawPreviewCircle(QPainter& painter, core::Vec2 center, double radius) const
{
    painter.setPen(QPen(theme::canvas().underConstrained, kCurveWidth, Qt::DashLine));
    painter.setBrush(Qt::NoBrush);
    painter.drawEllipse(toScreen(center), radius * m_zoom, radius * m_zoom);
}

void SketchView::drawPreviewArc(QPainter& painter, const core::ArcShape& arc) const
{
    painter.setPen(QPen(theme::canvas().underConstrained, kCurveWidth, Qt::DashLine));
    painter.setBrush(Qt::NoBrush);
    drawArcPath(painter, arc);
}

void SketchView::setShowConstraints(bool show)
{
    m_showConstraints = show;
    scheduleRepaint();
}

void SketchView::setPolygonSides(int sides)
{
    const int clamped = std::clamp(sides, 3, 64);
    if (clamped != m_polygonSides) {
        m_polygonSides = clamped;
        emit polygonSidesChanged(clamped);
    }
    refreshTool();
}

void SketchView::drawPreviewSketch(QPainter& painter, const core::Sketch& sketch, core::Id after) const
{
    drawPreviewSketch(painter, sketch, [after](core::Id id) { return id > after; });
}

void SketchView::drawPreviewSketch(QPainter& painter, const core::Sketch& sketch,
                                   const std::function<bool(core::Id)>& include) const
{
    const QColor color = theme::canvas().underConstrained;
    QColor faint = theme::canvas().construction;
    painter.setBrush(Qt::NoBrush);
    for (const auto& [id, e] : sketch.entities()) {
        if (!include(id)) {
            continue;
        }
        painter.setPen(e.construction ? QPen(faint, 1.0, Qt::DashLine) : QPen(color, kCurveWidth, Qt::DashLine));
        switch (e.kind()) {
            case core::EntityKind::Point: {
                const core::Point& p = sketch.point(e.id);
                painter.setPen(Qt::NoPen);
                painter.setBrush(color);
                painter.drawEllipse(toScreen({p.x, p.y}), 2.0, 2.0);
                painter.setBrush(Qt::NoBrush);
                break;
            }
            case core::EntityKind::Line: {
                const core::Line& l = sketch.line(e.id);
                const core::Point& a = sketch.point(l.p1);
                const core::Point& b = sketch.point(l.p2);
                painter.drawLine(toScreen({a.x, a.y}), toScreen({b.x, b.y}));
                break;
            }
            case core::EntityKind::Circle: {
                const core::Circle& c = sketch.circle(e.id);
                const core::Point& centre = sketch.point(c.center);
                painter.drawEllipse(toScreen({centre.x, centre.y}), c.radius * m_zoom, c.radius * m_zoom);
                break;
            }
            case core::EntityKind::Arc: drawArcPath(painter, core::arcShape(sketch, e.id)); break;
        }
    }
}

void SketchView::drawSnapMarker(QPainter& painter, const Snap& snap) const
{
    const QPointF p = toScreen(snap.pos);
    painter.setPen(QPen(theme::canvas().selection, 1.5));
    painter.setBrush(Qt::NoBrush);
    if (snap.point != core::kNoId) {
        painter.drawEllipse(p, 7.0, 7.0);  // existing point
    }
    else if (snap.midpointOf != core::kNoId) {
        const QPolygonF triangle{p + QPointF(0, -7), p + QPointF(6, 5), p + QPointF(-6, 5), p + QPointF(0, -7)};
        painter.drawPolyline(triangle);  // midpoint
    }
    else if (snap.onCurve != core::kNoId) {
        painter.drawLine(p + QPointF(-5, -5), p + QPointF(5, 5));  // on the curve
        painter.drawLine(p + QPointF(-5, 5), p + QPointF(5, -5));
    }
}

void SketchView::drawLabel(QPainter& painter, core::Vec2 at, const QString& text) const
{
    const QPointF p = toScreen(at) + QPointF(14, -10);
    const QFontMetricsF fm(font());
    QRectF box = fm.boundingRect(text);
    box.moveBottomLeft(p);
    box.adjust(-4, -2, 4, 2);
    QColor bg = theme::canvas().background;
    bg.setAlpha(210);
    painter.setPen(Qt::NoPen);
    painter.setBrush(bg);
    painter.drawRoundedRect(box, 3, 3);
    painter.setBrush(Qt::NoBrush);
    painter.setPen(theme::canvas().dimension);
    painter.drawText(box, Qt::AlignCenter, text);
}

// ---------------------------------------------------------------------------------------------
// Input
// ---------------------------------------------------------------------------------------------

ToolEvent SketchView::makeEvent(const QMouseEvent* event) const
{
    return ToolEvent{event->position(), toWorld(event->position()), event->button(), event->buttons(),
                     event->modifiers()};
}

void SketchView::wheelEvent(QWheelEvent* event)
{
    const double notches = event->angleDelta().y() / 120.0;
    if (notches == 0.0) {
        return;
    }
    // Zoom about the cursor: keep the world point under the mouse fixed on screen.
    const QPointF mouse = event->position();
    const core::Vec2 anchor = toWorld(mouse);
    m_zoom = std::clamp(m_zoom * std::pow(kZoomStep, notches), kMinZoom, kMaxZoom);
    m_autoView = AutoView::None;  // the user has taken over the view
    m_pan = QPointF(mouse.x() - anchor.x * m_zoom, mouse.y() + anchor.y * m_zoom);
    scheduleRepaint();
    event->accept();
}

void SketchView::mousePressEvent(QMouseEvent* event)
{
    if (event->button() == Qt::MiddleButton) {
        m_panning = true;
        m_lastMouse = event->position();
        setCursor(Qt::ClosedHandCursor);
        event->accept();
        return;
    }
    if (event->button() == Qt::RightButton) {
        // Right-click backs out of whatever the tool is in the middle of; with nothing to back
        // out of, it offers the recently used tools at the cursor.
        if (!m_tool->cancel()) {
            emit recentToolsRequested(event->globalPosition().toPoint());
        }
        event->accept();
        return;
    }
    m_tool->press(makeEvent(event));
}

void SketchView::mouseMoveEvent(QMouseEvent* event)
{
    if (m_panning) {
        m_pan += event->position() - m_lastMouse;
        m_autoView = AutoView::None;
        m_lastMouse = event->position();
        scheduleRepaint();
    }
    else {
        const qint64 start = m_clock.nsecsElapsed();
        m_tool->move(makeEvent(event));
        m_inputMs = (m_clock.nsecsElapsed() - start) / 1e6;
    }
    if (m_showStats) {
        m_inputRate.tick(m_clock.elapsed());
        scheduleRepaint();
    }
    const core::Vec2 w = toWorld(event->position());
    emit cursorMoved(QPointF(w.x, w.y));
}

void SketchView::mouseReleaseEvent(QMouseEvent* event)
{
    if (event->button() == Qt::MiddleButton && m_panning) {
        m_panning = false;
        setCursor(m_toolKind == ToolKind::Select ? Qt::ArrowCursor : Qt::CrossCursor);
        event->accept();
        return;
    }
    m_tool->release(makeEvent(event));
}

void SketchView::mouseDoubleClickEvent(QMouseEvent* event)
{
    if (event->button() == Qt::LeftButton) {
        m_tool->doubleClick(makeEvent(event));
    }
}

void SketchView::keyPressEvent(QKeyEvent* event)
{
    if (event->key() != Qt::Key_Escape && m_tool->keyPress(event->key(), event->text())) {
        return;
    }
    switch (event->key()) {
        case Qt::Key_Escape:
            if (!m_tool->cancel()) {
                if (m_toolKind != ToolKind::Select) {
                    setTool(ToolKind::Select);
                }
                else {
                    setSelection({});
                }
            }
            return;
        case Qt::Key_Delete:
        case Qt::Key_Backspace:
            deleteSelection();
            return;
        default: QWidget::keyPressEvent(event);
    }
}

void SketchView::leaveEvent(QEvent* event)
{
    setHovered(core::kNoId);
    QWidget::leaveEvent(event);
}
