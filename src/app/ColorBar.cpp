#include "ColorBar.h"

#include "Document.h"
#include "NumberInput.h"
#include "SketchView.h"

#include <QColorDialog>
#include <QComboBox>
#include <QGridLayout>
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QLineEdit>
#include <QMouseEvent>
#include <QPainter>
#include <QToolButton>

#include <algorithm>
#include <functional>

namespace
{
QColor toQt(const core::Color& c)
{
    return {c.r, c.g, c.b, c.a};
}

core::Color fromQt(const QColor& c)
{
    return {static_cast<std::uint8_t>(c.red()), static_cast<std::uint8_t>(c.green()),
            static_cast<std::uint8_t>(c.blue()), static_cast<std::uint8_t>(c.alpha())};
}

// MS Paint's palette: dark/strong colours on top, their lighter partners below.
const char* const kPalette[2][10] = {
    {"#000000", "#7f7f7f", "#880015", "#ed1c24", "#ff7f27", "#fff200", "#22b14c", "#00a2e8", "#3f48cc", "#a349a4"},
    {"#ffffff", "#c3c3c3", "#b97a57", "#ffaec9", "#ffc90e", "#efe4b0", "#b5e61d", "#99d9ea", "#7092be", "#c8bfe7"},
};

// Line widths (mm): ISO pen sizes.
// (None first: no line at all; then the hairline, 1 pixel.)
const double kWidths[] = {core::StrokeStyle::kNone, 0.0, 0.13, 0.18, 0.25, 0.35, 0.5, 0.7, 1.0, 1.4, 2.0};

QString widthText(double w)
{
    if (w < 0.0) {
        return QObject::tr("None");
    }
    return w == 0.0 ? QObject::tr("Hairline") : QStringLiteral("%1 mm").arg(w);
}

// A dash pattern sample, for the line-style list.
QIcon dashIcon(core::Dash dash)
{
    QPixmap pm(44, 12);
    pm.fill(Qt::transparent);
    QPainter p(&pm);
    QPen pen(QColor(0xe6, 0xeb, 0xf0), 2.0);
    pen.setCapStyle(Qt::FlatCap);
    switch (dash) {
        case core::Dash::Solid: break;
        case core::Dash::Dashed: pen.setDashPattern({5, 3}); break;
        case core::Dash::Dotted: pen.setDashPattern({1, 2}); break;
        case core::Dash::DashDot: pen.setDashPattern({8, 3, 1, 3}); break;
        case core::Dash::DashDotDot: pen.setDashPattern({8, 3, 1, 3, 1, 3}); break;
    }
    p.setPen(pen);
    p.drawLine(QPointF(1, 6), QPointF(43, 6));
    return QIcon(pm);
}
}  // namespace

// A clickable colour square. Empty (no colour yet) custom slots show as a dark outline.
class Swatch : public QWidget
{
public:
    explicit Swatch(QWidget* parent, int size = 14)
        : QWidget(parent)
    {
        setFixedSize(size, size);
        setCursor(Qt::PointingHandCursor);
    }
    void setColor(const core::Color& c)
    {
        m_color = c;
        m_empty = false;
        setToolTip(QString::fromStdString(c.hex()));
        update();
    }
    bool isEmpty() const { return m_empty; }
    const core::Color& color() const { return m_color; }
    std::function<void(Qt::MouseButton)> onClick;

protected:
    void paintEvent(QPaintEvent*) override
    {
        QPainter p(this);
        p.setPen(QColor(0x55, 0x5c, 0x66));
        p.setBrush(m_empty ? QColor(0x26, 0x29, 0x2e) : toQt(m_color));
        p.drawRect(rect().adjusted(0, 0, -1, -1));
    }
    void mousePressEvent(QMouseEvent* e) override
    {
        if (!m_empty && onClick) {
            onClick(e->button());
        }
    }

private:
    core::Color m_color;
    bool m_empty = true;
};

// Colour 1 in front of Colour 2, as in Paint. Clicking either edits it.
class CurrentColors : public QWidget
{
public:
    explicit CurrentColors(QWidget* parent)
        : QWidget(parent)
    {
        setFixedSize(34, 30);
        setToolTip(QObject::tr("Colour 1 (strokes) and Colour 2 (fills): click to edit"));
        setCursor(Qt::PointingHandCursor);
    }
    void setColors(const core::Color& c1, const core::Color& c2)
    {
        m_c1 = c1;
        m_c2 = c2;
        update();
    }
    std::function<void(bool second)> onEdit;

protected:
    void paintEvent(QPaintEvent*) override
    {
        QPainter p(this);
        p.setPen(QColor(0x8a, 0x92, 0x9c));
        p.setBrush(toQt(m_c2));
        p.drawRect(back());
        p.setBrush(toQt(m_c1));
        p.drawRect(front());
    }
    void mousePressEvent(QMouseEvent* e) override
    {
        if (onEdit) {
            onEdit(!front().contains(e->position().toPoint()));
        }
    }

private:
    static QRect front() { return {1, 1, 20, 18}; }
    static QRect back() { return {12, 10, 20, 18}; }
    core::Color m_c1, m_c2;
};

