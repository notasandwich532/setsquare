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
#include <QPainter>
#include <QPainterPath>
#include <QPlainTextEdit>

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
    t.spacing = o.textSpacing;
    if (m_editing != kNoId && s.hasText(m_editing)) {
        t = s.text(m_editing);
    }
    else {
        m_editing = kNoId;
    }

    m_type = panel()->addChoice(QObject::tr("Type"), "type", {QObject::tr("Text"), QObject::tr("Along a path")},
                                t.path != kNoId ? 1 : 0);
    m_position = addPick(QObject::tr("Position"), "position",
                         QObject::tr("click where the text goes (or a point to anchor it to)"),
                         [this](Id id) { return isPoint(id); }, false);
    m_path = addPick(QObject::tr("Path"), "path", QObject::tr("click the line, arc or circle to set the text along"),
                     [this](Id id) { return isCurve(id); }, false);
    m_text = new QPlainTextEdit(QString::fromStdString(t.text));
    m_text->setObjectName(QStringLiteral("text"));
    m_text->setFixedHeight(64);
    m_text->setTabChangesFocus(true);
    m_text->setToolTip(QObject::tr("Enter starts a new line; Ctrl+Enter (or OK) places the text"));
    panel()->addWidget(QObject::tr("Text"), m_text);
    QObject::connect(m_text, &QPlainTextEdit::textChanged, panel(), &CommandPanel::changed);
    m_font = new FontPicker;
    m_font->setObjectName(QStringLiteral("font"));
    m_font->setFamily(QString::fromStdString(t.font));
    panel()->addWidget(QObject::tr("Font"), m_font);
    QObject::connect(m_font, &FontPicker::familyChanged, panel(), &CommandPanel::changed);
    m_bold = panel()->addCheck(QObject::tr("Bold"), "bold", t.bold);
    m_italic = panel()->addCheck(QObject::tr("Italic"), "italic", t.italic);
    m_height = panel()->addNumber(QObject::tr("Height"), "height", t.height, 0.01, 1e5);
    m_height->setToolTip(QObject::tr("How tall the capital letters are"));
    m_spacing = panel()->addNumber(QObject::tr("Character spacing"), "spacing", t.spacing, -100, 1000, QStringLiteral(" %"));
    m_spacing->setToolTip(QObject::tr("Extra space between letters, as a share of the font size"));
    m_angle = panel()->addNumber(QObject::tr("Angle"), "angle", t.angle, -360, 360, QStringLiteral(" °"));
    m_align = panel()->addChoice(QObject::tr("Alignment"), "align",
                                 {QObject::tr("Left"), QObject::tr("Centre"), QObject::tr("Right")},
                                 static_cast<int>(t.align));
    m_valign = panel()->addChoice(QObject::tr("Vertical"), "valign",
                                  {QObject::tr("Baseline"), QObject::tr("Top"), QObject::tr("Middle"), QObject::tr("Bottom")},
                                  static_cast<int>(t.valign));
    m_flipH = panel()->addCheck(QObject::tr("Flip horizontally"), "flipH", t.flipH);
    m_flipV = panel()->addCheck(QObject::tr("Flip vertically"), "flipV", t.flipV);
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
        if (t.path != kNoId) {
            setPicked(m_path, {t.path});
        }
        else {
            setPicked(m_position, {t.anchor});
        }
        setPickEnabled(m_position, false);
        setPickEnabled(m_path, false);
        m_type->setEnabled(false);
    }
}

bool TextTool::alongPath() const
{
    return m_type && m_type->currentIndex() == 1;
}

