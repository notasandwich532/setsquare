#include "SketchView.h"

#include <core/Curves.h>
#include <core/Features.h>

#include "CommandPanel.h"
#include "Document.h"
#include "FrameClock.h"
#include "Theme.h"
#include "tools/ConstraintTools.h"
#include "tools/ModifyTools.h"
#include "tools/SelectTool.h"
#include "tools/ShapeTools.h"
#include "tools/CurveTools.h"
#include "tools/ProjectTool.h"
#include "tools/GradientTool.h"
#include "tools/ImageTool.h"
#include "tools/StyleTools.h"
#include "tools/TextTool.h"
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
#include <array>
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
constexpr double kOtherSublayerAlpha = 0.3;  // geometry in sublayers other than the active one
constexpr double kOtherLayerAlpha = 0.2;     // sketch geometry of other layers (not referable)
const QColor kProjected(0xb0, 0x8c, 0xff);   // projections from other layers (linked)
const QColor kUnlinked(0x8c, 0x84, 0x9c);    // projections whose original is gone
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
    if (kind != m_toolKind && m_toolKind != ToolKind::Eyedropper) {
        m_previousTool = m_toolKind;  // (the eyedropper goes back to it)
    }
    if (m_tool) {
        m_tool->cancel();
        m_tool.reset();  // before the new one starts (a command tidies up as it goes)
    }
    if (kind != ToolKind::Select && kind != ToolKind::StrokeBrush && kind != ToolKind::Fill && kind != ToolKind::Gradient
        && kind != ToolKind::ResetFill && kind != ToolKind::Eyedropper) {
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
        case ToolKind::Text: m_tool = std::make_unique<TextTool>(*this, std::exchange(m_editFeature, core::kNoId)); break;
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
        case ToolKind::Gradient: m_tool = std::make_unique<GradientTool>(*this); break;
        case ToolKind::ResetFill: m_tool = std::make_unique<ResetFillTool>(*this); break;
        case ToolKind::Image: m_tool = std::make_unique<ImageTool>(*this); break;
        case ToolKind::Eyedropper: m_tool = std::make_unique<EyedropperTool>(*this); break;
        case ToolKind::Group: m_tool = std::make_unique<GroupTool>(*this); break;
        case ToolKind::Project: m_tool = std::make_unique<ProjectTool>(*this); break;
        case ToolKind::Ellipse: m_tool = std::make_unique<EllipseTool>(*this); break;
        case ToolKind::SplineFit: m_tool = std::make_unique<SplineTool>(*this, core::SplineKind::Fit); break;
        case ToolKind::SplineControl: m_tool = std::make_unique<SplineTool>(*this, core::SplineKind::Control); break;
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

bool SketchView::offerColor(const core::Color& color, bool second)
{
    return m_tool && m_tool->takeColor(color, second);
}

void SketchView::setGradient(const core::Gradient& gradient)
{
    if (gradient == m_gradient) {
        return;
    }
    m_gradient = gradient;
    refreshTool();
    emit gradientChanged();
}

void SketchView::editGradient(core::Id item)
{
    const core::Sketch& s = m_doc.sketch();
    const GradientTool::Target target = s.hasFill(item)   ? GradientTool::Target::Fill
                                        : s.hasText(item) ? GradientTool::Target::Text
                                                          : GradientTool::Target::Stroke;
    setTool(ToolKind::Gradient);
    gradientTool()->setTarget(target);
    gradientTool()->setCurrent(item);
}

void SketchView::editImage(core::Id image)
{
    setTool(ToolKind::Image);
    static_cast<ImageTool*>(m_tool.get())->setImage(image);
}

core::Id SketchView::imageAt(core::Vec2 world) const
{
    const core::Sketch& s = m_doc.sketch();
    core::Id found = core::kNoId;
    for (const auto& [id, stored] : s.images()) {  // later ones are on top
        const core::Image& im = m_imagePreview && m_imagePreview->id == id ? *m_imagePreview : stored;
        if (im.visible && im.layer == s.activeLayer() && s.layer(im.layer).visible && im.contains({world.x, world.y})) {
            found = id;
        }
    }
    return found;
}

GradientTool* SketchView::gradientTool() const
{
    return m_toolKind == ToolKind::Gradient ? static_cast<GradientTool*>(m_tool.get()) : nullptr;
}

QPainterPath SketchView::textPath(const core::Text& t) const
{
    QPainterPath path;
    path.setFillRule(Qt::WindingFill);
    const core::Sketch& s = m_doc.sketch();
    if (!s.hasEntity(t.anchor)) {
        return path;
    }
    const core::Point& anchor = s.point(t.anchor);
    for (const auto& contour : t.shape) {
        QPolygonF polygon;
        for (const core::Point& p : contour) {
            const core::Point w = core::textToWorld(anchor, t.angle, p);
            polygon << toScreen({w.x, w.y});
        }
        path.addPolygon(polygon);
        path.closeSubpath();
    }
    return path;
}

QPolygonF SketchView::textBox(const core::Text& t) const
{
    // Round its letters (in its own direction), or a small box at the anchor if it has none.
    double x0 = 0, y0 = 0, x1 = 0, y1 = 0;
    bool any = false;
    for (const auto& contour : t.shape) {
        for (const core::Point& p : contour) {
            x0 = any ? std::min(x0, p.x) : p.x;
            y0 = any ? std::min(y0, p.y) : p.y;
            x1 = any ? std::max(x1, p.x) : p.x;
            y1 = any ? std::max(y1, p.y) : p.y;
            any = true;
        }
    }
    if (!any) {
        x1 = t.height;
        y1 = t.height;
    }
    const core::Sketch& s = m_doc.sketch();
    const core::Point& anchor = s.point(t.anchor);
    QPolygonF box;
    for (const core::Point corner : {core::Point{x0, y0}, core::Point{x1, y0}, core::Point{x1, y1}, core::Point{x0, y1}}) {
        const core::Point w = core::textToWorld(anchor, t.angle, corner);
        box << toScreen({w.x, w.y});
    }
    return box;
}

core::Id SketchView::textAt(QPointF screen, bool editable) const
{
    const core::Sketch& s = m_doc.sketch();
    core::Id found = core::kNoId;
    for (const auto& [id, t] : s.texts()) {  // the topmost (last) wins
        if (!s.isShown(id) || (editable && t.sublayer != s.activeSublayer())
            || (!m_presentation && s.layerOf(id) != s.activeLayer())) {
            continue;
        }
        if (s.hasEntity(t.anchor) && textBox(t).containsPoint(screen, Qt::OddEvenFill)) {
            found = id;
        }
    }
    return found;
}

const std::map<core::Id, std::optional<core::regions::Region>>& SketchView::fillRegions(core::Id layer) const
{
    const core::Sketch& s = m_doc.sketch();
    // A fingerprint of everything the areas depend on, positions measured in a frame of the
    // layer's own (from its first point, towards the point farthest from that), to a hair, so a
    // move or turn of the whole layer leaves it as it was: a group dragged round, say.
    std::uint64_t h = 1469598103934665603ull;
    auto mix = [&](std::uint64_t v) {
        h ^= v + 0x9e3779b97f4a7c15ull + (h << 6) + (h >> 2);
        h *= 1099511628211ull;
    };
    auto mixLength = [&](double v) { mix(static_cast<std::uint64_t>(std::llround(v * 1e6))); };
    std::optional<core::Vec2> anchor;
    core::Vec2 far{0, 0};
    double farthest = 0.0;
    for (const auto& [id, e] : s.entities()) {
        if (const auto* p = std::get_if<core::Point>(&e.geom); p && s.layerOf(id) == layer) {
            if (!anchor) {
                anchor = core::Vec2{p->x, p->y};
            }
            const double d = std::hypot(p->x - anchor->x, p->y - anchor->y);
            if (d > farthest) {
                farthest = d;
                far = {p->x, p->y};
            }
        }
    }
    const core::Vec2 at = anchor.value_or(core::Vec2{0, 0});
    const core::Vec2 axis = farthest > 1e-9 ? core::Vec2{(far.x - at.x) / farthest, (far.y - at.y) / farthest}
                                            : core::Vec2{1, 0};
    auto mixPosition = [&](double x, double y) {
        const double dx = x - at.x, dy = y - at.y;
        mixLength(dx * axis.x + dy * axis.y);
        mixLength(dy * axis.x - dx * axis.y);
    };
    for (const auto& [id, e] : s.entities()) {
        if (s.layerOf(id) != layer) {
            continue;
        }
        mix(id);
        mix(static_cast<std::uint64_t>(e.geom.index()) * 4 + (e.construction ? 2 : 0) + (s.isReference(id) ? 1 : 0));
        std::visit(
            [&](const auto& g) {
                using G = std::decay_t<decltype(g)>;
                if constexpr (std::is_same_v<G, core::Point>) {
                    mixPosition(g.x, g.y);
                }
                else if constexpr (std::is_same_v<G, core::Line>) {
                    mix(g.p1);
                    mix(g.p2);
                }
                else if constexpr (std::is_same_v<G, core::Circle>) {
                    mix(g.center);
                    mixLength(g.radius);
                }
                else if constexpr (std::is_same_v<G, core::Arc>) {
                    mix(g.center);
                    mix(g.start);
                    mix(g.end);
                }
                else if constexpr (std::is_same_v<G, core::Ellipse>) {
                    mix(g.center);
                    mix(g.major);
                    mix(g.minor);
                }
                else if constexpr (std::is_same_v<G, core::EllipseArc>) {
                    mix(g.center);
                    mix(g.major);
                    mix(g.minor);
                    mix(g.start);
                    mix(g.end);
                }
                else {
                    for (core::Id p : g.points) {
                        mix(p);
                    }
                    mix(static_cast<std::uint64_t>(g.kind) * 2 + (g.closed ? 1 : 0));
                    mix(g.start);
                    mix(g.end);
                }
            },
            e.geom);
    }
    for (const auto& [id, f] : s.fills()) {
        if (f.layer != layer) {
            continue;
        }
        mix(id);
        for (core::Id b : f.boundary) {
            mix(b);
        }
        // The seed only picks the area when there's no outline to go by: then where it is
        // matters. (It stays put while the geometry is dragged, so isn't counted otherwise.)
        if (f.boundary.empty()) {
            mixPosition(f.seed.x, f.seed.y);
        }
    }

    FillCache& cache = m_fillCache[layer];
    if (cache.key == h && !cache.regions.empty()) {
        if (at.x != cache.anchor.x || at.y != cache.anchor.y || axis.x != cache.axis.x || axis.y != cache.axis.y) {
            // Moved (and turned) as a whole: from the old frame to the new.
            const core::Vec2 a0 = cache.anchor, u0 = cache.axis;
            auto move = [&](core::Vec2& p) {
                const double dx = p.x - a0.x, dy = p.y - a0.y;
                const double along = dx * u0.x + dy * u0.y, across = dy * u0.x - dx * u0.y;
                p = {at.x + along * axis.x - across * axis.y, at.y + along * axis.y + across * axis.x};
            };
            for (auto& [id, region] : cache.regions) {
                if (!region) {
                    continue;
                }
                for (core::Vec2& p : region->outer) {
                    move(p);
                }
                for (auto& hole : region->holes) {
                    for (core::Vec2& p : hole) {
                        move(p);
                    }
                }
            }
            cache.anchor = at;
            cache.axis = axis;
        }
        return cache.regions;
    }
    cache.key = h;
    cache.anchor = at;
    cache.axis = axis;
    cache.regions.clear();
    const core::regions::Arrangement arrangement(s, layer);
    for (const auto& [id, f] : s.fills()) {
        if (f.layer == layer) {
            cache.regions[id] = core::regions::resolve(arrangement, f);
        }
    }
    return cache.regions;
}

QPainterPath SketchView::regionPath(const core::regions::Region& region) const
{
    QPainterPath path;
    path.setFillRule(Qt::OddEvenFill);  // holes cut out
    // On screen, to within a third of a pixel: outlines found finely (or imported ones of many tiny
    // pieces) can have far more corners than show, and every one costs when filling.
    std::vector<QPointF> screen;
    std::vector<char> keep;
    std::vector<std::pair<size_t, size_t>> runs;
    auto loop = [&](const std::vector<core::Vec2>& points) {
        screen.clear();
        screen.reserve(points.size());
        for (const core::Vec2& p : points) {
            const QPointF q = toScreen(p);
            if (screen.empty() || std::abs(q.x() - screen.back().x()) >= 0.25 || std::abs(q.y() - screen.back().y()) >= 0.25) {
                screen.push_back(q);
            }
        }
        // Douglas-Peucker: corners that stray further than that from the straight line past them.
        const size_t n = screen.size();
        keep.assign(n, 0);
        if (n > 0) {
            keep[0] = keep[n - 1] = 1;
        }
        // (A loop: split where it's farthest from its start, so both halves have ends apart.)
        runs.clear();
        if (n > 2) {
            size_t m = 0;
            double far = -1.0;
            for (size_t k = 1; k < n; ++k) {
                const double d = std::hypot(screen[k].x() - screen[0].x(), screen[k].y() - screen[0].y());
                if (d > far) {
                    far = d;
                    m = k;
                }
            }
            keep[m] = 1;
            runs.push_back({0, m});
            runs.push_back({m, n - 1});
        }
        constexpr double kTolerance = 1.0 / 3.0;
        while (!runs.empty()) {
            const auto [a, b] = runs.back();
            runs.pop_back();
            const QPointF pa = screen[a], d = screen[b] - pa;
            const double length2 = d.x() * d.x() + d.y() * d.y();
            double worst = -1.0;
            size_t at = a;
            for (size_t k = a + 1; k < b; ++k) {
                const QPointF v = screen[k] - pa;
                const double s = length2 > 1e-18 ? std::clamp((v.x() * d.x() + v.y() * d.y()) / length2, 0.0, 1.0) : 0.0;
                const double off = std::hypot(v.x() - s * d.x(), v.y() - s * d.y());
                if (off > worst) {
                    worst = off;
                    at = k;
                }
            }
            if (worst > kTolerance && at != a) {
                keep[at] = 1;
                if (at - a > 1) {
                    runs.emplace_back(a, at);
                }
                if (b - at > 1) {
                    runs.emplace_back(at, b);
                }
            }
        }
        QPolygonF poly;
        for (size_t k = 0; k < n; ++k) {
            if (keep[k]) {
                poly << screen[k];
            }
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

QBrush SketchView::gradientBrush(const core::Gradient& g, double alpha) const
{
    const auto placed = core::gradient::place(g, m_doc.sketch());
    if (!placed) {
        // Its geometry is incomplete or gone: its average colour, until it has some again.
        const core::Color c = g.average();
        QColor color(c.r, c.g, c.b, c.a);
        color.setAlphaF(color.alphaF() * alpha);
        return QBrush(color);
    }
    const core::gradient::Placed& p = *placed;
    QGradientStops stops;
    for (const core::GradientStop& s : p.stops) {
        QColor c(s.color.r, s.color.g, s.color.b, s.color.a);
        c.setAlphaF(c.alphaF() * alpha);
        stops.append({s.offset, c});
    }
    if (stops.size() == 1) {
        stops.append({1.0, stops.front().second});
    }
    const QGradient::Spread spread = p.spread == core::GradientSpread::Reflect ? QGradient::ReflectSpread
                                     : p.spread == core::GradientSpread::Repeat ? QGradient::RepeatSpread
                                                                                 : QGradient::PadSpread;
    if (p.kind == core::GradientKind::Linear) {
        QLinearGradient lg(toScreen(p.start), toScreen(p.end));
        lg.setStops(stops);
        lg.setSpread(spread);
        return QBrush(lg);
    }
    // A circle round the origin, then moved, turned and squashed into place on screen (y down,
    // as in the SVG export).
    const double r = std::max(p.radius * m_zoom, 1e-6);
    QRadialGradient rg(QPointF(0, 0), r, QPointF(p.focalLocal.x * m_zoom, -p.focalLocal.y * m_zoom));
    rg.setStops(stops);
    rg.setSpread(spread);
    QBrush brush(rg);
    QTransform t;
    const QPointF c = toScreen(p.center);
    t.translate(c.x(), c.y());
    t.rotateRadians(-p.angle);
    t.scale(1.0, p.aspect);
    brush.setTransform(t);
    return brush;
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

core::PickOptions SketchView::editableOptions(double pixels) const
{
    core::PickOptions options;
    options.tolerance = pixels / m_zoom;
    options.sublayer = m_doc.sketch().activeSublayer();
    options.layer = m_doc.sketch().activeLayer();
    return options;
}

core::Id SketchView::pickAt(QPointF screen, bool annotations, bool editable) const
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
    if (!m_presentation) {
        options.layer = m_doc.sketch().activeLayer();  // (other layers only through projections)
        if (editable) {
            options.sublayer = m_doc.sketch().activeSublayer();
        }
    }
    const core::Id hit = core::pick(m_doc.sketch(), toWorld(screen), options);
    return hit != core::kNoId ? hit : textAt(screen, editable);  // geometry first, then text under it
}

Snap SketchView::snap(QPointF screen) const
{
    const core::Sketch& sketch = m_doc.sketch();
    const core::Vec2 world = toWorld(screen);

    core::PickOptions options;
    options.tolerance = kSnapPixels / m_zoom;
    options.curves = false;
    options.layer = sketch.activeLayer();  // (other layers only through projections)
    if (const core::Id id = core::pick(sketch, world, options); id != core::kNoId) {
        const core::Point& p = sketch.point(id);
        return Snap{{p.x, p.y}, id};
    }

    // Line midpoints.
    for (const auto& [id, e] : sketch.entities()) {
        if (const auto* l = std::get_if<core::Line>(&e.geom);
            l && !sketch.isAxis(id) && sketch.isShown(id) && sketch.layerOf(id) == sketch.activeLayer()) {
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
    const core::Sketch& s = m_doc.sketch();
    for (const auto& [id, e] : s.entities()) {
        if (!s.isReference(id) && e.sublayer == s.activeSublayer() && s.isShown(id)) {
            all.insert(id);  // (what can be changed: the active sublayer's)
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
            const core::Sketch& s = ed.sketch();
            if (s.hasEntity(id) || s.hasConstraint(id) || s.hasText(id) || s.hasFill(id)) {
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
    if (const auto& page = s.page()) {
        const std::array<double, 4> r = page->rect();
        include({r[0], r[1]});
        include({r[2], r[3]});
    }
    for (const auto& [id, c] : s.constraints()) {
        if (core::isDimension(c.type)) {
            if (const core::DimensionLayout L = core::layoutDimension(s, c); L.valid) {
                include(L.label);  // dimensions often sit outside the geometry
            }
        }
    }
    for (const auto& [id, t] : s.texts()) {
        if (!s.hasEntity(t.anchor)) {
            continue;
        }
        for (const auto& contour : t.shape) {
            for (const core::Point& p : contour) {
                const core::Point w = core::textToWorld(s.point(t.anchor), t.angle, p);
                include({w.x, w.y});
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
    ++m_documentStamp;  // (the cached picture of it is out of date)
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
    const qreal ratio = devicePixelRatioF();
    const BaseKey key{m_documentStamp, m_previewStamp, m_zoom,         m_pan,      size(),
                      ratio,           m_presentation, m_showConstraints, m_selection};
    if (!m_baseKey || !(*m_baseKey == key) || m_base.isNull()) {
        QImage base(QSize(static_cast<int>(std::ceil(width() * ratio)), static_cast<int>(std::ceil(height() * ratio))),
                    QImage::Format_ARGB32_Premultiplied);
        base.setDevicePixelRatio(ratio);
        QPainter painter(&base);
        painter.fillRect(rect(), theme::canvas().background);
        if (!m_presentation) {
            // Reference images, under everything, the grid too (not part of the drawing: never in
            // presentation view). Bottom layer first.
            const auto& layers = m_doc.sketch().layers();
            for (auto it = layers.rbegin(); it != layers.rend(); ++it) {
                if (it->visible) {
                    drawImages(painter, it->id);
                }
            }
        }
        drawGrid(painter);
        painter.setRenderHint(QPainter::Antialiasing);
        drawPage(painter);
        // With nothing hovered (that's drawn over it, below).
        struct Unhover
        {
            core::Id& hovered;
            core::Id was;
            ~Unhover() { hovered = was; }
        } unhover{m_hovered, std::exchange(m_hovered, core::kNoId)};
        drawSketch(painter);
        painter.end();
        m_base = std::move(base);
        m_baseKey = key;
        m_imageCache.prune();  // (only the pictures just drawn are kept)
    }
    QPainter painter(this);
    painter.drawImage(QPointF(0, 0), m_base);
    painter.setRenderHint(QPainter::Antialiasing);
    if (m_hovered != core::kNoId) {
        drawSketch(painter, m_hovered);
    }
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

void SketchView::setImagePreview(std::optional<core::Image> image)
{
    m_imagePreview = std::move(image);
    ++m_previewStamp;
    scheduleRepaint();
}

void SketchView::drawImages(QPainter& painter, core::Id layer) const
{
    for (const auto& [id, stored] : m_doc.sketch().images()) {
        const core::Image& im = m_imagePreview && m_imagePreview->id == id ? *m_imagePreview : stored;
        if (im.layer != layer || !im.visible) {
            continue;
        }
        const double shownWide = im.width * m_zoom;
        const QImage& picture = m_imageCache.get(im, shownWide);
        if (picture.isNull()) {
            continue;
        }
        painter.save();
        painter.setRenderHint(QPainter::SmoothPixmapTransform);
        painter.setOpacity(std::clamp(im.opacity, 0.0, 1.0));
        const QPointF c = toScreen({im.x, im.y});
        painter.translate(c);
        painter.rotate(-im.angle);  // (y down on screen)
        painter.scale((im.flipH ? -1.0 : 1.0) * shownWide / picture.width(),
                      (im.flipV ? -1.0 : 1.0) * im.height() * m_zoom / picture.height());
        painter.drawImage(QPointF(-picture.width() / 2.0, -picture.height() / 2.0), picture);
        painter.restore();
    }
}

void SketchView::drawPage(QPainter& painter) const
{
    const auto& page = m_doc.sketch().page();
    if (!page) {
        return;
    }
    const std::array<double, 4> r = page->rect();
    const QRectF sheet = QRectF(toScreen({r[0], r[3]}), toScreen({r[2], r[1]})).normalized();
    const QColor paper = page->hasColor ? QColor(page->color.r, page->color.g, page->color.b, page->color.a)
                                        : QColor(Qt::transparent);
    if (m_presentation) {
        // The drawing as it will be: off the page dimmed, the page in its colour.
        QPainterPath outside;
        outside.addRect(rect());
        outside.addRect(sheet);
        outside.setFillRule(Qt::OddEvenFill);
        painter.fillPath(outside, QColor(0, 0, 0, 120));
        painter.fillRect(sheet, paper);
    }
    else {
        // A sheet under the sketch: a faint shadow, its colour faintly, and its edge.
        painter.fillRect(sheet.translated(4, 4), QColor(0, 0, 0, 60));
        QColor faint = page->hasColor ? paper : QColor(0xff, 0xff, 0xff);
        faint.setAlphaF(faint.alphaF() * (page->hasColor ? 0.12 : 0.03));
        painter.fillRect(sheet, faint);
    }
    painter.setPen(QPen(QColor(0x8a, 0x92, 0x9c), 1.0));
    painter.setBrush(Qt::NoBrush);
    painter.drawRect(sheet);
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

QPainterPath SketchView::curvePath(const core::Sketch& sketch, core::Id curve) const
{
    const core::curve::Path p = core::curve::pathOf(sketch, curve);
    QPainterPath out;
    if (p.kind() == core::curve::Path::Kind::Spline) {
        const std::vector<core::curve::Bezier> pieces = p.beziers();
        for (size_t i = 0; i < pieces.size(); ++i) {
            const auto& q = pieces[i].p;
            if (i == 0) {
                out.moveTo(toScreen(q[0]));
            }
            out.cubicTo(toScreen(q[1]), toScreen(q[2]), toScreen(q[3]));
        }
        return out;
    }
    // (enough segments that it looks smooth at this zoom)
    const double size = std::max(p.ellipseShape.a, p.ellipseShape.b) * m_zoom;
    const int perQuarter = std::clamp(static_cast<int>(size / 4), 8, 256);
    const std::vector<core::Vec2> pts = p.polyline(perQuarter);
    for (size_t i = 0; i < pts.size(); ++i) {
        if (i == 0) {
            out.moveTo(toScreen(pts[i]));
        }
        else {
            out.lineTo(toScreen(pts[i]));
        }
    }
    return out;
}

void SketchView::drawControlPolygon(QPainter& painter, const core::Sketch& sketch, core::Id spline) const
{
    const core::Spline& sp = sketch.spline(spline);
    if (sp.kind != core::SplineKind::Control) {
        return;
    }
    QPolygonF poly;
    for (core::Id p : sp.points) {
        poly << toScreen({sketch.point(p).x, sketch.point(p).y});
    }
    if (sp.closed && !poly.isEmpty()) {
        poly << poly.front();
    }
    QColor faint = theme::canvas().construction;
    faint.setAlphaF(faint.alphaF() * 0.6);
    painter.save();
    painter.setPen(QPen(faint, 1.0, Qt::DotLine));
    painter.setBrush(Qt::NoBrush);
    painter.drawPolyline(poly);
    painter.restore();
}

void SketchView::drawSketch(QPainter& painter, core::Id only) const
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

    const core::Id activeLayer = sketch.activeLayer();
    auto styleFor = [&](const core::Entity& e) {
        QColor color = result.fullyConstrained.contains(e.id) ? colors.fullyConstrained : colors.underConstrained;
        if (e.construction) {
            color = colors.construction;
        }
        if (e.projectedFrom != core::kNoId) {
            color = sketch.isLinked(e.id) ? kProjected : kUnlinked;
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
        // Other sublayers: faded, as references (not while being pointed at or picked); other
        // layers fainter still.
        if (!m_presentation && e.sublayer != core::kNoId && e.sublayer != sketch.activeSublayer()
            && width == kCurveWidth) {
            color.setAlphaF(color.alphaF()
                            * (sketch.layerOfSublayer(e.sublayer) == activeLayer ? kOtherSublayerAlpha : kOtherLayerAlpha));
        }
        QPen pen(color, width, e.construction ? Qt::DashLine : Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin);
        return pen;
    };
    // Hidden sublayers aren't drawn while sketching (presentation view shows the whole drawing);
    // hidden layers aren't drawn at all.
    auto hidden = [&](core::Id id) {
        const core::Id layer = sketch.layerOf(id);
        if (layer != core::kNoId && sketch.hasLayer(layer) && !sketch.layer(layer).visible) {
            return true;
        }
        return !m_presentation && !sketch.isShown(id);
    };

    // The axes are drawn with the grid; here they only light up when hovered or selected.
    for (core::Id axis : {sketch.xAxis(), sketch.yAxis()}) {
        if (only != core::kNoId && axis != only) {
            continue;
        }
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
            case core::EntityKind::Ellipse:
            case core::EntityKind::EllipseArc:
            case core::EntityKind::Spline: painter.drawPath(curvePath(sketch, id)); break;
        }
    };
    // Layer by layer, from the bottom: fills, strokes, texts, then the geometry.
    // Presentation view on coloured paper: plain geometry and text in black, as exported.
    const bool onPaper = m_presentation && sketch.page() && sketch.page()->hasColor;
    for (auto layerIt = sketch.layers().rbegin(); layerIt != sketch.layers().rend(); ++layerIt) {
    const core::Id layer = layerIt->id;
    if (!layerIt->visible) {
        continue;
    }
    auto inLayer = [&](core::Id id) { return sketch.layerOf(id) == layer; };


    // Fills, underneath the rest: each found afresh from the geometry (see core/Regions.h).
    if (only == core::kNoId) {
        const auto& regions = fillRegions(layer);
        for (const auto& [id, f] : sketch.fills()) {
            if (f.layer != layer) {
                continue;
            }
            const auto found = regions.find(id);
            if (found == regions.end() || !found->second) {
                continue;  // its outline is open: nothing to fill until it's closed again
            }
            const auto& region = found->second;
            const double alpha = m_presentation ? 1.0 : 0.35;
            if (f.gradient.active()) {
                painter.fillPath(regionPath(*region), gradientBrush(f.gradient, alpha));
                continue;
            }
            QColor color(f.color.r, f.color.g, f.color.b, f.color.a);
            color.setAlphaF(color.alphaF() * alpha);
            painter.fillPath(regionPath(*region), color);
        }
    }

    // Strokes: the drawing's look. Faded under the geometry while sketching; in presentation
    // view they replace the geometry they decorate. (Construction geometry is never decorated.)
    painter.setBrush(Qt::NoBrush);
    std::set<core::Id> decorated;
    for (const auto& [id, style] : sketch.strokes()) {
        if (!sketch.hasEntity(id) || sketch.entity(id).construction || hidden(id) || !inLayer(id)) {
            continue;
        }
        if (style.none() || only != core::kNoId) {
            decorated.insert(id);  // no line: in presentation view, nothing shows (or: just one item)
            continue;
        }
        QPen pen = strokePen(style);
        if (style.gradient.active()) {
            pen.setBrush(gradientBrush(style.gradient, m_presentation ? 1.0 : 0.3));
        }
        else if (!m_presentation) {
            QColor faded = pen.color();
            faded.setAlphaF(faded.alphaF() * 0.3);
            pen.setColor(faded);
        }
        painter.setPen(pen);
        drawCurve(id, sketch.entity(id));
        decorated.insert(id);
    }

    // Texts: filled faintly with an outline while sketching (in their anchor's colour, or their
    // own), solid in presentation view.
    for (const auto& [id, t] : sketch.texts()) {
        if ((only != core::kNoId && id != only) || !sketch.hasEntity(t.anchor) || hidden(id) || !inLayer(id)) {
            continue;
        }
        const QPainterPath path = textPath(t);
        QColor color = t.hasColor ? QColor(t.color.r, t.color.g, t.color.b, t.color.a)
                                  : styleFor(sketch.entity(t.anchor)).color();
        const bool graded = t.hasColor && t.gradient.active();
        if (m_presentation) {
            if (!t.hasColor) {
                color = onPaper ? QColor(Qt::black) : colors.fullyConstrained;  // as plain geometry shows
            }
            if (graded) {
                painter.fillPath(path, gradientBrush(t.gradient));
            }
            else {
                painter.fillPath(path, color);
            }
            if (m_selection.contains(id) || id == m_hovered) {
                painter.strokePath(path, QPen(m_selection.contains(id) ? colors.selection : colors.hover, 1.0));
            }
            continue;
        }
        if (t.sublayer != sketch.activeSublayer() && !m_selection.contains(id) && id != m_hovered) {
            color.setAlphaF(color.alphaF() * (layer == activeLayer ? kOtherSublayerAlpha : kOtherLayerAlpha));
        }
        QColor faint = color;
        faint.setAlphaF(faint.alphaF() * 0.3);
        if (graded) {
            painter.fillPath(path, gradientBrush(t.gradient, 0.3));
        }
        else {
            painter.fillPath(path, faint);
        }
        const bool hot = m_selection.contains(id) || id == m_hovered;
        const QColor edge = m_selection.contains(id) ? colors.selection : id == m_hovered ? colors.hover : color;
        painter.strokePath(path, QPen(edge, hot ? kHighlightWidth : 1.0));
    }

    // Curves (points go on top of everything, below).
    for (const auto& [id, e] : sketch.entities()) {
        if ((only != core::kNoId && id != only) || sketch.isAxis(id) || e.kind() == core::EntityKind::Point || hidden(id)
            || !inLayer(id)) {
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
        if (onPaper && !m_selection.contains(id) && id != m_hovered && !constrained.contains(id)) {
            // On paper, plain geometry shows as the SVG export draws it: thin and black.
            painter.setPen(strokePen({{0, 0, 0, 255}, 0.25, core::Dash::Solid, {}}));
        }
        else {
            painter.setPen(styleFor(e));
        }
        drawCurve(id, e);
        if (e.kind() == core::EntityKind::Spline && !m_presentation && layer == activeLayer) {
            drawControlPolygon(painter, sketch, id);
        }
    }
    }  // (layers)

    if (m_presentation) {
        return;  // no constraint symbols, dimensions or points: just the drawing
    }
    drawAnnotations(painter, only);

    const std::set<core::Id> placedPoints = core::feature::placedOffsetPoints(sketch);  // (follow their source)
    // Plain dots a colour at a time, stamped (below): an imported drawing can have thousands.
    std::map<QRgb, QPolygonF> dots;
    struct Special
    {
        core::Id id;
        const core::Entity* entity;
        QPointF at;
        QColor color;
        bool highlighted;
    };
    std::vector<Special> special;
    const QRectF onScreen = QRectF(rect()).adjusted(-10, -10, 10, 10);
    for (const auto& [id, e] : sketch.entities()) {
        if ((only != core::kNoId && id != only) || e.kind() != core::EntityKind::Point || sketch.isHiddenReference(id)
            || hidden(id) || placedPoints.contains(id)) {
            continue;
        }
        if (const core::Id layer = sketch.layerOf(id); layer != core::kNoId && layer != activeLayer) {
            continue;  // other layers' points can't be used here
        }
        const core::Point& p = std::get<core::Point>(e.geom);
        const QPointF at = toScreen({p.x, p.y});
        if (!onScreen.contains(at)) {  // (off screen)
            continue;
        }
        const QColor color = styleFor(e).color();
        const bool highlighted = m_selection.contains(id) || id == m_hovered || constrained.contains(id);
        const bool marked = sketch.isOrigin(id) || (e.fixed && e.projectedFrom == core::kNoId);
        if (!highlighted && !marked) {
            dots[color.rgba()] << at;
        }
        else {
            special.push_back({id, &e, at, color, highlighted});
        }
    }
    // Each colour's dot drawn once, then stamped (far quicker than thousands of round shapes),
    // with one for each quarter-pixel offset so each sits where its own round one would.
    const double ratio = devicePixelRatioF();
    const int side = static_cast<int>(std::ceil((2.0 * kPointRadius + 2.0) * ratio));
    for (const auto& [rgba, points] : dots) {
        std::array<QImage, 16> stamps;
        for (const QPointF& at : points) {
            const QPointF corner = at - QPointF(side / (2.0 * ratio), side / (2.0 * ratio));
            const double fx = std::floor(corner.x() * ratio), fy = std::floor(corner.y() * ratio);
            const int qx = std::clamp(static_cast<int>((corner.x() * ratio - fx) * 4.0), 0, 3);
            const int qy = std::clamp(static_cast<int>((corner.y() * ratio - fy) * 4.0), 0, 3);
            QImage& stamp = stamps[static_cast<size_t>(qy * 4 + qx)];
            if (stamp.isNull()) {
                stamp = QImage(side, side, QImage::Format_ARGB32_Premultiplied);
                stamp.fill(Qt::transparent);
                QPainter dot(&stamp);
                dot.setRenderHint(QPainter::Antialiasing);
                dot.setPen(Qt::NoPen);
                dot.setBrush(QColor::fromRgba(rgba));
                dot.drawEllipse(QPointF(side / 2.0 + (qx + 0.5) / 4.0, side / 2.0 + (qy + 0.5) / 4.0), kPointRadius * ratio,
                                kPointRadius * ratio);
                stamp.setDevicePixelRatio(ratio);
            }
            painter.drawImage(QPointF(fx / ratio, fy / ratio), stamp);
        }
    }
    // Then the ones that stand out, on top.
    for (const Special& sp : special) {
        const double r = sp.highlighted ? kPointRadius + 1.2 : kPointRadius;
        painter.setPen(Qt::NoPen);
        painter.setBrush(sp.color);
        painter.drawEllipse(sp.at, r, r);
        if (sketch.isOrigin(sp.id)) {
            // The origin: a ring around the dot.
            painter.setBrush(Qt::NoBrush);
            painter.setPen(QPen(sp.color, 1.5));
            painter.drawEllipse(sp.at, 6.0, 6.0);
        }
        else if (sp.entity->fixed && sp.entity->projectedFrom == core::kNoId) {
            // Fixed points get a small square outline, a stand-in for Fusion's lock glyph.
            painter.setBrush(Qt::NoBrush);
            painter.setPen(QPen(sp.color, 1.0));
            painter.drawRect(QRectF(sp.at.x() - 5, sp.at.y() - 5, 10, 10));
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
            case core::EntityKind::Ellipse:
            case core::EntityKind::EllipseArc:
            case core::EntityKind::Spline:
                try {
                    painter.drawPath(curvePath(sketch, e.id));
                    if (e.kind() == core::EntityKind::Spline) {
                        drawControlPolygon(painter, sketch, e.id);
                    }
                }
                catch (const core::SketchError&) {
                }
                break;
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