// ---------------------------------------------------------------------------------------------

ColorBar::ColorBar(SketchView& view, QWidget* parent)
    : QWidget(parent)
    , m_view(view)
{
    setObjectName(QStringLiteral("colorBar"));
    auto* row = new QHBoxLayout(this);
    row->setContentsMargins(4, 0, 4, 0);
    row->setSpacing(6);

    m_current = new CurrentColors(this);
    m_current->setObjectName(QStringLiteral("currentColors"));
    m_current->onEdit = [this](bool second) { editColor(second); };
    row->addWidget(m_current);

    // Palette (two rows) and custom colours (one row) in one grid.
    auto* grid = new QGridLayout;
    grid->setSpacing(2);
    for (int r = 0; r < 2; ++r) {
        for (int c = 0; c < 10; ++c) {
            auto* swatch = new Swatch(this);
            core::Color color;
            core::Color::fromHex(kPalette[r][c], color);
            swatch->setColor(color);
            swatch->onClick = [this, color](Qt::MouseButton b) { pickColor(color, b == Qt::RightButton); };
            grid->addWidget(swatch, r, c);
        }
    }
    for (int c = 0; c < 10; ++c) {
        auto* swatch = new Swatch(this, 10);
        swatch->setToolTip(tr("Custom colours appear here"));
        swatch->onClick = [this, swatch](Qt::MouseButton b) { pickColor(swatch->color(), b == Qt::RightButton); };
        grid->addWidget(swatch, 2, c, Qt::AlignCenter);
        m_custom.push_back(swatch);
    }
    row->addLayout(grid);

    auto* edit = new QToolButton(this);
    edit->setObjectName(QStringLiteral("editColors"));
    edit->setText(tr("Edit colours"));
    edit->setToolTip(tr("Pick any colour for Colour 1 (Shift: Colour 2)"));
    connect(edit, &QToolButton::clicked, this,
            [this] { editColor(QGuiApplication::keyboardModifiers() & Qt::ShiftModifier); });
    row->addWidget(edit);

    m_width = new QComboBox(this);
    m_width->setObjectName(QStringLiteral("strokeWidth"));
    m_width->setToolTip(tr("Line width on the drawing (type any size in mm). None: no line in the finished drawing, e.g. for a fill without an outline"));
    m_width->setEditable(true);
    m_width->setInsertPolicy(QComboBox::NoInsert);
    for (double w : kWidths) {
        m_width->addItem(widthText(w), w);
    }
    auto applyWidth = [this] {
        if (m_syncing) {
            return;
        }
        const QString text = m_width->currentText().trimmed();
        double w = 0.0;
        QString bare = text;
        if (bare.endsWith(QStringLiteral("mm"))) {
            bare.chop(2);  // as the list shows them: "0.5 mm"
        }
        bool ok = readNumber(bare, w);
        if (text.compare(tr("Hairline"), Qt::CaseInsensitive) == 0) {
            w = 0.0;
            ok = true;
        }
        if (text.compare(tr("None"), Qt::CaseInsensitive) == 0) {
            w = core::StrokeStyle::kNone;
            ok = true;
        }
        if (ok && (w >= 0.0 || w == core::StrokeStyle::kNone)) {
            setWidth(w);
        }
    };
    connect(m_width, &QComboBox::activated, this, applyWidth);
    connect(m_width->lineEdit(), &QLineEdit::editingFinished, this, applyWidth);
    row->addWidget(m_width);

    m_dash = new QComboBox(this);
    m_dash->setObjectName(QStringLiteral("strokeDash"));
    m_dash->setToolTip(tr("Line style"));
    m_dash->setIconSize(QSize(44, 12));
    const std::pair<core::Dash, QString> dashes[] = {
        {core::Dash::Solid, tr("Solid")},       {core::Dash::Dashed, tr("Dashed")},
        {core::Dash::Dotted, tr("Dotted")},     {core::Dash::DashDot, tr("Dash dot")},
        {core::Dash::DashDotDot, tr("Dash dot dot")},
    };
    for (const auto& [dash, name] : dashes) {
        m_dash->addItem(dashIcon(dash), name, static_cast<int>(dash));
    }
    connect(m_dash, &QComboBox::activated, this, [this](int index) {
        if (!m_syncing) {
            setDash(static_cast<core::Dash>(m_dash->itemData(index).toInt()));
        }
    });
    row->addWidget(m_dash);

    auto* reset = new QToolButton(this);
    reset->setObjectName(QStringLiteral("resetStroke"));
    reset->setText(tr("Reset stroke"));
    reset->setToolTip(tr("Take the strokes off the selected curves (they draw as plain geometry again)"));
    connect(reset, &QToolButton::clicked, this, &ColorBar::resetSelection);
    row->addWidget(reset);

    connect(&m_view, &SketchView::paintChanged, this, &ColorBar::syncFromView);
    connect(&m_view, &SketchView::selectionChanged, this, &ColorBar::syncFromSelection);
    syncFromView();
}

