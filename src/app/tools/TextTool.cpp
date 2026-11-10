#include "TextTool.h"

#include "../CommandPanel.h"
#include "../Document.h"
#include "../FontPicker.h"
#include "../SketchView.h"
#include "../TextShapes.h"
#include "../Theme.h"

#include <core/Features.h>

#include <QCheckBox>
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QPainter>
#include <QPainterPath>
#include <QPlainTextEdit>
#include <QSignalBlocker>

#include <algorithm>
#include <cmath>

using core::Id;
using core::kNoId;

namespace
{
// The vertical choices, as offered for each kind (in a box: against its top, middle or bottom).
void fillVertical(QComboBox* box, bool inBox, core::TextVAlign current)
{
    const QSignalBlocker block(box);
    box->clear();
    if (inBox) {
        box->addItem(QObject::tr("Top"), int(core::TextVAlign::Top));
        box->addItem(QObject::tr("Middle"), int(core::TextVAlign::Middle));
        box->addItem(QObject::tr("Bottom"), int(core::TextVAlign::Bottom));
        if (current == core::TextVAlign::Baseline) {
            current = core::TextVAlign::Bottom;
        }
    }
    else {
        box->addItem(QObject::tr("Baseline"), int(core::TextVAlign::Baseline));
        box->addItem(QObject::tr("Top"), int(core::TextVAlign::Top));
        box->addItem(QObject::tr("Middle"), int(core::TextVAlign::Middle));
        box->addItem(QObject::tr("Bottom"), int(core::TextVAlign::Bottom));
    }
    box->setCurrentIndex(std::max(0, box->findData(int(current))));
}
}  // namespace

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
    t.valign = core::TextVAlign::Top;
    if (m_editing != kNoId && s.hasText(m_editing)) {
        t = s.text(m_editing);
    }
    else {
        m_editing = kNoId;
    }
    const bool pointText = m_editing != kNoId && !t.inBox() && t.path == kNoId;

    QStringList kinds{QObject::tr("Text box"), QObject::tr("Along a path")};
    if (pointText) {
        kinds << QObject::tr("At a point");
    }
    m_type = panel()->addChoice(QObject::tr("Type"), "type", kinds, pointText ? 2 : t.path != kNoId ? 1 : 0);
    m_corner = addPick(QObject::tr("Corner"), "corner", QObject::tr("click a corner of the text's box"),
                       [this](Id id) { return isPoint(id); }, false);
    m_opposite = addPick(QObject::tr("Opposite corner"), "opposite", QObject::tr("click the opposite corner of the box"),
                         [this](Id id) { return isPoint(id); }, false);
    m_path = addPick(QObject::tr("Path"), "path",
                     QObject::tr("click the curve to set the text along (or curves joined end to end: click each)"),
                     [this](Id id) { return isCurve(id); }, true);
    m_position = addPick(QObject::tr("Position"), "position", QObject::tr("click where the text goes"),
                         [this](Id id) { return isPoint(id); }, false);
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
    m_angle = panel()->addNumber(QObject::tr("Angle"), "angle", t.angle, -360, 360, QStringLiteral(" Â°"));
    m_align = panel()->addChoice(QObject::tr("Alignment"), "align",
                                 {QObject::tr("Left"), QObject::tr("Centre"), QObject::tr("Right")},
                                 static_cast<int>(t.align));
    m_valign = panel()->addChoice(QObject::tr("Vertical"), "valign", {});
    fillVertical(m_valign, kind() == Kind::Box, t.valign);
    m_flipH = panel()->addCheck(QObject::tr("Flip horizontally"), "flipH", t.flipH);
    m_flipV = panel()->addCheck(QObject::tr("Flip vertically"), "flipV", t.flipV);
    m_wrap = panel()->addCheck(QObject::tr("Wrap text"), "wrap", t.wrap);
    m_wrap->setToolTip(QObject::tr("Break lines between words to fit the box's width"));
    m_boxHeight = panel()->addChoice(QObject::tr("Box height"), "boxHeight",
                                     {QObject::tr("Fixed"), QObject::tr("Fits the text")}, t.grow ? 1 : 0);
    m_boxHeight->setToolTip(QObject::tr("Fits the text: the box is made as tall as the wrapped text (growing or shrinking), its side away from "
                                        "where the text is aligned moving (refused if that side can't move)"));
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
        if (t.inBox()) {
            setPicked(m_corner, {t.anchor});
            setPicked(m_opposite, {t.boxCorner});
        }
        else if (t.path != kNoId) {
            std::vector<Id> chain{t.path};
            chain.insert(chain.end(), t.pathMore.begin(), t.pathMore.end());
            setPicked(m_path, chain);
        }
        else {
            setPicked(m_position, {t.anchor});
        }
        for (int pick : {m_corner, m_opposite, m_path, m_position}) {
            setPickEnabled(pick, false);
        }
        m_type->setEnabled(false);
    }
}

