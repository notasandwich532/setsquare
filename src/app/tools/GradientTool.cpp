#include "GradientTool.h"

#include "../CommandPanel.h"
#include "../Document.h"
#include "../GradientEditor.h"
#include "../SketchView.h"

#include <core/Features.h>
#include <core/Geometry.h>

#include <QColorDialog>
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QHBoxLayout>
#include <QPainter>
#include <QPainterPath>
#include <QPixmap>
#include <QPolygonF>
#include <QSignalBlocker>
#include <QSpinBox>
#include <QToolButton>

#include <algorithm>
#include <array>
#include <functional>
#include <cmath>
#include <numbers>

using core::Id;
using core::kNoId;
namespace gradient = core::gradient;

namespace
{
constexpr double kPickPixels = 7.0;
constexpr double kHandlePixels = 8.0;
constexpr double kDragPixels = 4.0;

QColor toQt(const core::Color& c)
{
    return {c.r, c.g, c.b, c.a};
}

core::Color fromQt(const QColor& c)
{
    return {static_cast<std::uint8_t>(c.red()), static_cast<std::uint8_t>(c.green()),
            static_cast<std::uint8_t>(c.blue()), static_cast<std::uint8_t>(c.alpha())};
}

bool sameOutline(const core::regions::Region& a, const core::regions::Region& b)
{
    return a.boundary == b.boundary && std::ranges::equal(a.outer, b.outer, [](core::Vec2 p, core::Vec2 q) {
               return p.x == q.x && p.y == q.y;
           });
}

// The default: Colour 1 to Colour 2, left to right.
core::Gradient defaultGradient(const SketchView::Paint& paint)
{
    core::Gradient g;
    g.stops = {{0.0, paint.color1}, {1.0, paint.color2}};
    return g;
}

// With Shift: the direction from `from` held to steps of 15 degrees.
core::Vec2 snapAngle(core::Vec2 from, core::Vec2 to)
{
    const double dx = to.x - from.x, dy = to.y - from.y;
    const double len = std::hypot(dx, dy);
    const double step = std::numbers::pi / 12;
    const double a = std::round(std::atan2(dy, dx) / step) * step;
    return {from.x + len * std::cos(a), from.y + len * std::sin(a)};
}
}  // namespace

GradientTool::GradientTool(SketchView& view)
    : Tool(view)
{}

GradientTool::~GradientTool()
{
    view().setHovered(kNoId);
    if (m_panel) {
        m_panel->hide();
        m_panel->deleteLater();
    }
}

QString GradientTool::hint() const
{
    switch (m_drag) {
        case Drag::Direction:
            return view().gradient().kind == core::GradientKind::Radial
                       ? QObject::tr("Gradient: drag out to where the last colour is (Shift: steps of 15°)")
                       : QObject::tr("Gradient: drag to where it ends (Shift: steps of 15°)");
        case Drag::Handle: return QObject::tr("Gradient: moving its handle (Shift: steps of 15°)");
        default: break;
    }
    const QString what = m_target == Target::Fill     ? QObject::tr("an area")
                         : m_target == Target::Stroke ? QObject::tr("a curve (double-click: its chain)")
                                                      : QObject::tr("a text");
    return QObject::tr("Gradient: click %1 to give it the gradient, or drag across it to set its direction; one "
                       "with a gradient is picked to edit (drag its handles). Shift+click: give it this gradient "
                       "anyway; Ctrl+click: take its gradient off")
        .arg(what);
}

// ---------------------------------------------------------------------------------------------
// The panel
// ---------------------------------------------------------------------------------------------

