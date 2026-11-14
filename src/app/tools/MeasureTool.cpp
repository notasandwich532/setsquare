#include "MeasureTool.h"

#include "../Document.h"
#include "../SketchView.h"
#include "../Theme.h"

#include <core/Geometry.h>
#include <core/Measure.h>

#include <QLabel>
#include <QPainter>
#include <QPainterPath>

#include <algorithm>

using core::Id;
using core::kNoId;

namespace
{
// 3 decimals, without trailing zeros: 12.5, 0.333, 40.
QString number(double v)
{
    QString s = QString::number(v, 'f', 3);
    if (s.contains(QLatin1Char('.'))) {
        while (s.endsWith(QLatin1Char('0'))) {
            s.chop(1);
        }
        if (s.endsWith(QLatin1Char('.'))) {
            s.chop(1);
        }
    }
    return s == QStringLiteral("-0") ? QStringLiteral("0") : s;
}

QString mm(double v)
{
    return number(v) + QStringLiteral(" mm");
}

QString mm2(double v)
{
    return number(v) + QStringLiteral(" mm²");
}

QLabel* valueLabel(const char* name)
{
    auto* label = new QLabel;
    label->setObjectName(QString::fromLatin1(name));
    label->setTextInteractionFlags(Qt::TextSelectableByMouse);  // (to copy)
    label->setMinimumWidth(110);
    return label;
}

bool isPoint(const core::Sketch& s, Id id)
{
    return s.hasEntity(id) && s.entity(id).kind() == core::EntityKind::Point;
}

bool isCurve(const core::Sketch& s, Id id)
{
    return s.hasEntity(id) && s.entity(id).kind() != core::EntityKind::Point && !s.isReference(id);
}

core::Vec2 at(const core::Sketch& s, Id point)
{
    const core::Point& p = s.point(point);
    return {p.x, p.y};
}
}  // namespace

MeasureTool::MeasureTool(SketchView& view)
    : Tool(view)
{}

MeasureTool::~MeasureTool()
{
    if (m_panel) {
        QObject::disconnect(&doc(), nullptr, m_panel, nullptr);  // (the panel goes later: not through it)
        m_panel->hide();
        m_panel->deleteLater();
    }
}

QString MeasureTool::hint() const
{
    if (m_mode == Mode::Points && m_b == kNoId) {
        return QObject::tr("Measure: click another point for the distance to it");
    }
    return QObject::tr("Measure: click a curve for its length (Shift+click to add more), a point and then another "
                       "for the distance between them, or inside a closed area for its area. Esc: done");
}

void MeasureTool::activate()
{
    m_panel = new CommandPanel(QObject::tr("Measure"), &view());
    m_prompt = new QLabel;
    m_prompt->setObjectName(QStringLiteral("measurePrompt"));
    m_prompt->setWordWrap(true);
    m_prompt->setMinimumWidth(180);
    m_panel->addWidget(QString(), m_prompt);
    m_length = valueLabel("measureLength");
    m_panel->addWidget(QObject::tr("Length"), m_length);
    m_distance = valueLabel("measureDistance");
    m_panel->addWidget(QObject::tr("Distance"), m_distance);
    m_dx = valueLabel("measureDx");
    m_panel->addWidget(QObject::tr("ΔX"), m_dx);
    m_dy = valueLabel("measureDy");
    m_panel->addWidget(QObject::tr("ΔY"), m_dy);
    m_area = valueLabel("measureArea");
    m_panel->addWidget(QObject::tr("Area"), m_area);

    // What's selected is measured straight away: curves (their total), or two points.
    const core::Sketch& s = doc().sketch();
    std::vector<Id> curves, points;
    for (Id id : view().selection()) {
        if (isCurve(s, id)) {
            curves.push_back(id);
        }
        else if (isPoint(s, id)) {
            points.push_back(id);
        }
    }
    if (!curves.empty()) {
        m_mode = Mode::Curves;
        m_curves = curves;
    }
    else if (points.size() == 2) {
        m_mode = Mode::Points;
        m_a = points[0];
        m_b = points[1];
    }
    else if (points.size() == 1) {
        m_mode = Mode::Points;
        m_a = points[0];
    }
    updatePanel();
    m_panel->show();
    // (Undo, a script or a drag elsewhere can change the drawing: the numbers keep up.)
    QObject::connect(&doc(), &Document::changed, m_panel, [this] {
        forgetMissing();
        updatePanel();
    });
}

void MeasureTool::clear()
{
    m_mode = Mode::None;
    m_curves.clear();
    m_a = m_b = kNoId;
    m_region.reset();
}

void MeasureTool::measureCurve(Id curve, bool add)
{
    if (add && m_mode == Mode::Curves) {
        // Shift+click: in the total, or out of it again.
        if (const auto it = std::ranges::find(m_curves, curve); it != m_curves.end()) {
            m_curves.erase(it);
            if (m_curves.empty()) {
                clear();
            }
            return;
        }
        m_curves.push_back(curve);
        return;
    }
    clear();
    m_mode = Mode::Curves;
    m_curves = {curve};
}

