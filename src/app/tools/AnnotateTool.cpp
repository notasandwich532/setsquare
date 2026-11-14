#include "AnnotateTool.h"

#include "ConstraintTools.h"

#include "../Document.h"
#include "../Editor.h"
#include "../SketchView.h"
#include "../TextShapes.h"
#include "../Theme.h"

#include <core/Annotations.h>
#include <core/Curves.h>

#include <QComboBox>
#include <QDoubleSpinBox>
#include <QLineEdit>
#include <QPainter>
#include <QSpinBox>

#include <algorithm>

using core::DrawnDimension;
using core::DrawnDimensionKind;
using core::Id;
using core::kNoId;

namespace
{
// What the panel had last time (the tool is made afresh each time it's picked).
AnnotateTool::Kind s_kind = AnnotateTool::Kind::Auto;
double s_height = 3.5;
double s_width = 0.25;

bool isPoint(const core::Sketch& s, Id id)
{
    return s.hasEntity(id) && s.entity(id).kind() == core::EntityKind::Point;
}

std::optional<DrawnDimensionKind> drawnKindOf(core::ConstraintType type)
{
    switch (type) {
        case core::ConstraintType::Distance: return DrawnDimensionKind::Aligned;
        case core::ConstraintType::HorizontalDistance: return DrawnDimensionKind::Horizontal;
        case core::ConstraintType::VerticalDistance: return DrawnDimensionKind::Vertical;
        case core::ConstraintType::Radius: return DrawnDimensionKind::Radius;
        case core::ConstraintType::Diameter: return DrawnDimensionKind::Diameter;
        case core::ConstraintType::Angle: return DrawnDimensionKind::Angle;
        default: return std::nullopt;
    }
}

core::ConstraintType constraintOf(DrawnDimensionKind kind)
{
    switch (kind) {
        case DrawnDimensionKind::Horizontal: return core::ConstraintType::HorizontalDistance;
        case DrawnDimensionKind::Vertical: return core::ConstraintType::VerticalDistance;
        case DrawnDimensionKind::Radius: return core::ConstraintType::Radius;
        case DrawnDimensionKind::Diameter: return core::ConstraintType::Diameter;
        case DrawnDimensionKind::Angle: return core::ConstraintType::Angle;
        default: return core::ConstraintType::Distance;
    }
}

DrawnDimensionKind drawnKindOf(AnnotateTool::Kind kind)
{
    switch (kind) {
        case AnnotateTool::Kind::Horizontal: return DrawnDimensionKind::Horizontal;
        case AnnotateTool::Kind::Vertical: return DrawnDimensionKind::Vertical;
        case AnnotateTool::Kind::Radius: return DrawnDimensionKind::Radius;
        case AnnotateTool::Kind::Diameter: return DrawnDimensionKind::Diameter;
        case AnnotateTool::Kind::Angle: return DrawnDimensionKind::Angle;
        case AnnotateTool::Kind::ArcLength: return DrawnDimensionKind::ArcLength;
        case AnnotateTool::Kind::OrdinateX:
        case AnnotateTool::Kind::OrdinateY: return DrawnDimensionKind::Ordinate;
        case AnnotateTool::Kind::Leader: return DrawnDimensionKind::Leader;
        default: return DrawnDimensionKind::Aligned;
    }
}
}  // namespace

AnnotateTool::AnnotateTool(SketchView& view)
    : Tool(view)
{}

AnnotateTool::~AnnotateTool()
{
    if (m_panel) {
        QObject::disconnect(&doc(), nullptr, m_panel, nullptr);  // (the panel goes later: not through it)
        for (QObject* child : m_panel->findChildren<QObject*>()) {
            child->disconnect(m_panel);
        }
        m_panel->hide();
        m_panel->deleteLater();
    }
}