void GradientTool::activate()
{
    if (!view().gradient().active()) {
        view().setGradient(defaultGradient(view().paint()));
    }
    m_panel = new CommandPanel(QObject::tr("Gradient"), &view());
    m_targetBox = m_panel->addChoice(QObject::tr("Paint"), "gradientTarget",
                                     {QObject::tr("Fills"), QObject::tr("Strokes"), QObject::tr("Text")});
    m_kind = m_panel->addChoice(QObject::tr("Type"), "gradientType", {QObject::tr("Linear"), QObject::tr("Radial")});

    m_stops = new GradientEditor;
    m_panel->addWidget(QObject::tr("Colours"), m_stops);

    // The selected stop: its colour, place and opacity.
    auto* stopRow = new QWidget;
    auto* stopLayout = new QHBoxLayout(stopRow);
    stopLayout->setContentsMargins(0, 0, 0, 0);
    stopLayout->setSpacing(4);
    m_stopColor = new QToolButton(stopRow);
    m_stopColor->setObjectName(QStringLiteral("stopColor"));
    m_stopColor->setToolTip(QObject::tr("The selected colour: click to change it (or click a palette colour)"));
    m_stopColor->setIconSize(QSize(28, 14));
    m_stopPosition = new QDoubleSpinBox(stopRow);
    m_stopPosition->setObjectName(QStringLiteral("stopPosition"));
    m_stopPosition->setToolTip(QObject::tr("Where the selected colour is along the gradient"));
    m_stopPosition->setRange(0.0, 100.0);
    m_stopPosition->setDecimals(1);
    m_stopPosition->setSuffix(QStringLiteral(" %"));
    m_stopOpacity = new QSpinBox(stopRow);
    m_stopOpacity->setObjectName(QStringLiteral("stopOpacity"));
    m_stopOpacity->setToolTip(QObject::tr("The selected colour's opacity"));
    m_stopOpacity->setRange(0, 100);
    m_stopOpacity->setSuffix(QStringLiteral(" %"));
    stopLayout->addWidget(m_stopColor);
    stopLayout->addWidget(m_stopPosition, 1);
    stopLayout->addWidget(m_stopOpacity, 1);
    m_panel->addWidget(QObject::tr("Selected"), stopRow);

    m_spread = m_panel->addChoice(QObject::tr("Past the ends"), "gradientSpread",
                                  {QObject::tr("Carry on (pad)"), QObject::tr("Reflect"), QObject::tr("Repeat")});
    m_aspect = m_panel->addNumber(QObject::tr("Width across"), "gradientAspect", 1.0, 0.01, 100.0, QStringLiteral(" ×"));
    m_aspect->setToolTip(QObject::tr("A radial gradient's width across over its radius: 1 is a circle, else an ellipse"));

    auto* buttons = new QWidget;
    auto* buttonLayout = new QHBoxLayout(buttons);
    buttonLayout->setContentsMargins(0, 2, 0, 0);
    buttonLayout->setSpacing(4);
    auto button = [&](const QString& text, const char* name, const QString& tip) {
        auto* b = new QToolButton(buttons);
        b->setObjectName(QLatin1String(name));
        b->setText(text);
        b->setToolTip(tip);
        buttonLayout->addWidget(b);
        return b;
    };
    QToolButton* reverse = button(QObject::tr("Reverse"), "gradientReverse", QObject::tr("Run the colours the other way"));
    QToolButton* fromPaint = button(QObject::tr("Colours 1 → 2"), "gradientFromPaint",
                                    QObject::tr("Start again from Colour 1 to Colour 2"));
    QToolButton* plain = button(QObject::tr("Plain colour"), "gradientRemove",
                                QObject::tr("Take the gradient off the picked item (its plain colour again)"));
    buttonLayout->addStretch();
    m_panel->addWidget(QString(), buttons);

    // A change made in the panel: the gradient changes, and so does the picked item.
    auto change = [this](const std::function<void(core::Gradient&)>& edit, bool keep = true) {
        if (m_syncing) {
            return;
        }
        core::Gradient g = view().gradient();
        edit(g);
        view().setGradient(g);
        if (keep) {
            commit();
        }
    };
    QObject::connect(m_targetBox, &QComboBox::currentIndexChanged, m_panel, [this](int i) {
        if (!m_syncing) {
            setTarget(static_cast<Target>(i));
        }
    });
    QObject::connect(m_kind, &QComboBox::currentIndexChanged, m_panel, [change](int i) {
        change([i](core::Gradient& g) {
            g.kind = i == 1 ? core::GradientKind::Radial : core::GradientKind::Linear;
            if (g.kind == core::GradientKind::Radial && g.x1 == 0.0 && g.y1 == 0.5 && g.x2 == 1.0 && g.y2 == 0.5) {
                // Left to right as it came: a circle from the middle instead.
                g.x1 = 0.5;
                g.y1 = 0.5;
            }
        });
    });
    QObject::connect(m_spread, &QComboBox::currentIndexChanged, m_panel, [change](int i) {
        change([i](core::Gradient& g) {
            g.spread = i == 1 ? core::GradientSpread::Reflect : i == 2 ? core::GradientSpread::Repeat : core::GradientSpread::Pad;
        });
    });
    QObject::connect(m_aspect, &QDoubleSpinBox::valueChanged, m_panel,
                     [change](double v) { change([v](core::Gradient& g) { g.aspect = v; }); });
    QObject::connect(m_stops, &GradientEditor::editing, m_panel, [this, change] {
        const core::Gradient edited = m_stops->gradient();
        change([&](core::Gradient& g) { g.stops = edited.stops; }, false);
    });
    QObject::connect(m_stops, &GradientEditor::edited, m_panel, [this, change] {
        const core::Gradient edited = m_stops->gradient();
        change([&](core::Gradient& g) { g.stops = edited.stops; });
    });
    QObject::connect(m_stops, &GradientEditor::selectionChanged, m_panel, [this] { syncStop(); });
    auto pickStopColor = [this] {
        const int i = m_stops->selectedStop();
        if (i < 0) {
            return;
        }
        const QColor chosen = QColorDialog::getColor(toQt(view().gradient().stops[size_t(i)].color), m_panel,
                                                     QObject::tr("Gradient colour"), QColorDialog::ShowAlphaChannel);
        if (chosen.isValid()) {
            m_stops->setStopColor(i, fromQt(chosen));
        }
    };
    QObject::connect(m_stops, &GradientEditor::colorRequested, m_panel, pickStopColor);
    QObject::connect(m_stopColor, &QToolButton::clicked, m_panel, pickStopColor);
    QObject::connect(m_stopPosition, &QDoubleSpinBox::valueChanged, m_panel, [this](double v) {
        if (!m_syncing && m_stops->selectedStop() >= 0) {
            m_stops->moveStop(m_stops->selectedStop(), v / 100.0);
        }
    });
    QObject::connect(m_stopOpacity, &QSpinBox::valueChanged, m_panel, [this](int v) {
        const int i = m_stops->selectedStop();
        if (!m_syncing && i >= 0) {
            core::Color c = m_stops->gradient().stops[size_t(i)].color;
            c.a = static_cast<std::uint8_t>(std::lround(v * 255 / 100.0));
            m_stops->setStopColor(i, c);
        }
    });
    QObject::connect(reverse, &QToolButton::clicked, m_panel, [change] { change([](core::Gradient& g) { g.reverse(); }); });
    QObject::connect(fromPaint, &QToolButton::clicked, m_panel, [this, change] {
        const SketchView::Paint paint = view().paint();
        change([&](core::Gradient& g) { g.stops = defaultGradient(paint).stops; });
    });
    QObject::connect(plain, &QToolButton::clicked, m_panel, [this] {
        if (m_current == kNoId) {
            emit view().message(QObject::tr("Pick an item with a gradient first (click it)"));
            return;
        }
        Item item;
        item.id = m_current;
        removeGradient(item);
    });
    QObject::connect(m_panel, &CommandPanel::accepted, m_panel, [this] { view().setFocus(); });
    QObject::connect(m_panel, &CommandPanel::rejected, m_panel, [this] { view().setFocus(); });
    QObject::connect(&view(), &SketchView::gradientChanged, m_panel, [this] { syncPanel(); });
    QObject::connect(&doc(), &Document::changed, m_panel, [this] { onDocumentChanged(); });
    syncPanel();
}

