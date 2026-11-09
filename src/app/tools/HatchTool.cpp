#include "HatchTool.h"

#include "../CommandPanel.h"
#include "../Document.h"
#include "../SketchView.h"

#include <core/Hatch.h>

#include <QComboBox>
#include <QDoubleSpinBox>
#include <QLineEdit>
#include <QPainter>
#include <QPainterPath>

#include <algorithm>

using core::Id;
using core::kNoId;

namespace
{
const QColor kAccent(0x4d, 0xa3, 0xff);

// Line widths (mm): hairline, then ISO pen sizes.
const double kWidths[] = {0.0, 0.13, 0.18, 0.25, 0.35, 0.5, 0.7, 1.0};

QString widthText(double w)
{
    return w == 0.0 ? QObject::tr("Hairline") : QStringLiteral("%1 mm").arg(w);
}

bool sameOutline(const core::regions::Region& a, const core::regions::Region& b)
{
    return a.boundary == b.boundary && std::ranges::equal(a.outer, b.outer, [](core::Vec2 p, core::Vec2 q) {
               return p.x == q.x && p.y == q.y;
           });
}
}  // namespace

HatchTool::HatchTool(SketchView& view)
    : Tool(view)
{}

HatchTool::~HatchTool()
{
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

QString HatchTool::hint() const
{
    return QObject::tr("Hatch: click areas to hatch them (again: leave one out); a hatched one is picked up to "
                       "edit. Shift+click: give it this hatch anyway; Ctrl+click: fill it solid again. Enter: done");
}

// ---------------------------------------------------------------------------------------------
// The panel
// ---------------------------------------------------------------------------------------------

void HatchTool::activate()
{
    m_panel = new CommandPanel(QObject::tr("Hatch"), &view());
    QStringList names;
    for (const core::hatch::Pattern& p : core::hatch::builtins()) {
        names << QStringLiteral("%1 — %2").arg(QString::fromStdString(p.name), QString::fromStdString(p.description));
    }
    m_pattern = m_panel->addChoice(QObject::tr("Pattern"), "hatchPattern", names);
    m_angle = m_panel->addNumber(QObject::tr("Angle"), "hatchAngle", 0.0, -360.0, 360.0, QStringLiteral("°"));
    m_angle->setDecimals(1);
    m_scale = m_panel->addNumber(QObject::tr("Scale"), "hatchScale", 1.0, 0.001, 10000.0, QString());
    m_scale->setDecimals(3);
    m_width = new QComboBox;
    m_width->setObjectName(QStringLiteral("hatchWidth"));
    m_width->setToolTip(QObject::tr("The width of the hatch's lines. Type any size in mm"));
    m_width->setEditable(true);
    m_width->setInsertPolicy(QComboBox::NoInsert);
    for (double w : kWidths) {
        m_width->addItem(widthText(w), w);
    }
    m_panel->addWidget(QObject::tr("Width"), m_width);
    m_list = m_panel->addSelection(QObject::tr("Areas"), "hatchItems");
    m_list->setCurrent(true);
    QObject::connect(m_list, &SelectionButton::cleared, m_panel, [this] {
        m_items.clear();  // (they keep their hatches: the panel's changes just stop going to them)
        syncPanel();
        refresh();
    });

    // A change in the panel: the hatch changes, and so do the listed areas.
    auto change = [this] {
        if (m_syncing) {
            return;
        }
        core::Hatch h = view().hatch();
        const int i = m_pattern->currentIndex();
        if (i >= 0 && i < static_cast<int>(core::hatch::builtins().size())) {
            const core::hatch::Pattern& p = core::hatch::builtins()[size_t(i)];
            h.pattern = p.name;
            h.lines = p.lines;
        }
        // (else the one the file brought: its own lines)
        h.angle = m_angle->value();
        h.scale = m_scale->value();
        QString text = m_width->currentText().trimmed();
        if (text.endsWith(QStringLiteral("mm"))) {
            text.chop(2);
        }
        bool ok = false;
        const double w = text.trimmed().toDouble(&ok);
        if (m_width->currentText().trimmed().compare(QObject::tr("Hairline"), Qt::CaseInsensitive) == 0) {
            h.width = 0.0;
        }
        else if (ok && w >= 0.0) {
            h.width = w;
        }
        setHatch(h);
    };
    QObject::connect(m_pattern, &QComboBox::activated, m_panel, change);
    QObject::connect(m_angle, &QDoubleSpinBox::editingFinished, m_panel, change);
    QObject::connect(m_scale, &QDoubleSpinBox::editingFinished, m_panel, change);
    QObject::connect(m_width, &QComboBox::activated, m_panel, change);
    QObject::connect(m_width->lineEdit(), &QLineEdit::editingFinished, m_panel, change);
    QObject::connect(m_panel, &CommandPanel::accepted, m_panel, [this] { view().setTool(ToolKind::Select); });
    QObject::connect(m_panel, &CommandPanel::rejected, m_panel, [this] { view().setFocus(); });
    QObject::connect(&doc(), &Document::changed, m_panel, [this] { onDocumentChanged(); });
    syncPanel();
    view().setFocus();
}

void HatchTool::syncPanel()
{
    if (!m_panel || m_syncing) {
        return;
    }
    m_syncing = true;
    const core::Hatch& h = view().hatch();
    // Its pattern: a built-in one, or (after them) the one it came with.
    const auto& builtins = core::hatch::builtins();
    int index = -1;
    for (size_t i = 0; i < builtins.size(); ++i) {
        if (builtins[i].name == h.pattern && builtins[i].lines == h.lines) {
            index = static_cast<int>(i);
        }
    }
    while (m_pattern->count() > static_cast<int>(builtins.size())) {
        m_pattern->removeItem(m_pattern->count() - 1);
    }
    if (index < 0) {
        m_pattern->addItem(QObject::tr("%1 (its own)").arg(QString::fromStdString(h.pattern)));
        index = m_pattern->count() - 1;
    }
    m_pattern->setCurrentIndex(index);
    m_angle->setValue(h.angle);
    m_scale->setValue(h.scale);
    if (const int i = m_width->findData(h.width); i >= 0) {
        m_width->setCurrentIndex(i);
    }
    else {
        m_width->setEditText(widthText(h.width));
    }
    m_list->setCount(static_cast<int>(m_items.size()));
    m_syncing = false;
}

void HatchTool::setHatch(const core::Hatch& h)
{
    view().setHatch(h);
    std::vector<Area> changed;
    for (Id id : m_items) {
        if (hatched(id) && !(doc().sketch().fill(id).hatch == h)) {
            Area a;
            a.fill = id;
            changed.push_back(a);
        }
    }
    if (!changed.empty()) {
        apply(changed, h);
    }
    syncPanel();
    refresh();
}

void HatchTool::setCurrent(Id fill)
{
    m_items.clear();
    if (hatched(fill)) {
        m_items.push_back(fill);
        view().setHatch(doc().sketch().fill(fill).hatch);
    }
    syncPanel();
    refresh();
}

bool HatchTool::takeColor(const core::Color& color, bool /*second*/)
{
    std::vector<Area> listed;
    for (Id id : m_items) {
        if (hatched(id)) {
            Area a;
            a.fill = id;
            listed.push_back(a);
        }
    }
    if (listed.empty()) {
        return false;  // (it goes to the colour bar: new hatches' colour)
    }
    apply(listed, view().hatch(), color);
    return true;
}

bool HatchTool::keyPress(int key, const QString& /*text*/)
{
    if (key == Qt::Key_Return || key == Qt::Key_Enter) {
        view().setTool(ToolKind::Select);  // done (this tool is gone after this)
        return true;
    }
    return false;
}

// ---------------------------------------------------------------------------------------------
// Areas
// ---------------------------------------------------------------------------------------------

const core::regions::Arrangement& HatchTool::arrangement()
{
    const Id layer = doc().sketch().activeLayer();
    if (!m_arrangement || m_builtFor != doc().revision() || m_builtLayer != layer) {
        m_arrangement = std::make_unique<core::regions::Arrangement>(doc().sketch(), layer);
        m_builtFor = doc().revision();
        m_builtLayer = layer;
    }
    return *m_arrangement;
}

HatchTool::Area HatchTool::areaAt(core::Vec2 world)
{
    const core::Sketch& s = doc().sketch();
    Area area;
    area.region = arrangement().regionAt(world);
    area.at = world;
    if (!area.region) {
        return {};
    }
    for (const auto& [id, f] : s.fills()) {  // (the one filling just this area)
        if (f.layer != s.activeLayer()) {
            continue;
        }
        const auto there = core::regions::resolve(arrangement(), f);
        if (there && sameOutline(*there, *area.region)) {
            area.fill = id;
        }
    }
    return area;
}

bool HatchTool::hatched(Id fill) const
{
    return fill != kNoId && doc().sketch().hasFill(fill) && doc().sketch().fill(fill).hatch.active();
}

std::vector<Id> HatchTool::apply(const std::vector<Area>& areas, const core::Hatch& h, std::optional<core::Color> color)
{
    std::vector<Id> out;
    view().tryEdit(QObject::tr("Hatch"), [&](Editor& ed) {
        const core::Sketch& s = ed.sketch();
        for (const Area& a : areas) {
            const bool has = a.fill != kNoId && s.hasFill(a.fill);
            const core::Point seed = has ? s.fill(a.fill).seed : core::Point{a.at.x, a.at.y};
            const core::Color c = color ? *color : has ? s.fill(a.fill).color : view().paint().color2;
            out.push_back(ed.fill({seed.x, seed.y}, c, {}, h));
        }
    });
    return out;
}

void HatchTool::onDocumentChanged()
{
    // Undone, or changed from a script: follow it.
    std::erase_if(m_items, [this](Id id) { return !hatched(id); });
    if (!m_items.empty() && !(doc().sketch().fill(m_items.front()).hatch == view().hatch())) {
        view().setHatch(doc().sketch().fill(m_items.front()).hatch);
    }
    syncPanel();
    refresh();
}

void HatchTool::click(const Area& area, Qt::KeyboardModifiers modifiers)
{
    if (modifiers & Qt::ControlModifier) {
        if (hatched(area.fill)) {
            view().tryEdit(QObject::tr("Hatch off"), [&](Editor& ed) { ed.setFillHatch(area.fill, {}); });
        }
        return;
    }
    // Already one of them: leave it out (it keeps its hatch).
    if (std::ranges::find(m_items, area.fill) != m_items.end() && area.fill != kNoId) {
        std::erase(m_items, area.fill);
        syncPanel();
        refresh();
        return;
    }
    if (m_items.empty() && hatched(area.fill) && !(modifiers & Qt::ShiftModifier)) {
        setCurrent(area.fill);  // picked up, to edit
        return;
    }
    for (Id id : apply({area}, view().hatch())) {
        if (std::ranges::find(m_items, id) == m_items.end()) {
            m_items.push_back(id);
        }
    }
    syncPanel();
    refresh();
}

// ---------------------------------------------------------------------------------------------
// Input and drawing
// ---------------------------------------------------------------------------------------------

void HatchTool::move(const ToolEvent& e)
{
    m_hover = areaAt(e.world);
    refresh();
}

void HatchTool::press(const ToolEvent& e)
{
    if (e.button != Qt::LeftButton) {
        return;
    }
    const Area area = areaAt(e.world);
    if (!area.region) {
        emit view().message(QObject::tr("There's no closed area there to hatch"));
        return;
    }
    click(area, e.modifiers);
}

bool HatchTool::cancel()
{
    if (!m_items.empty()) {
        m_items.clear();
        syncPanel();
        refresh();
        return true;
    }
    return false;
}

void HatchTool::paintOverlay(QPainter& painter)
{
    painter.setBrush(Qt::NoBrush);
    if (m_hover.region) {
        painter.setPen(QPen(QColor(0xff, 0xff, 0xff, 200), 1.5, Qt::DashLine));
        painter.drawPath(view().regionPath(*m_hover.region));
    }
    const core::Sketch& s = doc().sketch();
    painter.setPen(QPen(kAccent, 1.5));
    for (Id id : m_items) {
        if (!hatched(id)) {
            continue;
        }
        if (const auto region = core::regions::resolve(arrangement(), s.fill(id))) {
            painter.drawPath(view().regionPath(*region));
        }
    }
}