void MeasureTool::measurePoint(Id point)
{
    if (m_mode == Mode::Points && m_b == kNoId && m_a != point) {
        m_b = point;
        return;
    }
    clear();  // (a new pair, from this one)
    m_mode = Mode::Points;
    m_a = point;
}

void MeasureTool::measureArea(core::Vec2 where)
{
    const core::Sketch& s = doc().sketch();
    auto region = core::measuring::areaAt(s, s.activeLayer(), where);
    clear();
    if (!region) {
        emit view().message(QObject::tr("Nothing to measure there: click a curve, a point, or inside a closed area"));
        return;
    }
    m_mode = Mode::Area;
    m_areaAt = where;
    m_region = std::move(region);
    m_regionFor = doc().revision();
}

void MeasureTool::press(const ToolEvent& e)
{
    if (e.button != Qt::LeftButton) {
        return;
    }
    const core::Sketch& s = doc().sketch();
    const Id hit = view().pickAt(e.screen, false);
    if (isPoint(s, hit)) {
        measurePoint(hit);
    }
    else if (isCurve(s, hit)) {
        measureCurve(hit, e.modifiers & Qt::ShiftModifier);
    }
    else {
        measureArea(e.world);
    }
    updatePanel();
    refresh();
}

const core::regions::Arrangement& MeasureTool::hoverArrangement()
{
    const Id layer = doc().sketch().activeLayer();
    if (!m_arrangement || m_arrangementFor != doc().revision() || m_arrangementLayer != layer) {
        m_arrangement = std::make_unique<core::regions::Arrangement>(doc().sketch(), layer);
        m_arrangementFor = doc().revision();
        m_arrangementLayer = layer;
    }
    return *m_arrangement;
}

void MeasureTool::move(const ToolEvent& e)
{
    const core::Sketch& s = doc().sketch();
    const Id hit = view().pickAt(e.screen, false);
    m_hover = isPoint(s, hit) || isCurve(s, hit) ? hit : kNoId;
    m_hoverRegion.reset();
    if (m_hover == kNoId) {
        m_hoverRegion = hoverArrangement().regionAt(e.world);
    }
    refresh();
}

bool MeasureTool::cancel()
{
    if (m_mode == Mode::Points && m_b == kNoId) {
        clear();  // (a first point left waiting: let it go)
        updatePanel();
        refresh();
        return true;
    }
    return false;
}

void MeasureTool::forgetMissing()
{
    const core::Sketch& s = doc().sketch();
    std::erase_if(m_curves, [&](Id id) { return !isCurve(s, id); });
    if (m_mode == Mode::Curves && m_curves.empty()) {
        clear();
    }
    if (m_mode == Mode::Points) {
        if (!isPoint(s, m_b)) {
            m_b = kNoId;
        }
        if (!isPoint(s, m_a)) {
            clear();
        }
    }
    if (m_mode == Mode::Area && m_regionFor != doc().revision()) {
        // Edited: the area around the same place, as it is now.
        m_region = core::measuring::areaAt(s, s.activeLayer(), m_areaAt);
        m_regionFor = doc().revision();
        if (!m_region) {
            clear();
        }
    }
    if (!s.hasEntity(m_hover)) {
        m_hover = kNoId;
    }
}

void MeasureTool::beforePaint()
{
    forgetMissing();  // (also kept up by Document::changed; a drag in progress may not have sent it yet)
    updatePanel();
}

void MeasureTool::updatePanel()
{
    if (!m_panel) {
        return;
    }
    const core::Sketch& s = doc().sketch();
    QString prompt, length, distance, dx, dy, area;
    switch (m_mode) {
        case Mode::None:
            prompt = QObject::tr("Click a curve, two points, or inside a closed area");
            break;
        case Mode::Curves:
            prompt = m_curves.size() == 1 ? QObject::tr("1 curve (Shift+click to add more)")
                                          : QObject::tr("%1 curves, in all").arg(m_curves.size());
            length = mm(core::measuring::length(s, m_curves));
            break;
        case Mode::Points:
            if (m_b == kNoId) {
                prompt = QObject::tr("Click another point");
                break;
            }
            {
                const core::measuring::Separation d = core::measuring::between(s, m_a, m_b);
                prompt = QObject::tr("From the first point to the second");
                distance = mm(d.distance);
                dx = mm(d.dx);
                dy = mm(d.dy);
            }
            break;
        case Mode::Area:
            prompt = m_region && !m_region->holes.empty() ? QObject::tr("Closed area (its holes taken off)")
                                                          : QObject::tr("Closed area");
            if (m_region) {
                area = mm2(core::measuring::area(*m_region));
            }
            break;
    }
    const auto set = [this](QLabel* label, const QString& text) {
        if (label->text() != text) {
            label->setText(text);
        }
        m_panel->setRowVisible(label, !text.isEmpty());
    };
    set(m_prompt, prompt);
    set(m_length, length);
    set(m_distance, distance);
    set(m_dx, dx);
    set(m_dy, dy);
    set(m_area, area);
}