void GradientTool::syncPanel()
{
    if (!m_panel || m_syncing) {
        return;
    }
    m_syncing = true;
    const core::Gradient& g = view().gradient();
    m_targetBox->setCurrentIndex(static_cast<int>(m_target));
    m_kind->setCurrentIndex(g.kind == core::GradientKind::Radial ? 1 : 0);
    m_spread->setCurrentIndex(static_cast<int>(g.spread));
    m_aspect->setValue(g.aspect);
    m_panel->setRowVisible(m_aspect, g.kind == core::GradientKind::Radial);
    if (!(m_stops->gradient() == g)) {
        m_stops->setGradient(g);
    }
    m_syncing = false;
    syncStop();
}

void GradientTool::syncStop()
{
    if (!m_panel || m_syncing) {
        return;
    }
    m_syncing = true;
    const core::Gradient& g = m_stops->gradient();
    const int i = m_stops->selectedStop();
    m_stopColor->setEnabled(i >= 0);
    m_stopPosition->setEnabled(i >= 0);
    m_stopOpacity->setEnabled(i >= 0);
    if (i >= 0 && i < static_cast<int>(g.stops.size())) {
        const core::GradientStop& s = g.stops[size_t(i)];
        QPixmap swatch(28, 14);
        swatch.fill(toQt(core::Color{s.color.r, s.color.g, s.color.b, 255}));
        m_stopColor->setIcon(QIcon(swatch));
        m_stopPosition->setValue(s.offset * 100.0);
        m_stopOpacity->setValue(static_cast<int>(std::lround(s.color.a * 100 / 255.0)));
    }
    m_syncing = false;
}

