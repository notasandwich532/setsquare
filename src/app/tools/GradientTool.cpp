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
#include <QSpinBox>
#include <QToolButton>

#include <algorithm>
#include <cmath>
#include <functional>

using core::Id;
using core::kNoId;
namespace gradient = core::gradient;

namespace
{
constexpr double kPickPixels = 7.0;

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

Id& refOf(core::Gradient& g, int input)
{
    switch (input) {
        case GradientTool::Start: return g.start;
        case GradientTool::End: return g.end;
        case GradientTool::Shape: return g.shape;
        default: return g.focal;
    }
}

Id refOf(const core::Gradient& g, int input)
{
    return refOf(const_cast<core::Gradient&>(g), input);
}

const QColor kAccent(0x4d, 0xa3, 0xff);
}  // namespace

GradientTool::GradientTool(SketchView& view)
    : Tool(view)
{}

GradientTool::~GradientTool()
{
    view().setHovered(kNoId);
    if (m_panel) {
        // (It goes later: nothing may reach this tool through it meanwhile.)
        QObject::disconnect(&doc(), nullptr, m_panel, nullptr);
        QObject::disconnect(&view(), nullptr, m_panel, nullptr);
        for (QObject* child : m_panel->findChildren<QObject*>()) {
            child->disconnect(m_panel);
        }
        m_panel->hide();
        m_panel->deleteLater();
    }
}

QString GradientTool::hint() const
{
    switch (m_input) {
        case Start: return QObject::tr("Gradient: click the point where it starts");
        case End: return QObject::tr("Gradient: click the point where it ends");
        case Shape:
            return QObject::tr("Gradient: click the circle, arc, ellipse or elliptical arc where its last colour is "
                               "(its first is at the centre)");
        case Focal:
            return QObject::tr("Gradient: click a point to put its first colour at instead of the centre (× in the "
                               "panel: back to the centre)");
        default: break;
    }
    const QString what = m_target == Target::Fill     ? QObject::tr("an area")
                         : m_target == Target::Stroke ? QObject::tr("a curve (double-click: its chain)")
                                                      : QObject::tr("a text");
    return QObject::tr("Gradient: click %1 to give it the gradient; one with a gradient is picked to edit. "
                       "Shift+click: give it this gradient anyway; Ctrl+click: take its gradient off")
        .arg(what);
}

// ---------------------------------------------------------------------------------------------
// The panel
// ---------------------------------------------------------------------------------------------