QString MeasureTool::shown(const char* row) const
{
    if (!m_panel) {
        return {};
    }
    const QLabel* label = m_panel->findChild<QLabel*>(QString::fromLatin1(row));
    return label && label->isVisibleTo(m_panel) ? label->text() : QString();
}

void MeasureTool::paintOverlay(QPainter& painter)
{
    const core::Sketch& s = doc().sketch();
    const QColor accent = theme::canvas().dimension;
    QColor soft = accent;
    soft.setAlpha(70);

    // A label centred on a place on screen.
    const auto label = [&](QPointF p, const QString& text) {
        const QFontMetricsF fm(painter.font());
        QRectF box = fm.boundingRect(text);
        box.moveCenter(p);
        box.adjust(-5, -2, 5, 2);
        QColor bg = theme::canvas().background;
        bg.setAlpha(225);
        painter.setPen(QPen(accent, 1));
        painter.setBrush(bg);
        painter.drawRoundedRect(box, 3, 3);
        painter.setPen(accent);
        painter.drawText(box, Qt::AlignCenter, text);
    };
    const auto curvePath = [&](Id id) {
        QPainterPath path;
        const std::vector<core::Vec2> pts = core::sample(s, id, 128);
        for (size_t i = 0; i < pts.size(); ++i) {
            const QPointF p = view().toScreen(pts[i]);
            i == 0 ? path.moveTo(p) : path.lineTo(p);
        }
        return path;
    };
    const auto dot = [&](core::Vec2 p) {
        painter.setPen(QPen(accent, 1.5));
        painter.setBrush(theme::canvas().background);
        painter.drawEllipse(view().toScreen(p), 5.0, 5.0);
    };

    // What a click would measure.
    painter.setBrush(Qt::NoBrush);
    if (m_hover != kNoId && s.hasEntity(m_hover)) {
        if (isPoint(s, m_hover)) {
            painter.setPen(QPen(accent, 1.5));
            painter.drawEllipse(view().toScreen(at(s, m_hover)), 8.0, 8.0);
        }
        else {
            painter.setPen(QPen(soft, 6, Qt::SolidLine, Qt::RoundCap));
            painter.drawPath(curvePath(m_hover));
        }
    }
    else if (m_hoverRegion) {
        painter.setPen(QPen(accent, 1, Qt::DashLine));
        painter.drawPath(view().regionPath(*m_hoverRegion));
    }

    // What's measured.
    switch (m_mode) {
        case Mode::None: break;
        case Mode::Curves: {
            QPainterPath all;
            for (Id id : m_curves) {
                all.addPath(curvePath(id));
            }
            painter.setBrush(Qt::NoBrush);
            painter.setPen(QPen(accent, 3, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
            painter.drawPath(all);
            if (m_curves.size() == 1) {
                const std::vector<core::Vec2> pts = core::sample(s, m_curves.front(), 128);
                label(view().toScreen(pts[pts.size() / 2]) + QPointF(0, -16), mm(core::measuring::length(s, m_curves)));
            }
            break;
        }
        case Mode::Points: {
            const core::Vec2 a = at(s, m_a);
            if (m_b == kNoId) {
                dot(a);
                break;
            }
            const core::Vec2 b = at(s, m_b);
            const core::measuring::Separation d = core::measuring::between(a, b);
            const QPointF pa = view().toScreen(a), pb = view().toScreen(b);
            const QPointF corner = view().toScreen({b.x, a.y});  // (along x from a, then up/down to b)
            painter.setBrush(Qt::NoBrush);
            painter.setPen(QPen(accent, 1, Qt::DashLine));
            if (std::abs(d.dx) > 1e-9 && std::abs(d.dy) > 1e-9) {
                painter.drawLine(pa, corner);
                painter.drawLine(corner, pb);
            }
            painter.setPen(QPen(accent, 2));
            painter.drawLine(pa, pb);
            dot(a);
            dot(b);
            label((pa + pb) / 2 + QPointF(0, -14), mm(d.distance));
            if (std::abs(d.dx) > 1e-9 && std::abs(d.dy) > 1e-9) {
                label((pa + corner) / 2 + QPointF(0, pa.y() <= pb.y() ? -12 : 12),
                      QObject::tr("ΔX %1").arg(number(d.dx)));
                label((corner + pb) / 2 + QPointF(corner.x() >= pa.x() ? 40 : -40, 0),
                      QObject::tr("ΔY %1").arg(number(d.dy)));
            }
            break;
        }
        case Mode::Area: {
            if (!m_region) {
                break;
            }
            const QPainterPath path = view().regionPath(*m_region);
            painter.fillPath(path, soft);
            painter.setBrush(Qt::NoBrush);
            painter.setPen(QPen(accent, 2));
            painter.drawPath(path);
            label(view().toScreen(m_region->interiorPoint()), mm2(core::measuring::area(*m_region)));
            break;
        }
    }
}