void GradientTool::setTarget(Target target)
{
    if (target == m_target) {
        return;
    }
    m_target = target;
    m_current = kNoId;
    m_hover = {};
    view().setHovered(kNoId);
    syncPanel();
    refresh();
}

bool GradientTool::takeColor(const core::Color& color, bool /*second*/)
{
    const int i = m_stops ? m_stops->selectedStop() : -1;
    if (i < 0) {
        return false;
    }
    m_stops->setStopColor(i, color);  // (edited: applied to the picked item)
    return true;
}

// ---------------------------------------------------------------------------------------------
// Items
// ---------------------------------------------------------------------------------------------

const core::regions::Arrangement& GradientTool::arrangement()
{
    const Id layer = doc().sketch().activeLayer();
    if (!m_arrangement || m_builtFor != doc().revision() || m_builtLayer != layer) {
        m_arrangement = std::make_unique<core::regions::Arrangement>(doc().sketch(), layer);
        m_builtFor = doc().revision();
        m_builtLayer = layer;
    }
    return *m_arrangement;
}

GradientTool::Item GradientTool::itemAt(QPointF screen, core::Vec2 world)
{
    const core::Sketch& s = doc().sketch();
    Item item;
    switch (m_target) {
        case Target::Fill: {
            item.region = arrangement().regionAt(world);
            item.at = world;
            if (!item.region) {
                return {};
            }
            for (const auto& [id, f] : s.fills()) {  // (the one filling just this area)
                if (f.layer != s.activeLayer()) {
                    continue;
                }
                const auto there = core::regions::resolve(arrangement(), f);
                if (there && sameOutline(*there, *item.region)) {
                    item.id = id;
                }
            }
            return item;
        }
        case Target::Stroke: {
            core::PickOptions options;
            options.tolerance = kPickPixels / view().zoom();
            options.points = false;
            const Id hit = core::pick(s, world, options);
            if (hit != kNoId && !s.isReference(hit) && !s.entity(hit).construction && !s.isAxis(hit)) {
                item.id = hit;
            }
            return item;
        }
        case Target::Text: item.id = view().textAt(screen, true); return item;
    }
    return item;
}

const core::Gradient* GradientTool::gradientOf(Id id) const
{
    const core::Sketch& s = doc().sketch();
    const core::Gradient* g = nullptr;
    switch (m_target) {
        case Target::Fill: g = s.hasFill(id) ? &s.fill(id).gradient : nullptr; break;
        case Target::Stroke: {
            const core::StrokeStyle* style = s.hasEntity(id) ? s.stroke(id) : nullptr;
            g = style ? &style->gradient : nullptr;
            break;
        }
        case Target::Text: g = s.hasText(id) && s.text(id).hasColor ? &s.text(id).gradient : nullptr; break;
    }
    return g && g->active() ? g : nullptr;
}

bool GradientTool::hasGradient(Id id) const
{
    return id != kNoId && gradientOf(id) != nullptr;
}

std::optional<gradient::Frame> GradientTool::frameOf(const Item& item) const
{
    const core::Sketch& s = doc().sketch();
    switch (m_target) {
        case Target::Fill:
            if (item.region) {
                return gradient::fillFrame(*item.region);
            }
            break;
        case Target::Stroke: {
            if (item.id == kNoId || !s.hasEntity(item.id)) {
                break;
            }
            // As it will be with a gradient (a whole group when it's in one already).
            std::map<Id, core::StrokeStyle> strokes = s.strokes();
            core::StrokeStyle style = s.stroke(item.id) ? *s.stroke(item.id) : view().paint().stroke();
            if (!style.gradient.active()) {
                style.gradient = view().gradient();
                if (style.none()) {
                    style.width = 0.35;
                }
                strokes[item.id] = style;
            }
            const auto frames = gradient::strokeFrames(s, strokes);
            if (const auto it = frames.find(item.id); it != frames.end()) {
                return it->second;
            }
            break;
        }
        case Target::Text:
            if (s.hasText(item.id)) {
                return gradient::textFrame(s, s.text(item.id));
            }
            break;
    }
    return std::nullopt;
}

