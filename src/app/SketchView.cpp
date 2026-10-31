#include "SketchView.h"

#include "Document.h"
#include "FrameClock.h"
#include "Theme.h"
#include "tools/SelectTool.h"

#include <QKeyEvent>
#include <QMouseEvent>
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
    scheduleRepaint();
}

// ---------------------------------------------------------------------------------------------
// Tools
// ---------------------------------------------------------------------------------------------

void SketchView::setTool(ToolKind kind)
{
    if (m_tool) {
        m_tool->cancel();
    }
    switch (kind) {
        case ToolKind::Select: m_tool = std::make_unique<SelectTool>(*this); break;
        case ToolKind::Line: m_tool = std::make_unique<LineTool>(*this); break;
        case ToolKind::Rectangle: m_tool = std::make_unique<RectangleTool>(*this); break;
        case ToolKind::Circle: m_tool = std::make_unique<CircleTool>(*this); break;
        case ToolKind::ArcCenter: m_tool = std::make_unique<CenterArcTool>(*this); break;
        case ToolKind::Arc3Point: m_tool = std::make_unique<ThreePointArcTool>(*this); break;
    }
    m_toolKind = kind;
    setCursor(kind == ToolKind::Select ? Qt::ArrowCursor : Qt::CrossCursor);
    setHovered(core::kNoId);
    emit toolChanged(kind);
    refreshTool();
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

core::Id SketchView::pickAt(QPointF screen) const
{
    core::PickOptions options;
    options.tolerance = kPickPixels / m_zoom;
    return core::pick(m_doc.sketch(), toWorld(screen), options);
}

Snap SketchView::snap(QPointF screen) const
{
    core::PickOptions options;
    options.tolerance = kSnapPixels / m_zoom;
    options.curves = false;
    const core::Id id = core::pick(m_doc.sketch(), toWorld(screen), options);
    if (id != core::kNoId) {
        const core::Point& p = m_doc.sketch().point(id);
        return {{p.x, p.y}, id};
    }
    return {toWorld(screen), core::kNoId};
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
        all.insert(id);
    }
    setSelection(std::move(all));
}

void SketchView::deleteSelection()
{
    if (m_selection.empty()) {
        return;
    }
    const std::set<core::Id> doomed = m_selection;
    tryModify(tr("Delete"), [&](core::Sketch& s) {
        for (core::Id id : doomed) {
            if (s.hasEntity(id) || s.hasConstraint(id)) {
                s.remove(id);  // may already be gone via an earlier cascade
            }
        }
    });
}

bool SketchView::tryModify(const QString& label, const std::function<void(core::Sketch&)>& edit)
{
    try {
        m_doc.modify(label, edit);
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
    if (m_hovered != core::kNoId && !s.hasEntity(m_hovered)) {
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
    if (!m_initialised) {
        resetView();
        m_initialised = true;
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

    auto styleFor = [&](const core::Entity& e) {
        QColor color = result.fullyConstrained.contains(e.id) ? colors.fullyConstrained : colors.underConstrained;
        if (e.construction) {
            color = colors.construction;
        }
        if (conflicted.contains(e.id)) {
            color = colors.conflict;
        }
        double width = kCurveWidth;
        if (m_selection.contains(e.id)) {
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

    // Curves first, points on top.
    painter.setBrush(Qt::NoBrush);
    for (const auto& [id, e] : sketch.entities()) {
        switch (e.kind()) {
            case core::EntityKind::Point: break;
            case core::EntityKind::Line: {
                const auto& l = std::get<core::Line>(e.geom);
                const core::Point& a = sketch.point(l.p1);
                const core::Point& b = sketch.point(l.p2);
                painter.setPen(styleFor(e));
                painter.drawLine(toScreen({a.x, a.y}), toScreen({b.x, b.y}));
                break;
            }
            case core::EntityKind::Circle: {
                const auto& c = std::get<core::Circle>(e.geom);
                const core::Point& centre = sketch.point(c.center);
                const double r = c.radius * m_zoom;
                painter.setPen(styleFor(e));
                painter.drawEllipse(toScreen({centre.x, centre.y}), r, r);
                break;
            }
            case core::EntityKind::Arc:
                painter.setPen(styleFor(e));
                drawArcPath(painter, core::arcShape(sketch, id));
                break;
        }
    }

    for (const auto& [id, e] : sketch.entities()) {
        if (e.kind() != core::EntityKind::Point) {
            continue;
        }
        const core::Point& p = std::get<core::Point>(e.geom);
        QPen pen = styleFor(e);
        const bool highlighted = m_selection.contains(id) || id == m_hovered;
        const double r = highlighted ? kPointRadius + 1.2 : kPointRadius;
        painter.setPen(Qt::NoPen);
        painter.setBrush(pen.color());
        painter.drawEllipse(toScreen({p.x, p.y}), r, r);
        if (e.fixed) {
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

void SketchView::drawSnapMarker(QPainter& painter, const Snap& snap) const
{
    if (snap.point == core::kNoId) {
        return;
    }
    painter.setPen(QPen(theme::canvas().selection, 1.5));
    painter.setBrush(Qt::NoBrush);
    painter.drawEllipse(toScreen(snap.pos), 7.0, 7.0);
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
    m_tool->press(makeEvent(event));
}

void SketchView::mouseMoveEvent(QMouseEvent* event)
{
    if (m_panning) {
        m_pan += event->position() - m_lastMouse;
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