void GradientTool::activate()
{
    if (!view().gradient().active()) {
        core::Gradient g;
        g.stops = {{0.0, view().paint().color1}, {1.0, view().paint().color2}};
        view().setGradient(g);
    }
    m_panel = new CommandPanel(QObject::tr("Gradient"), &view());
    m_targetBox = m_panel->addChoice(QObject::tr("Paint"), "gradientTarget",
                                     {QObject::tr("Fills"), QObject::tr("Strokes"), QObject::tr("Text")});
    m_kind = m_panel->addChoice(QObject::tr("Type"), "gradientType", {QObject::tr("Linear"), QObject::tr("Radial")});

    // The geometry placing it, picked on the canvas.
    const std::array<std::pair<QString, const char*>, kInputs> inputs{{
        {QObject::tr("Start point"), "gradientStart"},
        {QObject::tr("End point"), "gradientEnd"},
        {QObject::tr("Circle / ellipse"), "gradientShape"},
        {QObject::tr("Focal point"), "gradientFocal"},
    }};
    for (int i = 0; i < kInputs; ++i) {
        SelectionButton* b = m_panel->addSelection(inputs[size_t(i)].first, inputs[size_t(i)].second);
        m_inputs[size_t(i)] = b;
        QObject::connect(b, &QPushButton::clicked, m_panel, [this, i] {
            setCurrentInput(i);
            view().setFocus();
        });
        QObject::connect(b, &SelectionButton::cleared, m_panel, [this, i] { pickGeometry(i, kNoId); });
    }
    m_inputs[Focal]->setToolTip(QObject::tr("Optional: where the first colour is, if not the centre"));

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
    QObject::connect(m_kind, &QComboBox::currentIndexChanged, m_panel, [this, change](int i) {
        if (m_syncing) {
            return;
        }
        change([i](core::Gradient& g) { g.kind = i == 1 ? core::GradientKind::Radial : core::GradientKind::Linear; });
        nextInput();
    });
    QObject::connect(m_spread, &QComboBox::currentIndexChanged, m_panel, [change](int i) {
        change([i](core::Gradient& g) {
            g.spread = i == 1 ? core::GradientSpread::Reflect : i == 2 ? core::GradientSpread::Repeat : core::GradientSpread::Pad;
        });
    });
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
        const QColor chosen = QColorDialog::getColor(toQt(m_stops->gradient().stops[size_t(i)].color), m_panel,
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
        change([&](core::Gradient& g) { g.stops = {{0.0, paint.color1}, {1.0, paint.color2}}; });
    });
    QObject::connect(m_panel, &CommandPanel::accepted, m_panel, [this] { view().setFocus(); });
    QObject::connect(m_panel, &CommandPanel::rejected, m_panel, [this] { view().setFocus(); });
    QObject::connect(&view(), &SketchView::gradientChanged, m_panel, [this] { syncPanel(); });
    QObject::connect(&doc(), &Document::changed, m_panel, [this] { onDocumentChanged(); });

    // Geometry the gradient had that's gone (a new drawing, say): pick it again.
    core::Gradient g = view().gradient();
    bool gone = false;
    for (int i = 0; i < kInputs; ++i) {
        Id& ref = refOf(g, i);
        const bool fits = i == Shape ? gradient::canBeShape(doc().sketch(), ref) : gradient::canBePoint(doc().sketch(), ref);
        if (ref != kNoId && !fits) {
            ref = kNoId;
            gone = true;
        }
    }
    if (gone) {
        view().setGradient(g);
    }
    syncPanel();
    nextInput();
    view().setFocus();
}