std::optional<gradient::Frame> GradientTool::currentFrame() const
{
    if (m_current == kNoId) {
        return std::nullopt;
    }
    Item item;
    item.id = m_current;
    if (m_target == Target::Fill) {
        if (!doc().sketch().hasFill(m_current)) {
            return std::nullopt;
        }
        item.region = core::regions::resolve(const_cast<GradientTool*>(this)->arrangement(), doc().sketch().fill(m_current));
    }
    return frameOf(item);
}

void GradientTool::setCurrent(Id item)
{
    m_current = hasGradient(item) ? item : kNoId;
    if (m_current != kNoId) {
        view().setGradient(*gradientOf(m_current));
    }
    syncPanel();
    refresh();
}

void GradientTool::applyStroke(const std::vector<Id>& curves, const core::Gradient& g)
{
    const core::Sketch& s = doc().sketch();
    const SketchView::Paint& paint = view().paint();
    std::vector<std::pair<core::StrokeStyle, std::vector<Id>>> groups;
    for (Id c : curves) {
        if (!s.hasEntity(c) || s.isReference(c) || s.entity(c).construction
            || s.entity(c).kind() == core::EntityKind::Point) {
            continue;
        }
        core::StrokeStyle style = s.stroke(c) ? *s.stroke(c) : paint.stroke();
        style.gradient = g;
        if (style.none()) {
            style.width = paint.width >= 0.0 ? paint.width : 0.35;  // (a gradient on no line wouldn't show)
        }
        if (s.stroke(c) && *s.stroke(c) == style) {
            continue;
        }
        auto it = std::ranges::find_if(groups, [&](const auto& gr) { return gr.first == style; });
        if (it == groups.end()) {
            groups.push_back({style, {c}});
        }
        else {
            it->second.push_back(c);
        }
    }
    if (!groups.empty()) {
        view().tryEdit(QObject::tr("Gradient"), [&](Editor& ed) {
            for (const auto& [style, ids] : groups) {
                ed.setStroke(ids, style);
            }
        });
    }
}

void GradientTool::apply(const Item& item, const core::Gradient& g)
{
    const core::Sketch& s = doc().sketch();
    Id made = item.id;
    switch (m_target) {
        case Target::Fill:
            if (item.id != kNoId) {
                view().tryEdit(QObject::tr("Gradient"), [&](Editor& ed) { ed.setFillGradient(item.id, g); });
            }
            else {
                const core::Color color = view().paint().color2;
                view().tryEdit(QObject::tr("Gradient"), [&](Editor& ed) { made = ed.fill(item.at, color, g); });
            }
            break;
        case Target::Stroke: {
            // A curve in a gradient group: the whole group (it shares one gradient).
            const std::vector<Id> curves = hasGradient(item.id) ? gradient::strokeGroup(s, item.id) : std::vector<Id>{item.id};
            applyStroke(curves, g);
            break;
        }
        case Target::Text: {
            core::Text t = s.text(item.id);
            t.hasColor = true;
            t.gradient = g;
            t.color = g.average();
            view().tryEdit(QObject::tr("Gradient"), [&](Editor& ed) { ed.setText(t); });
            break;
        }
    }
    view().setGradient(g);
    setCurrent(made);
}

void GradientTool::removeGradient(const Item& item)
{
    if (!hasGradient(item.id)) {
        return;
    }
    const core::Sketch& s = doc().sketch();
    switch (m_target) {
        case Target::Fill:
            view().tryEdit(QObject::tr("Remove gradient"), [&](Editor& ed) { ed.setFillGradient(item.id, {}); });
            break;
        case Target::Stroke: {
            const std::vector<Id> group = gradient::strokeGroup(s, item.id);
            core::StrokeStyle style = *s.stroke(item.id);
            style.color = style.gradient.average();
            style.gradient = {};
            view().tryEdit(QObject::tr("Remove gradient"), [&](Editor& ed) { ed.setStroke(group, style); });
            break;
        }
        case Target::Text: {
            core::Text t = s.text(item.id);
            t.gradient = {};
            view().tryEdit(QObject::tr("Remove gradient"), [&](Editor& ed) { ed.setText(t); });
            break;
        }
    }
    if (item.id == m_current) {
        m_current = kNoId;
    }
    refresh();
}