TextTool::Kind TextTool::kind() const
{
    if (!m_type) {
        return Kind::Box;
    }
    return static_cast<Kind>(m_type->currentIndex());
}

core::Text TextTool::settings() const
{
    core::Text t;
    const core::Sketch& s = doc().sketch();
    if (m_editing != kNoId && s.hasText(m_editing)) {
        t = s.text(m_editing);  // (keeps its id, anchor, box, path and colour)
    }
    t.text = m_text->toPlainText().toStdString();
    t.font = m_font->family().toStdString();
    t.height = m_height->value();
    if (kind() == Kind::Point) {
        t.angle = m_angle->value();
    }
    t.bold = m_bold->isChecked();
    t.italic = m_italic->isChecked();
    t.spacing = m_spacing->value();
    t.align = static_cast<core::TextAlign>(m_align->currentIndex());
    t.valign = static_cast<core::TextVAlign>(m_valign->currentData().toInt());
    t.flipH = m_flipH->isChecked();
    t.flipV = m_flipV->isChecked();
    t.wrap = kind() == Kind::Box && m_wrap->isChecked();
    t.grow = t.wrap && m_boxHeight->currentIndex() == 1;
    t.outline = static_cast<core::TextOutline>(m_outline->currentIndex());
    t.minThickness = m_thickness->value();
    if (kind() == Kind::Path) {
        if (const auto chain = pathChain()) {
            t.path = chain->front();
            t.pathMore.assign(chain->begin() + 1, chain->end());
            t.anchor = s.definingPoints(t.path).front();
        }
    }
    return t;
}

std::optional<std::vector<Id>> TextTool::pathChain() const
{
    if (picked(m_path).empty()) {
        return std::nullopt;
    }
    try {
        return core::feature::chainOrder(doc().sketch(), picked(m_path));
    }
    catch (const core::SketchError&) {
        return std::nullopt;  // (not one chain: not ready)
    }
}

bool TextTool::corners(core::Vec2& a, core::Vec2& b) const
{
    const core::Sketch& s = doc().sketch();
    auto corner = [&](int input, int k, core::Vec2& out) {
        if (!picked(input).empty()) {
            const core::Point& p = s.point(picked(input).front());
            out = {p.x, p.y};
            return true;
        }
        if (m_hasCorner[k]) {
            out = m_cornerAt[k].pos;
            return true;
        }
        return false;
    };
    if (!corner(m_corner, 0, a)) {
        return false;
    }
    if (!corner(m_opposite, 1, b)) {
        b = mouse();  // following the cursor until it's placed
    }
    return true;
}