QString AnnotateTool::hint() const
{
    const Kind k = kind();
    if (k == Kind::Leader) {
        return m_picks.empty() && !m_freeTip
            ? QObject::tr("Leader: click what it points at (a point, a curve, or anywhere)")
            : QObject::tr("Click where its note goes, then type the note in the panel");
    }
    if (k == Kind::OrdinateX || k == Kind::OrdinateY) {
        return QObject::tr("Ordinate: click a point, then where its number goes (measured from the %1; Alt+click a "
                           "point to measure from it)")
            .arg(m_origin != kNoId ? QObject::tr("point you chose") : QObject::tr("origin"));
    }
    if (m_picks.empty()) {
        return QObject::tr("Annotate: pick what to measure (a line, circle, arc, two points or lines), then click where "
                           "the label goes. Click a dimension to change it. Its text, decimals, sizes: in the panel");
    }
    return QObject::tr("Move to place it and click (or pick a second thing)");
}

// ---------------------------------------------------------------------------------------------
// The panel
// ---------------------------------------------------------------------------------------------

void AnnotateTool::activate()
{
    m_panel = new CommandPanel(QObject::tr("Annotate"), &view());
    m_kind = m_panel->addChoice(QObject::tr("Kind"), "annotateKind",
                                {QObject::tr("Auto"), QObject::tr("Aligned"), QObject::tr("Horizontal"),
                                 QObject::tr("Vertical"), QObject::tr("Radius"), QObject::tr("Diameter"),
                                 QObject::tr("Angle"), QObject::tr("Arc length"), QObject::tr("Ordinate X"),
                                 QObject::tr("Ordinate Y"), QObject::tr("Leader (note)")},
                                static_cast<int>(s_kind));
    m_text = m_panel->addText(QObject::tr("Text"), "annotateText", QString());
    m_text->setToolTip(QObject::tr("What it says: <> is the measured value (e.g. \"4× Ø<> THRU\"). Empty: the "
                                   "usual for its kind"));
    QStringList places{QObject::tr("The drawing's")};
    for (int i = 0; i <= 6; ++i) {
        places << QString::number(i);
    }
    m_decimals = m_panel->addChoice(QObject::tr("Decimals"), "annotateDecimals", places);
    m_height = m_panel->addNumber(QObject::tr("Text height"), "annotateTextHeight", s_height, 0.1, 500.0);
    m_height->setDecimals(2);
    m_width = m_panel->addNumber(QObject::tr("Line width"), "annotateWidth", s_width, 0.0, 50.0);
    m_width->setDecimals(2);
    m_drawingDecimals = m_panel->addCount(QObject::tr("Drawing's decimals"), "annotateDrawingDecimals",
                                          doc().sketch().dimensionDecimals(), 0, 6);
    m_drawingDecimals->setToolTip(QObject::tr("How many places dimensions in this drawing show unless they say"));

    QObject::connect(m_kind, &QComboBox::currentIndexChanged, m_panel, [this](int i) {
        if (m_syncing) {
            return;
        }
        s_kind = static_cast<Kind>(i);
        reset();
        syncPanel();
    });
    QObject::connect(m_text, &QLineEdit::textEdited, m_panel, [this] { applyPanel(); });
    QObject::connect(m_decimals, &QComboBox::currentIndexChanged, m_panel, [this] { applyPanel(); });
    QObject::connect(m_height, &QDoubleSpinBox::valueChanged, m_panel, [this](double v) {
        s_height = v;
        applyPanel();
    });
    QObject::connect(m_width, &QDoubleSpinBox::valueChanged, m_panel, [this](double v) {
        s_width = v;
        applyPanel();
    });
    QObject::connect(m_drawingDecimals, &QSpinBox::valueChanged, m_panel, [this](int places) {
        if (m_syncing || places == doc().sketch().dimensionDecimals()) {
            return;
        }
        view().tryEdit(QObject::tr("Dimension decimals"), [&](Editor& ed) { ed.setDimensionDecimals(places); });
    });
    // Undo (or a script) may change or take away the one being edited.
    QObject::connect(&doc(), &Document::changed, m_panel, [this] {
        if (m_current != kNoId && !doc().sketch().hasDrawnDimension(m_current)) {
            m_current = kNoId;
        }
        syncPanel();
    });

    // Started with one selected: editing it; with geometry: measuring it.
    const core::Sketch& s = doc().sketch();
    std::vector<Id> geometry;
    for (Id id : view().selection()) {
        if (s.hasDrawnDimension(id)) {
            setCurrent(id);
            m_panel->show();
            return;
        }
        if (s.hasEntity(id) && !s.isReference(id)) {
            geometry.push_back(id);
        }
    }
    if (!geometry.empty() && geometry.size() <= 2) {
        bool incomplete = false;
        m_mouse = {0, 0};
        if (candidate(geometry, m_mouse, &incomplete) || incomplete) {
            m_picks = geometry;
        }
    }
    syncPanel();
    m_panel->show();
}