void GradientTool::commit()
{
    if (m_current == kNoId || !hasGradient(m_current) || *gradientOf(m_current) == view().gradient()) {
        return;
    }
    Item item;
    item.id = m_current;
    apply(item, view().gradient());
}

void GradientTool::onDocumentChanged()
{
    if (m_drag != Drag::None || m_current == kNoId) {
        return;
    }
    // Undone, or changed from a script: follow it.
    if (!hasGradient(m_current)) {
        m_current = kNoId;
    }
    else if (!(*gradientOf(m_current) == view().gradient())) {
        view().setGradient(*gradientOf(m_current));
    }
    refresh();
}

// ---------------------------------------------------------------------------------------------
// Handles
// ---------------------------------------------------------------------------------------------

std::vector<core::Vec2> GradientTool::handles() const
{
    const auto frame = currentFrame();
    if (!frame) {
        return {};
    }
    const gradient::Placed p = gradient::place(view().gradient(), *frame);
    if (p.kind == core::GradientKind::Linear) {
        return {p.start, p.end};
    }
    const double r = p.radius * p.aspect;
    const core::Vec2 across{p.center.x - r * std::sin(p.angle), p.center.y + r * std::cos(p.angle)};
    return {p.center, p.end, p.focal, across};
}

int GradientTool::handleAt(QPointF screen) const
{
    const std::vector<core::Vec2> hs = handles();
    int best = -1;
    double bestD = kHandlePixels;
    // Later ones first: the focal point sits on the centre until it's moved off it.
    for (int i = static_cast<int>(hs.size()) - 1; i >= 0; --i) {
        const QPointF d = view().toScreen(hs[size_t(i)]) - screen;
        const double dist = std::hypot(d.x(), d.y());
        if (dist < bestD) {
            bestD = dist;
            best = i;
        }
    }
    return best;
}

void GradientTool::dragHandle(int handle, core::Vec2 world)
{
    const auto frame = currentFrame();
    if (!frame) {
        return;
    }
    core::Gradient g = view().gradient();
    const gradient::Placed p = gradient::place(g, *frame);
    const bool shift = m_pressModifiers & Qt::ShiftModifier;
    switch (handle) {
        case 0: {
            if (g.kind == core::GradientKind::Linear) {
                const core::Vec2 at = shift ? snapAngle(p.end, world) : world;
                const core::Vec2 f = frame->fractions(at);
                g.x1 = f.x;
                g.y1 = f.y;
            }
            else {
                // The centre: the whole gradient moves with it.
                const core::Vec2 f = frame->fractions(world);
                g.x2 += f.x - g.x1;
                g.y2 += f.y - g.y1;
                g.x1 = f.x;
                g.y1 = f.y;
            }
            break;
        }
        case 1: {
            const core::Vec2 f = frame->fractions(shift ? snapAngle(p.start, world) : world);
            g.x2 = f.x;
            g.y2 = f.y;
            break;
        }
        case 2: {
            const core::Vec2 f = frame->fractions(world);
            g.fx = f.x - g.x1;
            g.fy = f.y - g.y1;
            break;
        }
        case 3:
            if (p.radius > 0.0) {
                g.aspect = std::clamp(std::hypot(world.x - p.center.x, world.y - p.center.y) / p.radius, 0.01, 100.0);
            }
            break;
        default: return;
    }
    view().setGradient(g);
}

// ---------------------------------------------------------------------------------------------
// Input
// ---------------------------------------------------------------------------------------------