core::Text TextTool::settings() const
{
    core::Text t;
    const core::Sketch& s = doc().sketch();
    if (m_editing != kNoId && s.hasText(m_editing)) {
        t = s.text(m_editing);  // (keeps its id, anchor, path and colour)
    }
    t.text = m_text->toPlainText().toStdString();
    t.font = m_font->family().toStdString();
    t.height = m_height->value();
    t.angle = alongPath() ? 0.0 : m_angle->value();
    t.bold = m_bold->isChecked();
    t.italic = m_italic->isChecked();
    t.spacing = m_spacing->value();
    t.align = static_cast<core::TextAlign>(m_align->currentIndex());
    t.valign = static_cast<core::TextVAlign>(m_valign->currentIndex());
    t.flipH = m_flipH->isChecked();
    t.flipV = m_flipV->isChecked();
    t.outline = static_cast<core::TextOutline>(m_outline->currentIndex());
    t.minThickness = m_thickness->value();
    if (alongPath() && !picked(m_path).empty()) {
        t.path = picked(m_path).front();
        t.anchor = s.definingPoints(t.path).front();
    }
    else if (!alongPath()) {
        t.path = kNoId;
    }
    return t;
}

bool TextTool::where(core::Vec2& at) const
{
    const core::Sketch& s = doc().sketch();
    if (alongPath()) {
        if (picked(m_path).empty()) {
            return false;
        }
        const core::Point& p = s.point(s.definingPoints(picked(m_path).front()).front());
        at = {p.x, p.y};
        return true;
    }
    if (!picked(m_position).empty()) {
        const core::Point& p = s.point(picked(m_position).front());
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
    return where(at) && !m_text->toPlainText().isEmpty();
}

void TextTool::changed()
{
    if (!m_thickness) {
        return;
    }
    const bool path = alongPath();
    panel()->setRowVisible(pickButton(m_position)->parentWidget(), !path);
    panel()->setRowVisible(pickButton(m_path)->parentWidget(), path);
    panel()->setRowVisible(m_angle, !path);
    if (path && picked(m_path).empty()) {
        setCurrentPick(m_path);
    }
    else if (!path && picked(m_position).empty() && !m_hasLocation) {
        setCurrentPick(m_position);
    }
    // Remade only when something that shapes the letters changed.
    const core::Text t = settings();
    const std::string key = t.text + '\n' + t.font + '\n' + std::to_string(t.height) + (t.bold ? "b" : "")
        + (t.italic ? "i" : "") + std::to_string(static_cast<int>(t.outline)) + ':' + std::to_string(t.minThickness)
        + ':' + std::to_string(t.spacing) + ':' + std::to_string(static_cast<int>(t.align))
        + std::to_string(static_cast<int>(t.valign)) + (t.flipH ? "h" : "") + (t.flipV ? "v" : "") + ':'
        + std::to_string(t.path);
    if (key != m_shapeKey) {
        m_shapeKey = key;
        m_shape = (path && t.path == kNoId) ? decltype(m_shape){} : textshapes::make(t, &doc().sketch());
    }
    if (m_editing == kNoId) {
        SketchView::ModifyOptions& o = view().modifyOptions();
        o.textFont = QString::fromStdString(t.font);
        o.textHeight = t.height;
        o.textBold = t.bold;
        o.textItalic = t.italic;
        o.textOutline = static_cast<int>(t.outline);
        o.textMinThickness = t.minThickness;
        o.textSpacing = t.spacing;
    }
}

void TextTool::paintPreview(QPainter& painter)
{
    core::Vec2 at;
    if (!where(at)) {
        if (alongPath()) {
            return;  // nothing to follow yet
        }
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
    if (!alongPath()) {
        // The anchor: a small cross where the text is placed from.
        const QPointF a = view().toScreen(at);
        painter.setPen(QPen(theme::canvas().underConstrained, 1.5));
        painter.drawLine(a - QPointF(6, 0), a + QPointF(6, 0));
        painter.drawLine(a - QPointF(0, 6), a + QPointF(0, 6));
    }
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
    const bool path = alongPath();
    return view().tryEdit(QObject::tr("Text"), [&](Editor& ed) {
        if (m_editing != kNoId) {
            ed.setText(t);
            return;
        }
        if (!path) {
            t.anchor = !point.empty() ? point.front() : resolveSnap(ed, location);
        }
        ed.text(t);
    });
}