std::vector<int> GradientTool::inputs() const
{
    return view().gradient().kind == core::GradientKind::Linear ? std::vector<int>{Start, End}
                                                                : std::vector<int>{Shape, Focal};
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
    const std::vector<int> shown = inputs();
    for (int i = 0; i < kInputs; ++i) {
        SelectionButton* b = m_inputs[size_t(i)];
        m_panel->setRowVisible(b, std::ranges::find(shown, i) != shown.end());
        b->setCount(refOf(g, i) != kNoId ? 1 : 0);
        if (i == Focal && refOf(g, i) == kNoId) {
            b->setText(QObject::tr("▸ Centre (select a point)"));
        }
        b->setCurrent(i == m_input);
    }
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

void GradientTool::setCurrentInput(int input)
{
    m_input = input >= 0 && input < kInputs ? input : -1;
    m_hoverGeometry = kNoId;
    view().setHovered(kNoId);
    syncPanel();
    refresh();
}

void GradientTool::nextInput()
{
    const core::Gradient& g = view().gradient();
    for (int i : inputs()) {
        if (i != Focal && refOf(g, i) == kNoId) {
            setCurrentInput(i);
            return;
        }
    }
    setCurrentInput(-1);
}

void GradientTool::pickGeometry(int input, Id id)
{
    core::Gradient g = view().gradient();
    refOf(g, input) = id;
    if (g.kind == core::GradientKind::Linear && g.start != kNoId && g.start == g.end) {
        emit view().message(QObject::tr("The start and end must be different points"));
        return;
    }
    view().setGradient(g);
    commit();
    nextInput();
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
// Items and geometry
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

Id GradientTool::geometryAt(core::Vec2 world, int input) const
{
    const core::Sketch& s = doc().sketch();
    core::PickOptions options;
    options.tolerance = kPickPixels / view().zoom();
    options.layer = s.activeLayer();  // (a gradient's geometry is in the layer it paints)
    const bool shape = input == Shape;
    options.points = !shape;
    options.curves = shape;
    const Id hit = core::pick(s, world, options);
    const bool fits = shape ? gradient::canBeShape(s, hit) : gradient::canBePoint(s, hit);
    return fits ? hit : kNoId;
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
            options.layer = s.activeLayer();
            const Id hit = core::pick(s, world, options);
            if (hit != kNoId && !s.isReference(hit) && !s.entity(hit).construction) {
                item.id = hit;
            }
            return item;
        }
        case Target::Text: {
            const Id t = view().textAt(screen, true);
            if (t != kNoId && s.layerOf(t) == s.activeLayer()) {
                item.id = t;
            }
            return item;
        }
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

void GradientTool::setCurrent(Id item)
{
    m_current = hasGradient(item) ? item : kNoId;
    if (m_current != kNoId) {
        view().setGradient(*gradientOf(m_current));
        if (m_input >= 0 && view().gradient().complete()) {
            setCurrentInput(-1);  // (it has its geometry: clicks edit, not pick)
        }
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
    if (!g.complete()) {
        emit view().message(g.kind == core::GradientKind::Linear
                                ? QObject::tr("Pick the gradient's start and end points first")
                                : QObject::tr("Pick the gradient's circle or ellipse first"));
        nextInput();
        return;
    }
    const core::Sketch& s = doc().sketch();
    Id made = item.id;
    bool ok = true;
    switch (m_target) {
        case Target::Fill:
            if (item.id != kNoId) {
                ok = view().tryEdit(QObject::tr("Gradient"), [&](Editor& ed) { ed.setFillGradient(item.id, g); });
            }
            else {
                const core::Color color = view().paint().color2;
                ok = view().tryEdit(QObject::tr("Gradient"), [&](Editor& ed) { made = ed.fill(item.at, color, g); });
            }
            break;
        case Target::Stroke: {
            // A curve in a gradient group (a stroked outline): the whole group.
            const std::vector<Id> curves = hasGradient(item.id) ? gradient::strokeGroup(s, item.id) : std::vector<Id>{item.id};
            applyStroke(curves, g);
            break;
        }
        case Target::Text: {
            core::Text t = s.text(item.id);
            t.hasColor = true;
            t.gradient = g;
            t.color = g.average();
            ok = view().tryEdit(QObject::tr("Gradient"), [&](Editor& ed) { ed.setText(t); });
            break;
        }
    }
    if (ok) {
        setCurrent(made);
    }
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
    const core::Gradient& g = view().gradient();
    if (m_current == kNoId || !hasGradient(m_current) || *gradientOf(m_current) == g || !g.complete()) {
        return;
    }
    Item item;
    item.id = m_current;
    apply(item, g);
}

void GradientTool::onDocumentChanged()
{
    // Undone, or changed from a script: follow it.
    if (m_current != kNoId) {
        if (!hasGradient(m_current)) {
            m_current = kNoId;
        }
        else if (!(*gradientOf(m_current) == view().gradient())) {
            view().setGradient(*gradientOf(m_current));
        }
    }
    refresh();
}

// ---------------------------------------------------------------------------------------------
// Input
// ---------------------------------------------------------------------------------------------

void GradientTool::move(const ToolEvent& e)
{
    if (m_input >= 0) {
        m_hoverGeometry = geometryAt(e.world, m_input);
        m_hover = {};
        view().setHovered(m_hoverGeometry);
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
    if (m_input >= 0) {
        const Id id = geometryAt(e.world, m_input);
        if (id == kNoId) {
            emit view().message(m_input == Shape ? QObject::tr("Click a circle, arc, ellipse or elliptical arc")
                                                 : QObject::tr("Click a point of the sketch"));
            return;
        }
        pickGeometry(m_input, id);
        return;
    }
    const Item item = itemAt(e.screen, e.world);
    if (m_target == Target::Fill ? !item.region : item.id == kNoId) {
        if (m_target == Target::Fill) {
            emit view().message(QObject::tr("There's no closed area there to fill"));
        }
        return;
    }
    if (e.modifiers & Qt::ControlModifier) {
        removeGradient(item);
    }
    else if (hasGradient(item.id) && !(e.modifiers & Qt::ShiftModifier)) {
        setCurrent(item.id);  // pick it, to edit
    }
    else {
        apply(item, view().gradient());
    }
    refresh();
}

void GradientTool::doubleClick(const ToolEvent& e)
{
    if (m_input >= 0 || m_target != Target::Stroke || (e.modifiers & Qt::ControlModifier)) {
        return;
    }
    const Item item = itemAt(e.screen, e.world);

    if (item.id == kNoId || !view().gradient().complete()) {
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
    if (m_input >= 0) {
        setCurrentInput(-1);
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

    // Hover: the item a click would paint.
    if (m_input < 0 && (m_hover.id != kNoId || m_hover.region)) {
        painter.setBrush(Qt::NoBrush);
        painter.setPen(QPen(QColor(0xff, 0xff, 0xff, 200), 1.5, Qt::DashLine));
        if (m_target == Target::Fill && m_hover.region) {
            painter.drawPath(view().regionPath(*m_hover.region));
        }
        else if (m_target == Target::Text && s.hasText(m_hover.id)) {
            painter.drawPolygon(view().textBox(s.text(m_hover.id)));
        }
    }

    // The picked item, drawn as the panel has it while that's being changed.
    if (m_current != kNoId && hasGradient(m_current) && !(*gradientOf(m_current) == g)) {
        const QBrush brush = view().gradientBrush(g);
        switch (m_target) {
            case Target::Fill:
                if (const auto region = core::regions::resolve(arrangement(), s.fill(m_current))) {
                    painter.fillPath(view().regionPath(*region), brush);
                }
                break;
            case Target::Stroke:
                for (Id c : gradient::strokeGroup(s, m_current)) {
                    QPen pen = view().strokePen(*s.stroke(c));
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
            case Target::Text: painter.fillPath(view().textPath(s.text(m_current)), brush); break;
        }
    }

    // The geometry placing the gradient: the shape, and a line from the first colour to the
    // last with a dot at each end in its colour (a diamond for a focal point).
    const auto placed = gradient::place(g, s);
    if (!placed) {
        return;
    }
    auto dot = [&](core::Vec2 at, const core::Color& colour, bool diamond) {
        const QPointF c = view().toScreen(at);
        painter.setPen(QPen(QColor(0xff, 0xff, 0xff), 1.5));
        painter.setBrush(QColor(colour.r, colour.g, colour.b));
        if (diamond) {
            QPolygonF d;
            d << c + QPointF(0, -6) << c + QPointF(6, 0) << c + QPointF(0, 6) << c + QPointF(-6, 0);
            painter.drawPolygon(d);
        }
        else {
            painter.drawEllipse(c, 5.0, 5.0);
        }
    };
    const bool radial = placed->kind == core::GradientKind::Radial;
    const core::Vec2 from = radial ? placed->focal : placed->start;
    painter.setPen(QPen(QColor(0, 0, 0, 160), 3.0));
    painter.drawLine(view().toScreen(from), view().toScreen(placed->end));
    painter.setPen(QPen(QColor(0xff, 0xff, 0xff), 1.2, Qt::DashLine));
    painter.drawLine(view().toScreen(from), view().toScreen(placed->end));
    if (radial) {
        painter.setPen(QPen(kAccent, 2.0));
        painter.setBrush(Qt::NoBrush);
        QPolygonF outline;
        for (const core::Vec2& p : core::sample(s, g.shape, 128)) {
            outline << view().toScreen(p);
        }
        painter.drawPolyline(outline);
    }
    dot(from, g.stops.front().color, radial && g.focal != kNoId);
    dot(placed->end, g.stops.back().color, false);
}