void GradientTool::move(const ToolEvent& e)
{
    if (m_drag == Drag::Pending) {
        const QPointF d = e.screen - m_pressScreen;
        if (std::hypot(d.x(), d.y()) >= kDragPixels && m_dragFrame) {
            m_drag = Drag::Direction;
        }
    }
    if (m_drag == Drag::Direction) {
        core::Gradient g = view().gradient();
        const core::Vec2 to = (e.modifiers & Qt::ShiftModifier) ? snapAngle(m_pressWorld, e.world) : e.world;
        const core::Vec2 a = m_dragFrame->fractions(m_pressWorld);
        const core::Vec2 b = m_dragFrame->fractions(to);
        g.x1 = a.x;
        g.y1 = a.y;
        g.x2 = b.x;
        g.y2 = b.y;
        g.fx = g.fy = 0.0;
        view().setGradient(g);
        refresh();
        return;
    }
    if (m_drag == Drag::Handle) {
        m_pressModifiers = e.modifiers;
        dragHandle(m_handle, e.world);
        refresh();
        return;
    }
    m_hover = itemAt(e.screen, e.world);
    view().setHovered(m_target == Target::Stroke ? m_hover.id : kNoId);
    refresh();
}

void GradientTool::press(const ToolEvent& e)
{
    if (e.button != Qt::LeftButton) {
        return;
    }
    m_pressScreen = e.screen;
    m_pressWorld = e.world;
    m_pressModifiers = e.modifiers;
    if (!(e.modifiers & (Qt::ControlModifier | Qt::ShiftModifier))) {
        m_handle = handleAt(e.screen);
        if (m_handle >= 0) {
            m_drag = Drag::Handle;
            return;
        }
    }
    m_dragItem = itemAt(e.screen, e.world);
    if (m_target != Target::Fill && m_dragItem.id == kNoId) {
        return;
    }
    if (m_target == Target::Fill && !m_dragItem.region) {
        emit view().message(QObject::tr("There's no closed area there to fill"));
        return;
    }
    m_dragFrame = frameOf(m_dragItem);
    m_drag = Drag::Pending;
}

void GradientTool::release(const ToolEvent& e)
{
    if (e.button != Qt::LeftButton) {
        return;
    }
    const Drag drag = std::exchange(m_drag, Drag::None);
    switch (drag) {
        case Drag::None: return;
        case Drag::Handle: commit(); break;
        case Drag::Direction: apply(m_dragItem, view().gradient()); break;
        case Drag::Pending: {
            const Item& item = m_dragItem;
            if (m_pressModifiers & Qt::ControlModifier) {
                removeGradient(item);
            }
            else if (hasGradient(item.id) && !(m_pressModifiers & Qt::ShiftModifier)) {
                setCurrent(item.id);  // pick it, to edit
            }
            else {
                apply(item, view().gradient());
            }
            break;
        }
    }
    m_dragFrame.reset();
    refresh();
}

void GradientTool::doubleClick(const ToolEvent& e)
{
    if (m_target != Target::Stroke || (e.modifiers & Qt::ControlModifier)) {
        return;
    }
    const Item item = itemAt(e.screen, e.world);
    if (item.id == kNoId) {
        return;
    }
    try {
        applyStroke(core::feature::chainAt(doc().sketch(), item.id), view().gradient());
        setCurrent(item.id);
    }
    catch (const core::SketchError&) {
    }
}

bool GradientTool::cancel()
{
    if (m_drag != Drag::None) {
        m_drag = Drag::None;
        m_dragFrame.reset();
        if (hasGradient(m_current)) {
            view().setGradient(*gradientOf(m_current));  // as it was
        }
        refresh();
        return true;
    }
    if (m_current != kNoId) {
        m_current = kNoId;
        refresh();
        return true;
    }
    return false;
}

// ---------------------------------------------------------------------------------------------
// Drawing
// ---------------------------------------------------------------------------------------------