AnnotateTool::Kind AnnotateTool::kind() const
{
    return m_kind ? static_cast<Kind>(m_kind->currentIndex()) : s_kind;
}

void AnnotateTool::setKind(Kind kind)
{
    m_kind->setCurrentIndex(static_cast<int>(kind));
}

void AnnotateTool::setCurrent(Id dimension)
{
    m_current = doc().sketch().hasDrawnDimension(dimension) ? dimension : kNoId;
    view().setSelection(m_current != kNoId ? std::set<Id>{m_current} : std::set<Id>{});
    syncPanel();
    refresh();
}

void AnnotateTool::syncPanel()
{
    if (!m_panel) {
        return;
    }
    m_syncing = true;
    const core::Sketch& s = doc().sketch();
    m_drawingDecimals->setValue(s.dimensionDecimals());
    m_decimals->setItemText(0, QObject::tr("The drawing's (%1)").arg(s.dimensionDecimals()));
    if (m_current != kNoId) {
        // Its settings (its text only where it isn't the usual for its kind).
        const DrawnDimension& d = s.drawnDimension(m_current);
        const std::string usual = core::annotation::defaultText(d.kind);
        if (!m_text->hasFocus()) {
            m_text->setText(d.text == usual ? QString() : QString::fromStdString(d.text));
        }
        m_text->setPlaceholderText(QString::fromStdString(usual));
        m_decimals->setCurrentIndex(d.decimals + 1);
        m_height->setValue(d.textHeight);
        m_width->setValue(d.style.width);
    }
    else {
        m_text->setPlaceholderText(QString::fromStdString(core::annotation::defaultText(drawnKindOf(kind()))));
    }
    m_syncing = false;
}

DrawnDimension AnnotateTool::fromPanel(DrawnDimension d) const
{
    const QString text = m_text ? m_text->text() : QString();
    d.text = text.isEmpty() ? core::annotation::defaultText(d.kind) : text.toStdString();
    d.decimals = m_decimals ? m_decimals->currentIndex() - 1 : -1;
    d.textHeight = m_height ? m_height->value() : s_height;
    d.style.width = m_width ? m_width->value() : s_width;
    return d;
}

void AnnotateTool::applyPanel()
{
    if (m_syncing || m_current == kNoId || !doc().sketch().hasDrawnDimension(m_current)) {
        refresh();  // (a new one's preview)
        return;
    }
    const DrawnDimension d = fromPanel(doc().sketch().drawnDimension(m_current));
    // (Typing, or a spin box held down: one undo step for the run of them.)
    doc().setMergeKey(QStringLiteral("annotation %1").arg(m_current));
    view().tryEdit(QObject::tr("Edit annotation"), [&](Editor& ed) { ed.setAnnotation(d); });
}

bool AnnotateTool::takeColor(const core::Color& color, bool second)
{
    // Colour 1 recolours the one being edited too (and becomes Colour 1, for the next ones).
    if (!second && m_current != kNoId && doc().sketch().hasDrawnDimension(m_current)) {
        DrawnDimension d = doc().sketch().drawnDimension(m_current);
        d.style.color = color;
        view().tryEdit(QObject::tr("Colour"), [&](Editor& ed) { ed.setAnnotation(d); });
    }
    return false;
}