bool TextTool::where(core::Vec2& at) const
{
    const core::Sketch& s = doc().sketch();
    switch (kind()) {
        case Kind::Box: {
            core::Vec2 b;
            return corners(at, b);
        }
        case Kind::Path: {
            const auto chain = pathChain();
            if (!chain) {
                return false;
            }
            const core::Point& p = s.point(s.definingPoints(chain->front()).front());
            at = {p.x, p.y};
            return true;
        }
        case Kind::Point:
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
    return false;
}

bool TextTool::pickLocation(int input, const Snap& snap)
{
    if (input == m_corner || input == m_opposite) {
        const int k = input == m_corner ? 0 : 1;
        m_cornerAt[k] = snap;
        m_hasCorner[k] = true;
        setCurrentPick(k == 0 && picked(m_opposite).empty() && !m_hasCorner[1] ? m_opposite : -1);
        return true;
    }
    if (input == m_position) {
        m_location = snap;
        m_hasLocation = true;
        setCurrentPick(-1);
        return true;
    }
    return false;
}

bool TextTool::ready() const
{
    if (m_text->toPlainText().isEmpty()) {
        return false;
    }
    if (kind() == Kind::Box && m_editing == kNoId) {
        return (!picked(m_corner).empty() || m_hasCorner[0]) && (!picked(m_opposite).empty() || m_hasCorner[1]);
    }
    core::Vec2 at;
    return where(at);
}

void TextTool::changed()
{
    if (!m_thickness) {
        return;
    }
    const Kind k = kind();
    for (int pick : {m_corner, m_opposite}) {
        panel()->setRowVisible(pickButton(pick)->parentWidget(), k == Kind::Box);
    }
    panel()->setRowVisible(pickButton(m_path)->parentWidget(), k == Kind::Path);
    panel()->setRowVisible(pickButton(m_position)->parentWidget(), k == Kind::Point);
    panel()->setRowVisible(m_angle, k == Kind::Point);
    panel()->setRowVisible(m_wrap, k == Kind::Box);
    panel()->setRowVisible(m_boxHeight, k == Kind::Box);
    m_boxHeight->setEnabled(m_wrap->isChecked());
    // The vertical choices for the kind (a box has no baseline to sit on).
    const bool boxChoices = m_valign->findData(int(core::TextVAlign::Baseline)) < 0;
    if (boxChoices != (k == Kind::Box)) {
        fillVertical(m_valign, k == Kind::Box, static_cast<core::TextVAlign>(m_valign->currentData().toInt()));
    }
    if (m_editing == kNoId) {
        if (k == Kind::Path && picked(m_path).empty()) {
            setCurrentPick(m_path);
        }
        else if (k == Kind::Box && picked(m_corner).empty() && !m_hasCorner[0]) {
            setCurrentPick(m_corner);
        }
    }
    m_shapeKey.clear();  // (remade on the next paint, for the box as it is then)
    if (m_editing == kNoId) {
        const core::Text t = settings();
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
    const QColor color = theme::canvas().underConstrained;
    core::Text t = settings();
    core::Point origin;  // where the shape's (0, 0) goes, and which way it's turned
    double angle = t.angle;
    std::string key;
    core::Sketch scratch;  // a new box, for laying the text out in before it exists

    if (kind() == Kind::Box && m_editing == kNoId) {
        core::Vec2 a, b;
        if (!corners(a, b)) {
            return;
        }
        const double x0 = std::min(a.x, b.x), x1 = std::max(a.x, b.x), y0 = std::min(a.y, b.y), y1 = std::max(a.y, b.y);
        painter.setPen(QPen(color, 1.0, Qt::DashLine));
        painter.setBrush(Qt::NoBrush);
        painter.drawPolygon(QPolygonF({view().toScreen({x0, y0}), view().toScreen({x1, y0}), view().toScreen({x1, y1}),
                                       view().toScreen({x0, y1})}));
        t.anchor = scratch.addPoint(x0, y0);
        t.boxSide = scratch.addPoint(x1, y0);
        t.boxCorner = scratch.addPoint(x1, y1);
        origin = {x0, y0};
        angle = 0.0;
        key = std::to_string(std::lround((x1 - x0) * 1000)) + 'x' + std::to_string(std::lround((y1 - y0) * 1000));
    }
    else if (kind() == Kind::Point && m_editing == kNoId) {
        core::Vec2 at;
        if (!where(at)) {
            at = mouse();
        }
        origin = {at.x, at.y};
    }
    else {
        core::Vec2 at;
        if (!where(at)) {
            return;  // nothing to follow yet
        }
        if (m_editing != kNoId && t.inBox()) {
            angle = doc().sketch().text(m_editing).angle;
        }
        origin = {at.x, at.y};
    }
    const core::Sketch& layoutIn = (kind() == Kind::Box && m_editing == kNoId) ? scratch : doc().sketch();
    // Remade only when something that shapes the letters changed.
    key += t.text + '\n' + t.font + '\n' + std::to_string(t.height) + (t.bold ? "b" : "") + (t.italic ? "i" : "")
        + std::to_string(static_cast<int>(t.outline)) + ':' + std::to_string(t.minThickness) + ':'
        + std::to_string(t.spacing) + ':' + std::to_string(static_cast<int>(t.align))
        + std::to_string(static_cast<int>(t.valign)) + (t.flipH ? "h" : "") + (t.flipV ? "v" : "") + (t.wrap ? "w" : "") + ':'
        + std::to_string(t.path) + ':' + std::to_string(t.pathMore.size()) + ':' + std::to_string(static_cast<int>(kind()));
    if (key != m_shapeKey) {
        m_shapeKey = key;
        m_shape = (kind() == Kind::Path && t.path == kNoId) ? decltype(m_shape){} : textshapes::make(t, &layoutIn);
    }
    QPainterPath path;
    path.setFillRule(Qt::WindingFill);
    for (const auto& contour : m_shape) {
        QPolygonF polygon;
        for (const core::Point& p : contour) {
            const core::Point w = core::textToWorld(origin, angle, p);
            polygon << view().toScreen({w.x, w.y});
        }
        path.addPolygon(polygon);
        path.closeSubpath();
    }
    QColor fill = color;
    fill.setAlphaF(0.25);
    painter.fillPath(path, fill);
    painter.strokePath(path, QPen(color, 1.0, Qt::DashLine));
}

bool TextTool::commit()
{
    core::Text t = settings();
    if (t.text.empty()) {
        emit view().message(QObject::tr("Text: type something first"));
        return false;
    }
    const Kind k = kind();
    const std::vector<Id> corner = picked(m_corner), opposite = picked(m_opposite), point = picked(m_position);
    const Snap cornerAt = m_cornerAt[0], oppositeAt = m_cornerAt[1], location = m_location;
    return view().tryEdit(QObject::tr("Text"), [&](Editor& ed) {
        if (m_editing != kNoId) {
            ed.setText(t);
            return;
        }
        switch (k) {
            case Kind::Box: {
                const Id a = !corner.empty() ? corner.front() : resolveSnap(ed, cornerAt);
                const Id b = !opposite.empty() ? opposite.front() : resolveSnap(ed, oppositeAt);
                ed.textBox(t, a, b);
                break;
            }
            case Kind::Path: ed.text(t); break;
            case Kind::Point:
                t.anchor = !point.empty() ? point.front() : resolveSnap(ed, location);
                ed.text(t);
                break;
        }
    });
}