void GradientTool::paintOverlay(QPainter& painter)
{
    const core::Sketch& s = doc().sketch();
    const core::Gradient& g = view().gradient();

    // An item drawn with the gradient as it's being set (it isn't kept until the mouse is let go).
    auto drawWith = [&](Id id, const std::optional<core::regions::Region>& region, const gradient::Frame& frame) {
        const QBrush brush = view().gradientBrush(g, frame);
        switch (m_target) {
            case Target::Fill:
                if (region) {
                    painter.fillPath(view().regionPath(*region), brush);
                }
                break;
            case Target::Stroke: {
                std::vector<Id> curves = hasGradient(id) ? gradient::strokeGroup(s, id) : std::vector<Id>{id};
                for (Id c : curves) {
                    if (!s.hasEntity(c)) {
                        continue;
                    }
                    core::StrokeStyle style = s.stroke(c) ? *s.stroke(c) : view().paint().stroke();
                    if (style.none()) {
                        style.width = 0.35;
                    }
                    QPen pen = view().strokePen(style);
                    pen.setBrush(brush);
                    painter.setPen(pen);
                    painter.setBrush(Qt::NoBrush);
                    QPolygonF line;
                    for (const core::Vec2& p : core::sample(s, c, 96)) {
                        line << view().toScreen(p);
                    }
                    painter.drawPolyline(line);
                }
                break;
            }
            case Target::Text:
                if (s.hasText(id)) {
                    painter.fillPath(view().textPath(s.text(id)), brush);
                }
                break;
        }
    };

    // Hover: the item a click would paint.
    if (m_drag == Drag::None && (m_hover.id != kNoId || m_hover.region)) {
        painter.setBrush(Qt::NoBrush);
        painter.setPen(QPen(QColor(0xff, 0xff, 0xff, 200), 1.5, Qt::DashLine));
        if (m_target == Target::Fill && m_hover.region) {
            painter.drawPath(view().regionPath(*m_hover.region));
        }
        else if (m_target == Target::Text && s.hasText(m_hover.id)) {
            painter.drawPolygon(view().textBox(s.text(m_hover.id)));
        }
    }

    if (m_drag == Drag::Direction && m_dragFrame) {
        drawWith(m_dragItem.id, m_dragItem.region, *m_dragFrame);
        painter.setPen(QPen(QColor(0xff, 0xff, 0xff), 1.5));
        painter.drawLine(view().toScreen(m_pressWorld), view().toScreen(gradient::place(g, *m_dragFrame).end));
        return;
    }

    // The picked item: drawn as the panel has it (while that's being changed), with its handles.
    if (m_current == kNoId || !hasGradient(m_current)) {
        return;
    }
    const auto frame = currentFrame();
    if (!frame) {
        return;
    }
    if (!(*gradientOf(m_current) == g)) {
        std::optional<core::regions::Region> region;
        if (m_target == Target::Fill) {
            region = core::regions::resolve(arrangement(), s.fill(m_current));
        }
        drawWith(m_current, region, *frame);
    }
    // Its box, faintly.
    QPolygonF box;
    for (const auto& [fx, fy] : std::array<std::pair<double, double>, 4>{{{0.0, 0.0}, {1.0, 0.0}, {1.0, 1.0}, {0.0, 1.0}}}) {
        box << view().toScreen(frame->at(fx, fy));
    }
    painter.setBrush(Qt::NoBrush);
    painter.setPen(QPen(QColor(0xff, 0xff, 0xff, 90), 1.0, Qt::DotLine));
    painter.drawPolygon(box);

    const std::vector<core::Vec2> hs = handles();
    const QColor accent(0x4d, 0xa3, 0xff);
    auto line = [&](core::Vec2 a, core::Vec2 b) {
        painter.setPen(QPen(QColor(0, 0, 0, 160), 3.0));
        painter.drawLine(view().toScreen(a), view().toScreen(b));
        painter.setPen(QPen(QColor(0xff, 0xff, 0xff), 1.2));
        painter.drawLine(view().toScreen(a), view().toScreen(b));
    };
    line(hs[0], hs[1]);
    if (hs.size() > 3) {
        line(hs[0], hs[3]);
    }
    for (size_t i = 0; i < hs.size(); ++i) {
        const QPointF c = view().toScreen(hs[i]);
        // Each handle in the colour it stands for: start/centre and end/edge, the focal point
        // as a diamond, the second axis as a square.
        const core::Color colour = i == 1 ? g.stops.back().color : g.stops.front().color;
        painter.setPen(QPen(i == size_t(m_handle) && m_drag == Drag::Handle ? accent : QColor(0xff, 0xff, 0xff), 1.5));
        painter.setBrush(QColor(colour.r, colour.g, colour.b));
        if (i == 2) {
            QPolygonF diamond;
            diamond << c + QPointF(0, -6) << c + QPointF(6, 0) << c + QPointF(0, 6) << c + QPointF(-6, 0);
            painter.drawPolygon(diamond);
        }
        else if (i == 3) {
            painter.setBrush(QColor(0x26, 0x29, 0x2e));
            painter.drawRect(QRectF(c - QPointF(4, 4), QSizeF(8, 8)));
        }
        else {
            painter.drawEllipse(c, 5.5, 5.5);
        }
    }
}
