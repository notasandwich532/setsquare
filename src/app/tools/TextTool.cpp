#include "TextTool.h"

#include "../CommandPanel.h"
#include "../Document.h"
#include "../FontPicker.h"
#include "../SketchView.h"
#include "../TextShapes.h"
#include "../Theme.h"

#include <QCheckBox>
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QLineEdit>
#include <QPainter>
#include <QPainterPath>

using core::Id;
using core::kNoId;

void TextTool::buildPanel()
{
    const core::Sketch& s = doc().sketch();
    const SketchView::ModifyOptions& o = view().modifyOptions();
    core::Text t;
    t.text = "Text";
    t.font = o.textFont.isEmpty() ? textshapes::defaultFamily().toStdString() : o.textFont.toStdString();
    t.height = o.textHeight;
    t.bold = o.textBold;
    t.italic = o.textItalic;
    t.outline = static_cast<core::TextOutline>(o.textOutline);
    t.minThickness = o.textMinThickness;
    if (m_editing != kNoId && s.hasText(m_editing)) {
        t = s.text(m_editing);
    }
    else {
        m_editing = kNoId;
    }

    m_position = addPick(QObject::tr("Position"), "position",
                         QObject::tr("click where the text starts (or a point to anchor it to)"),
                         [this](Id id) { return isPoint(id); }, false);
    m_text = panel()->addText(QObject::tr("Text"), "text", QString::fromStdString(t.text));
    m_font = new FontPicker;
    m_font->setObjectName(QStringLiteral("font"));
    m_font->setFamily(QString::fromStdString(t.font));
    panel()->addWidget(QObject::tr("Font"), m_font);
    QObject::connect(m_font, &FontPicker::familyChanged, panel(), &CommandPanel::changed);
    m_height = panel()->addNumber(QObject::tr("Height"), "height", t.height, 0.01, 1e5);
    m_height->setToolTip(QObject::tr("How tall the capital letters are"));
    m_angle = panel()->addNumber(QObject::tr("Angle"), "angle", t.angle, -360, 360, QStringLiteral(" °"));
    m_bold = panel()->addCheck(QObject::tr("Bold"), "bold", t.bold);
    m_italic = panel()->addCheck(QObject::tr("Italic"), "italic", t.italic);
    m_outline = panel()->addChoice(QObject::tr("Outline"), "outline",
                                   {QObject::tr("As designed"), QObject::tr("Merged (no overlaps)"),
                                    QObject::tr("Silhouette (outside only)")},
                                   static_cast<int>(t.outline));
    m_outline->setToolTip(QObject::tr("Merged joins overlapping strokes (holes like in an o stay); "
                                      "Silhouette keeps only the outside edge, for messy fonts"));
    m_thickness = panel()->addNumber(QObject::tr("Min thickness"), "minThickness", t.minThickness, 0.0, 1e4);
    m_thickness->setToolTip(QObject::tr("Thickens any part of the letters narrower than this (0: as the font has them)"));
    m_thickness->setSpecialValueText(QObject::tr("Off"));

    if (m_editing != kNoId) {
        setPicked(m_position, {t.anchor});
        setPickEnabled(m_position, false);
    }
}

core::Text TextTool::settings() const
{
    core::Text t;
    if (m_editing != kNoId && doc().sketch().hasText(m_editing)) {
        t = doc().sketch().text(m_editing);  // (keeps its id, anchor and colour)
    }
    t.text = m_text->text().toStdString();
    t.font = m_font->family().toStdString();
    t.height = m_height->value();
    t.angle = m_angle->value();
    t.bold = m_bold->isChecked();
    t.italic = m_italic->isChecked();
    t.outline = static_cast<core::TextOutline>(m_outline->currentIndex());
    t.minThickness = m_thickness->value();
    return t;
}

bool TextTool::where(core::Vec2& at) const
{
    if (!picked(m_position).empty()) {
        const core::Point& p = doc().sketch().point(picked(m_position).front());
        at = {p.x, p.y};
        return true;
    }
    if (m_hasLocation) {
        at = m_location.pos;
        return true;
    }
    return false;
}

bool TextTool::pickLocation(int input, const Snap& snap)
{
    if (input != m_position) {
        return false;
    }
    m_location = snap;
    m_hasLocation = true;
    setCurrentPick(-1);
    return true;
}

bool TextTool::ready() const
{
    core::Vec2 at;
    return where(at) && !m_text->text().isEmpty();
}

void TextTool::changed()
{
    if (!m_thickness) {
        return;
    }
    // Remade only when something that shapes the letters changed.
    const core::Text t = settings();
    const std::string key = t.text + '\n' + t.font + '\n' + std::to_string(t.height) + (t.bold ? "b" : "")
        + (t.italic ? "i" : "") + std::to_string(static_cast<int>(t.outline)) + ':' + std::to_string(t.minThickness);
    if (key != m_shapeKey) {
        m_shapeKey = key;
        m_shape = textshapes::make(t);
    }
    if (m_editing == kNoId) {
        SketchView::ModifyOptions& o = view().modifyOptions();
        o.textFont = QString::fromStdString(t.font);
        o.textHeight = t.height;
        o.textBold = t.bold;
        o.textItalic = t.italic;
        o.textOutline = static_cast<int>(t.outline);
        o.textMinThickness = t.minThickness;
    }
}

void TextTool::paintPreview(QPainter& painter)
{
    core::Vec2 at;
    if (!where(at)) {
        at = mouse();  // following the cursor until it's placed
    }
    if (m_shapeKey.empty()) {
        changed();
    }
    const core::Text t = settings();
    QPainterPath path;
    path.setFillRule(Qt::WindingFill);
    for (const auto& contour : m_shape) {
        QPolygonF polygon;
        for (const core::Point& p : contour) {
            const core::Point w = core::textToWorld({at.x, at.y}, t.angle, p);
            polygon << view().toScreen({w.x, w.y});
        }
        path.addPolygon(polygon);
        path.closeSubpath();
    }
    QColor fill = theme::canvas().underConstrained;
    fill.setAlphaF(0.25);
    painter.fillPath(path, fill);
    painter.strokePath(path, QPen(theme::canvas().underConstrained, 1.0, Qt::DashLine));
    // The baseline's start.
    const QPointF a = view().toScreen(at);
    painter.setPen(QPen(theme::canvas().underConstrained, 1.5));
    painter.drawLine(a, a + QPointF(12, 0));
}

bool TextTool::commit()
{
    core::Text t = settings();
    if (t.text.empty()) {
        emit view().message(QObject::tr("Text: type something first"));
        return false;
    }
    const std::vector<Id> point = picked(m_position);
    const Snap location = m_location;
    return view().tryEdit(QObject::tr("Text"), [&](Editor& ed) {
        if (m_editing != kNoId) {
            ed.setText(t);
            return;
        }
        t.anchor = !point.empty() ? point.front() : resolveSnap(ed, location);
        ed.text(t);
    });
}