void ColorBar::syncFromView()
{
    const SketchView::Paint& p = m_view.paint();
    m_syncing = true;
    m_current->setColors(p.color1, p.color2);
    const int w = m_width->findData(p.width);
    if (w >= 0) {
        m_width->setCurrentIndex(w);
    }
    else {
        m_width->setEditText(widthText(p.width));
    }
    m_dash->setCurrentIndex(m_dash->findData(static_cast<int>(p.dash)));
    m_syncing = false;
}

void ColorBar::syncFromSelection()
{
    const core::Sketch& s = m_view.document().sketch();
    for (core::Id id : m_view.selection()) {
        if (const core::StrokeStyle* style = s.hasEntity(id) ? s.stroke(id) : nullptr) {
            SketchView::Paint p = m_view.paint();
            p.color1 = style->color;
            p.width = style->width;
            p.dash = style->dash;
            m_view.setPaint(p);  // shown, not applied: nothing was edited
            return;
        }
    }
}

void ColorBar::pickColor(const core::Color& color, bool second)
{
    if (m_view.offerColor(color, second)) {
        return;  // the tool took it (the gradient tool: for its selected stop)
    }
    SketchView::Paint p = m_view.paint();
    (second ? p.color2 : p.color1) = color;
    m_view.setPaint(p);
    if (!second) {
        applyToSelection(ColorPart);
    }
}

void ColorBar::setWidth(double width)
{
    SketchView::Paint p = m_view.paint();
    p.width = width;
    m_view.setPaint(p);
    applyToSelection(WidthPart);
}

void ColorBar::setDash(core::Dash dash)
{
    SketchView::Paint p = m_view.paint();
    p.dash = dash;
    m_view.setPaint(p);
    applyToSelection(DashPart);
}

void ColorBar::editColor(bool second)
{
    const SketchView::Paint& p = m_view.paint();
    const QColor chosen = QColorDialog::getColor(toQt(second ? p.color2 : p.color1), this,
                                                 second ? tr("Colour 2") : tr("Colour 1"),
                                                 QColorDialog::ShowAlphaChannel);
    if (!chosen.isValid()) {
        return;
    }
    const core::Color color = fromQt(chosen);
    addCustom(color);
    pickColor(color, second);
}

void ColorBar::addCustom(const core::Color& color)
{
    std::erase(m_customColors, color);
    m_customColors.insert(m_customColors.begin(), color);  // most recent first
    if (m_customColors.size() > m_custom.size()) {
        m_customColors.resize(m_custom.size());
    }
    for (size_t i = 0; i < m_customColors.size(); ++i) {
        m_custom[i]->setColor(m_customColors[i]);
    }
}

void ColorBar::resetSelection()
{
    const core::Sketch& s = m_view.document().sketch();
    std::vector<core::Id> curves;
    for (core::Id id : m_view.selection()) {
        if (s.hasEntity(id) && s.stroke(id)) {
            curves.push_back(id);
        }
    }
    if (curves.empty()) {
        emit m_view.message(tr("Select the curves to reset first (or Ctrl+click them with the stroke brush)"));
        return;
    }
    m_view.tryEdit(tr("Remove stroke"), [&](Editor& ed) { ed.clearStroke(curves); });
}

void ColorBar::applyToSelection(int parts)
{
    const core::Sketch& s = m_view.document().sketch();
    const core::StrokeStyle paint = m_view.paint().stroke();
    // Each selected curve keeps what wasn't changed; curves that come out alike go together.
    std::vector<std::pair<core::StrokeStyle, std::vector<core::Id>>> groups;
    for (core::Id id : m_view.selection()) {
        if (!s.hasEntity(id) || s.isReference(id) || s.entity(id).kind() == core::EntityKind::Point
            || s.entity(id).construction) {
            continue;
        }
        core::StrokeStyle style = s.stroke(id) ? *s.stroke(id) : paint;
        if (parts & ColorPart) {
            style.color = paint.color;
            style.gradient = {};  // a plain colour replaces a gradient
        }
        if (parts & WidthPart) {
            style.width = paint.width;
        }
        if (parts & DashPart) {
            style.dash = paint.dash;
        }
        if (s.stroke(id) && *s.stroke(id) == style) {
            continue;
        }
        auto it = std::ranges::find_if(groups, [&](const auto& g) { return g.first == style; });
        if (it == groups.end()) {
            groups.push_back({style, {id}});
        }
        else {
            it->second.push_back(id);
        }
    }
    if (groups.empty()) {
        return;
    }
    m_view.tryEdit(tr("Stroke"), [&](Editor& ed) {
        for (const auto& [style, curves] : groups) {
            ed.setStroke(curves, style);
        }
    });
}