// ---------------------------------------------------------------------------------------------
// Picking and placing
// ---------------------------------------------------------------------------------------------

std::optional<DrawnDimension> AnnotateTool::candidate(const std::vector<Id>& picks, core::Vec2 at, bool* incomplete) const
{
    if (incomplete) {
        *incomplete = false;
    }
    const core::Sketch& s = doc().sketch();
    const Kind k = kind();
    DrawnDimension d;
    d.kind = drawnKindOf(k);
    switch (k) {
        case Kind::ArcLength:
            if (picks.size() != 1 || !s.hasEntity(picks[0]) || s.entity(picks[0]).kind() != core::EntityKind::Arc) {
                return std::nullopt;
            }
            d.refs = picks;
            break;
        case Kind::OrdinateX:
        case Kind::OrdinateY: {
            const Id origin = m_origin != kNoId && s.hasEntity(m_origin) ? m_origin : s.origin();
            if (picks.size() != 1 || !isPoint(s, picks[0]) || origin == kNoId || origin == picks[0]) {
                return std::nullopt;
            }
            d.refs = {picks[0], origin};
            d.alongY = k == Kind::OrdinateY;
            break;
        }
        case Kind::Leader:
            if (picks.size() == 1) {
                d.refs = picks;
                d.along = m_along;
            }
            else if (m_freeTip) {
                d.tip = {m_freeTip->x, m_freeTip->y};
            }
            else {
                return std::nullopt;
            }
            break;
        default: {
            // As the Dimension tool would: then, unless Auto, the kind asked for if that fits.
            bool more = false;
            const auto c = DimensionTool::candidateFor(s, picks, at, &more);
            if (incomplete) {
                *incomplete = more;
            }
            if (!c) {
                return std::nullopt;
            }
            const auto made = drawnKindOf(c->type);
            if (!made) {
                return std::nullopt;
            }
            d.kind = *made;
            d.refs = c->refs;
            d.flip = c->flip;
            if (k != Kind::Auto) {
                const DrawnDimensionKind wanted = drawnKindOf(k);
                const bool linear = [](DrawnDimensionKind x) {
                    return x == DrawnDimensionKind::Aligned || x == DrawnDimensionKind::Horizontal
                        || x == DrawnDimensionKind::Vertical;
                }(wanted);
                const bool round = wanted == DrawnDimensionKind::Radius || wanted == DrawnDimensionKind::Diameter;
                const bool fits = linear ? (d.kind == DrawnDimensionKind::Aligned || d.kind == DrawnDimensionKind::Horizontal
                                            || d.kind == DrawnDimensionKind::Vertical)
                                 : round ? (d.kind == DrawnDimensionKind::Radius || d.kind == DrawnDimensionKind::Diameter)
                                         : d.kind == wanted;
                if (!fits || !s.canAddConstraint(constraintOf(wanted), d.refs)) {
                    return std::nullopt;
                }
                d.kind = wanted;
            }
            break;
        }
    }
    d = fromPanel(d);
    d.font = textshapes::defaultFamily().toStdString();
    d.style.color = view().paint().color1;
    const core::Vec2 off = core::annotation::labelOffsetFor(s, d, at);
    d.labelX = off.x;
    d.labelY = off.y;
    return d;
}

void AnnotateTool::reset()
{
    m_picks.clear();
    m_freeTip.reset();
    m_along = 0.0;
    refresh();
}

void AnnotateTool::press(const ToolEvent& e)
{
    if (e.button != Qt::LeftButton) {
        cancel();
        return;
    }
    m_mouse = e.world;
    const core::Sketch& s = doc().sketch();
    const Kind k = kind();

    // A dimension clicked (with nothing picked yet): edit it.
    if (m_picks.empty() && !m_freeTip) {
        if (const Id dim = view().annotationAt(e.screen); dim != kNoId) {
            setCurrent(dim);
            return;
        }
    }
    const Id hit = view().pickAt(e.screen, false);
    const bool geometry = s.hasEntity(hit) && !s.isHiddenReference(hit);

    if ((k == Kind::OrdinateX || k == Kind::OrdinateY) && (e.modifiers & Qt::AltModifier)) {
        if (isPoint(s, hit)) {
            m_origin = hit;
            emit view().message(QObject::tr("Ordinates are measured from that point now"));
            refresh();
        }
        return;
    }
    if (k == Kind::Leader && m_picks.empty() && !m_freeTip) {
        if (geometry && !s.isAxis(hit)) {
            m_picks = {hit};
            if (!isPoint(s, hit)) {
                m_along = core::curve::pathOf(s, hit).param(e.world);
            }
        }
        else {
            m_freeTip = e.world;
        }
        refresh();
        return;
    }
    // A pick that still makes (or could make) one.
    if (k != Kind::Leader && geometry && m_picks.size() < 2 && std::ranges::find(m_picks, hit) == m_picks.end()) {
        std::vector<Id> tentative = m_picks;
        tentative.push_back(hit);
        bool incomplete = false;
        if (candidate(tentative, m_mouse, &incomplete) || incomplete) {
            m_picks = std::move(tentative);
            view().setSelection({m_picks.begin(), m_picks.end()});
            refresh();
            return;
        }
        if (m_picks.empty()) {
            emit view().message(QObject::tr("Can't dimension that as a %1").arg(m_kind->currentText().toLower()));
            return;
        }
    }

    // Otherwise: place the one we have.
    const auto d = candidate(m_picks, m_mouse);
    if (!d) {
        reset();
        return;
    }
    Id made = kNoId;
    view().tryEdit(QObject::tr("Annotate"), [&](Editor& ed) { made = ed.annotate(*d); });
    reset();
    if (made != kNoId) {
        setCurrent(made);
        if (k == Kind::Leader) {
            m_text->setFocus();  // (type the note)
            m_text->selectAll();
        }
    }
}

void AnnotateTool::move(const ToolEvent& e)
{
    m_mouse = e.world;
    if (m_picks.empty() && !m_freeTip) {
        const Id dim = view().annotationAt(e.screen);
        view().setHovered(dim != kNoId ? dim : view().pickAt(e.screen, false));
    }
    refresh();
}

bool AnnotateTool::cancel()
{
    if (!m_picks.empty() || m_freeTip) {
        reset();
        view().setSelection(m_current != kNoId ? std::set<Id>{m_current} : std::set<Id>{});
        return true;
    }
    if (m_current != kNoId) {
        setCurrent(kNoId);
        return true;
    }
    return false;
}

bool AnnotateTool::keyPress(int key, const QString&)
{
    if ((key == Qt::Key_Return || key == Qt::Key_Enter) && (!m_picks.empty() || m_freeTip)) {
        if (const auto d = candidate(m_picks, m_mouse)) {
            Id made = kNoId;
            view().tryEdit(QObject::tr("Annotate"), [&](Editor& ed) { made = ed.annotate(*d); });
            reset();
            setCurrent(made);
        }
        return true;
    }
    return false;
}

void AnnotateTool::paintOverlay(QPainter& painter)
{
    const core::Sketch& s = doc().sketch();
    // Where ordinates are measured from, when it's not the origin.
    if ((kind() == Kind::OrdinateX || kind() == Kind::OrdinateY) && m_origin != kNoId && s.hasEntity(m_origin)) {
        const core::Point& o = s.point(m_origin);
        painter.setPen(QPen(theme::canvas().dimension, 1.5));
        painter.setBrush(Qt::NoBrush);
        painter.drawEllipse(view().toScreen({o.x, o.y}), 7.0, 7.0);
    }
    const auto d = candidate(m_picks, m_mouse);
    if (!d) {
        return;
    }
    QColor color(d->style.color.r, d->style.color.g, d->style.color.b);
    color.setAlpha(170);
    view().drawAnnotation(painter, *d, color);
}
