// SketchView: constraint symbols, dimensions and the inline dimension editor.

#include "Document.h"
#include "Icons.h"
#include "NumberInput.h"
#include "SketchView.h"
#include "Theme.h"

#include <core/Curves.h>
#include <core/Features.h>

#include <QFocusEvent>
#include <QKeyEvent>
#include <QLineEdit>
#include <QLocale>
#include <QPainter>
#include <QPainterPath>

#include <cmath>
#include <functional>
#include <map>
#include <numbers>
#include <optional>
#include <set>

using core::ConstraintType;
using core::Id;
using core::kNoId;
using core::Vec2;

namespace
{
constexpr double kGlyphSize = 16.0;
constexpr double kGlyphGap = 18.0;     // spacing between stacked symbols on one entity
constexpr double kGlyphOffset = 14.0;  // distance of symbols from their entity
constexpr double kArrowLength = 9.0;
constexpr double kArrowHalfWidth = 3.0;
constexpr double kRadToDeg = 180.0 / std::numbers::pi;

const QColor kGlyphColor(0xa8, 0xb2, 0xbf);
const QColor kGlyphBackground(0x2a, 0x2d, 0x33, 225);

QString symbolFor(ConstraintType type)
{
    switch (type) {
        case ConstraintType::Coincident: return QStringLiteral("•");     // •
        case ConstraintType::Horizontal: return QStringLiteral("H");
        case ConstraintType::Vertical: return QStringLiteral("V");
        case ConstraintType::Parallel: return QStringLiteral("∥");       // ∥
        case ConstraintType::Perpendicular: return QStringLiteral("⊥");  // ⊥
        case ConstraintType::Tangent: return QStringLiteral("T");
        case ConstraintType::Equal: return QStringLiteral("=");
        case ConstraintType::Midpoint: return QStringLiteral("△");       // △
        case ConstraintType::Concentric: return QStringLiteral("◎");     // ◎
        case ConstraintType::Symmetric: return QStringLiteral("⇔");      // ⇔
        case ConstraintType::Collinear: return QStringLiteral("⋯");      // ⋯
        default: return {};
    }
}

// Which entities carry a symbol for this constraint.
std::vector<Id> symbolTargets(const core::Constraint& c)
{
    switch (c.type) {
        case ConstraintType::Coincident:
        case ConstraintType::Midpoint: return {c.refs[0]};
        case ConstraintType::Symmetric: return {c.refs[0], c.refs[1]};
        default: return c.refs;
    }
}

// Where two tangent curves touch (world coordinates).
std::optional<Vec2> tangentContact(const core::Sketch& s, const core::Constraint& c)
{
    auto ends = [&](Id id) -> std::vector<Id> {
        const core::Entity& e = s.entity(id);
        if (const auto* l = std::get_if<core::Line>(&e.geom)) {
            return {l->p1, l->p2};
        }
        if (const auto* a = std::get_if<core::Arc>(&e.geom)) {
            return {a->start, a->end};
        }
        return {};
    };
    for (Id p : ends(c.refs[0])) {
        for (Id q : ends(c.refs[1])) {
            if (p == q) {
                return Vec2{s.point(p).x, s.point(p).y};
            }
        }
    }
    auto round = [&](Id id, Vec2& centre, double& r) {
        const core::Entity& e = s.entity(id);
        if (const auto* circle = std::get_if<core::Circle>(&e.geom)) {
            centre = {s.point(circle->center).x, s.point(circle->center).y};
            r = circle->radius;
            return true;
        }
        if (const auto* arc = std::get_if<core::Arc>(&e.geom)) {
            centre = {s.point(arc->center).x, s.point(arc->center).y};
            r = s.arcRadius(id);
            return true;
        }
        return false;
    };
    Vec2 c2;
    double r2 = 0;
    if (!round(c.refs[1], c2, r2)) {
        return std::nullopt;
    }
    if (s.entity(c.refs[0]).kind() == core::EntityKind::Line) {
        // Foot of the perpendicular from the centre onto the (infinite) line.
        const core::Line& l = s.line(c.refs[0]);
        const Vec2 a{s.point(l.p1).x, s.point(l.p1).y}, b{s.point(l.p2).x, s.point(l.p2).y};
        const double dx = b.x - a.x, dy = b.y - a.y, l2 = dx * dx + dy * dy;
        if (l2 == 0.0) {
            return std::nullopt;
        }
        const double t = ((c2.x - a.x) * dx + (c2.y - a.y) * dy) / l2;
        return Vec2{a.x + t * dx, a.y + t * dy};
    }
    Vec2 c1;
    double r1 = 0;
    if (!round(c.refs[0], c1, r1)) {
        return std::nullopt;
    }
    const double d = std::hypot(c2.x - c1.x, c2.y - c1.y);
    if (d < 1e-12) {
        return std::nullopt;
    }
    if (d < std::max(r1, r2)) {  // internal: from the big one's centre towards the small one
        const bool firstBig = r1 >= r2;
        const Vec2 big = firstBig ? c1 : c2, small = firstBig ? c2 : c1;
        const double rb = firstBig ? r1 : r2;
        return Vec2{big.x + (small.x - big.x) / d * rb, big.y + (small.y - big.y) / d * rb};
    }
    return Vec2{c1.x + (c2.x - c1.x) / d * r1, c1.y + (c2.y - c1.y) / d * r1};
}

QString formatNumber(double v)
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
    return s;
}

QPointF unit(QPointF v)
{
    const double n = std::hypot(v.x(), v.y());
    return n > 1e-9 ? v / n : QPointF(1, 0);
}

double dotp(QPointF a, QPointF b)
{
    return a.x() * b.x() + a.y() * b.y();
}

void drawArrow(QPainter& painter, QPointF tip, QPointF direction)
{
    const QPointF u = unit(direction);
    const QPointF n(-u.y(), u.x());
    const QPointF base = tip - u * kArrowLength;
    const QPolygonF head{tip, base + n * kArrowHalfWidth, base - n * kArrowHalfWidth};
    painter.save();
    painter.setBrush(painter.pen().color());
    painter.setPen(Qt::NoPen);
    painter.drawPolygon(head);
    painter.restore();
}

// A line edit that reports Enter / Esc / focus loss through callbacks.
class DimensionEditor : public QLineEdit
{
public:
    using QLineEdit::QLineEdit;
    std::function<void()> onAccept;
    std::function<void()> onCancel;

protected:
    void keyPressEvent(QKeyEvent* e) override
    {
        if (e->key() == Qt::Key_Return || e->key() == Qt::Key_Enter) {
            if (onAccept) {
                onAccept();
            }
            return;
        }
        if (e->key() == Qt::Key_Escape) {
            if (onCancel) {
                onCancel();
            }
            return;
        }
        QLineEdit::keyPressEvent(e);
    }
    void focusOutEvent(QFocusEvent* e) override
    {
        QLineEdit::focusOutEvent(e);
        if (onCancel) {
            onCancel();
        }
    }
};
}  // namespace

// ---------------------------------------------------------------------------------------------
// Layout
// ---------------------------------------------------------------------------------------------

QString SketchView::formatDimension(const core::Constraint& c) const
{
    switch (c.type) {
        case ConstraintType::Radius: return QStringLiteral("R") + formatNumber(c.value);
        case ConstraintType::Diameter: return QStringLiteral("⌀") + formatNumber(c.value);  // ⌀
        case ConstraintType::Angle: return formatNumber(c.value) + QStringLiteral("°");
        default: return formatNumber(c.value);
    }
}

std::vector<SketchView::Annotation> SketchView::layoutAnnotations() const
{
    const core::Sketch& s = m_doc.sketch();
    std::vector<Annotation> out;
    std::map<Id, int> stacked;  // symbols already placed per entity
    const QFontMetricsF fm(font());

    auto pos = [&](Id point) {
        const core::Point& p = s.point(point);
        return toScreen({p.x, p.y});
    };

    // Screen position for the next symbol on an entity.
    auto symbolPos = [&](Id entity) -> QPointF {
        const int k = stacked[entity]++;
        const core::Entity& e = s.entity(entity);
        switch (e.kind()) {
            case core::EntityKind::Point: return pos(entity) + QPointF(kGlyphOffset + k * kGlyphGap, kGlyphOffset);
            case core::EntityKind::Line: {
                const auto& l = std::get<core::Line>(e.geom);
                const QPointF a = pos(l.p1), b = pos(l.p2);
                QPointF dir = unit(b - a);
                if (dir.x() < 0) {
                    dir = -dir;  // keep symbols on a consistent side
                }
                const QPointF normal(dir.y(), -dir.x());  // "above" the line on screen
                const QPointF mid = (a + b) / 2;
                return mid + normal * kGlyphOffset + dir * ((k - 0.0) * kGlyphGap + kGlyphGap * 0.75);
            }
            case core::EntityKind::Circle: {
                const auto& c = std::get<core::Circle>(e.geom);
                const QPointF centre = pos(c.center);
                const double r = c.radius * m_zoom;
                return centre + QPointF(k * kGlyphGap, -r - kGlyphOffset);
            }
            case core::EntityKind::Arc: {
                const core::ArcShape a = core::arcShape(s, entity);
                const double mid = a.startAngle + a.sweep / 2;
                const QPointF radial(std::cos(mid), -std::sin(mid));  // screen y is down
                const QPointF tangent(-radial.y(), radial.x());
                return toScreen(a.center) + radial * (a.radius * m_zoom + kGlyphOffset) + tangent * (k * kGlyphGap);
            }
            case core::EntityKind::Ellipse:
            case core::EntityKind::EllipseArc:
            case core::EntityKind::Spline: {
                // Beside the middle of the curve, stacked along it.
                const core::curve::Path p = core::curve::pathOf(s, entity);
                const double mid = p.end() / 2;
                const core::Vec2 d = p.derivative(mid);
                QPointF along(d.x, -d.y);  // screen y is down
                const double len = std::hypot(along.x(), along.y());
                along = len > 0 ? along / len : QPointF(1, 0);
                const QPointF normal(along.y(), -along.x());
                return toScreen(p.at(mid)) + normal * kGlyphOffset + along * (k * kGlyphGap);
            }
        }
        return {};
    };

    std::set<std::pair<Id, QString>> shownOn;  // (entity, symbol) pairs already drawn
    for (const auto& [id, c] : s.constraints()) {
        if (c.sublayer != kNoId && c.sublayer != s.activeSublayer()) {
            continue;  // only the active sublayer's constraints and dimensions show
        }
        if (core::isDimension(c.type)) {
            Annotation a;
            a.id = id;
            a.isDimension = true;
            a.dimension = core::layoutDimension(s, c);
            if (!a.dimension.valid) {
                continue;
            }
            a.text = formatDimension(c);
            const QSizeF size(fm.horizontalAdvance(a.text) + 10, fm.height() + 4);
            a.rect = QRectF(toScreen(a.dimension.label) - QPointF(size.width() / 2, size.height() / 2), size);
            out.push_back(std::move(a));
            continue;
        }

        if (!m_showConstraints) {
            continue;
        }
        auto glyphAt = [&](QPointF at, const QString& symbol, const QString& icon = {}) {
            Annotation a{id, false, {}, symbol, {}, icon};
            a.rect = QRectF(at - QPointF(kGlyphSize / 2, kGlyphSize / 2), QSizeF(kGlyphSize, kGlyphSize));
            out.push_back(std::move(a));
        };
        if (core::isFeature(c.type)) {
            // One symbol, its tool's icon, on what it made (a divide: on its line).
            Id on = kNoId;
            QString icon;
            switch (c.type) {
                case ConstraintType::Offset:
                    on = core::feature::offsetRefs(c).offsets.front();
                    icon = QStringLiteral("offset");
                    break;
                case ConstraintType::LinearPattern:
                case ConstraintType::CircularPattern: {
                    const auto p = core::feature::pattern(c);
                    on = p.copies.empty() ? p.sources.front() : p.copies.front().front();
                    icon = c.type == ConstraintType::LinearPattern ? QStringLiteral("rectangular_pattern")
                                                                   : QStringLiteral("circular_pattern");
                    break;
                }
                case ConstraintType::Divide:
                    on = c.refs.front();
                    icon = QStringLiteral("divide");
                    break;
                default: break;
            }
            if (on != kNoId && s.hasEntity(on)) {
                glyphAt(symbolPos(on), {}, icon);
            }
            continue;
        }
        if (c.type == ConstraintType::Ratio) {
            // Each side's share: "1.5" on the first, "1" on the second.
            const QString shares[2] = {formatNumber(c.value), QStringLiteral("1")};
            for (int i = 0; i < 2; ++i) {
                Annotation a{id, false, {}, shares[i], {}, {}};
                const double w = std::max(kGlyphSize, fm.horizontalAdvance(shares[i]) + 8.0);
                a.rect = QRectF(symbolPos(c.refs[static_cast<size_t>(i)]) - QPointF(w / 2, kGlyphSize / 2),
                                QSizeF(w, kGlyphSize));
                out.push_back(std::move(a));
            }
            continue;
        }
        const QString symbol = symbolFor(c.type);
        const bool pointPair = (c.type == ConstraintType::Horizontal || c.type == ConstraintType::Vertical)
            && c.refs.size() == 2;
        if (pointPair) {
            glyphAt((pos(c.refs[0]) + pos(c.refs[1])) / 2 + QPointF(0, -kGlyphOffset), symbol);
            continue;
        }
        if (c.type == ConstraintType::Tangent) {
            // One symbol, at the point where the curves touch.
            if (const auto touch = tangentContact(s, c)) {
                glyphAt(toScreen(*touch) + QPointF(kGlyphOffset * 0.8, -kGlyphOffset * 0.8), symbol);
                continue;
            }
        }
        for (Id target : symbolTargets(c)) {
            // An item made equal (say) to five others shows one "=", not five.
            if (!shownOn.insert({target, symbol}).second) {
                continue;
            }
            glyphAt(symbolPos(target), symbol);
        }
    }

    // Fixed items: a padlock each (selecting it selects the item; F unfixes it).
    if (m_showConstraints) {
        for (const auto& [id, e] : s.entities()) {
            if (e.fixed && !s.isReference(id) && e.sublayer == s.activeSublayer() && e.projectedFrom == kNoId) {
                Annotation a{id, false, {}, {}, {}, QStringLiteral("fix")};
                a.rect = QRectF(symbolPos(id) - QPointF(kGlyphSize / 2, kGlyphSize / 2), QSizeF(kGlyphSize, kGlyphSize));
                out.push_back(std::move(a));
            }
        }
    }
    return out;
}

QColor SketchView::annotationColor(Id constraint, const QColor& normal) const
{
    const auto& conflicting = m_doc.solveResult().conflicting;
    if (m_selection.contains(constraint)) {
        return theme::canvas().selection;
    }
    if (constraint == m_hovered) {
        return theme::canvas().hover;
    }
    if (std::ranges::find(conflicting, constraint) != conflicting.end()) {
        return theme::canvas().conflict;
    }
    return normal;
}

// ---------------------------------------------------------------------------------------------
// Drawing
// ---------------------------------------------------------------------------------------------

void SketchView::drawAnnotations(QPainter& painter) const
{
    for (const Annotation& a : layoutAnnotations()) {
        if (a.isDimension) {
            drawDimension(painter, a.dimension, a.text, annotationColor(a.id, theme::canvas().dimension));
            continue;
        }
        const QColor color = annotationColor(a.id, kGlyphColor);
        painter.setPen(QPen(color, 1.0));
        painter.setBrush(kGlyphBackground);
        painter.drawRoundedRect(a.rect, 3, 3);
        painter.setBrush(Qt::NoBrush);
        if (!a.icon.isEmpty()) {
            icons::forName(a.icon).paint(&painter, a.rect.adjusted(1.5, 1.5, -1.5, -1.5).toRect());
            continue;
        }
        painter.drawText(a.rect, Qt::AlignCenter, a.text);
    }
}

void SketchView::drawDimension(QPainter& painter, const core::DimensionLayout& L, const QString& text,
                               const QColor& color, QRectF* textRect) const
{
    using Style = core::DimensionLayout::Style;
    painter.save();
    painter.setPen(QPen(color, 1.0));
    painter.setBrush(Qt::NoBrush);
    const QPointF label = toScreen(L.label);

    switch (L.style) {
        case Style::Linear: {
            const QPointF a = toScreen(L.a), b = toScreen(L.b);
            // Measuring direction on screen (y flipped) and its normal.
            const QPointF dir = unit(QPointF(L.dir.x, -L.dir.y));
            const QPointF n(-dir.y(), dir.x());
            const QPointF a2 = a + n * dotp(label - a, n);
            const QPointF b2 = b + n * dotp(label - b, n);
            // Extension lines, overshooting the dimension line slightly.
            const auto overshoot = [&](QPointF from, QPointF to) { return to + unit(to - from) * 4.0; };
            if (std::hypot((a2 - a).x(), (a2 - a).y()) > 1.0) {
                painter.drawLine(a, overshoot(a, a2));
            }
            if (std::hypot((b2 - b).x(), (b2 - b).y()) > 1.0) {
                painter.drawLine(b, overshoot(b, b2));
            }
            painter.drawLine(a2, b2);
            // Extend towards a label placed beyond the ends.
            const double t = dotp(label - a2, unit(b2 - a2));
            const double len = std::hypot((b2 - a2).x(), (b2 - a2).y());
            if (t < 0) {
                painter.drawLine(a2, a2 + unit(b2 - a2) * t);
            }
            else if (t > len) {
                painter.drawLine(b2, a2 + unit(b2 - a2) * t);
            }
            if (len > 1.0) {
                drawArrow(painter, a2, a2 - b2);
                drawArrow(painter, b2, b2 - a2);
            }
            break;
        }
        case Style::Radius:
        case Style::Diameter: {
            const QPointF c = toScreen(L.center);
            const QPointF u = unit(label - c);
            const double r = L.radius * m_zoom;
            const QPointF rim = c + u * r;
            const QPointF far = L.style == Style::Diameter ? c - u * r : c;
            const double toLabel = std::hypot((label - c).x(), (label - c).y());
            painter.drawLine(far, toLabel > r ? label : rim);
            drawArrow(painter, rim, u);
            if (L.style == Style::Diameter) {
                drawArrow(painter, far, -u);
            }
            break;
        }
        case Style::Angle: {
            const QPointF c = toScreen(L.center);
            const double r = L.radius * m_zoom;
            const QRectF box(c.x() - r, c.y() - r, 2 * r, 2 * r);
            QPainterPath path;
            path.arcMoveTo(box, L.startAngle * kRadToDeg);
            path.arcTo(box, L.startAngle * kRadToDeg, L.sweep * kRadToDeg);
            painter.drawPath(path);
            // Arrowheads along the arc's tangent at both ends.
            const double a0 = L.startAngle, a1 = L.startAngle + L.sweep;
            const double sign = L.sweep >= 0 ? 1.0 : -1.0;
            const QPointF p0 = c + QPointF(std::cos(a0), -std::sin(a0)) * r;
            const QPointF p1 = c + QPointF(std::cos(a1), -std::sin(a1)) * r;
            drawArrow(painter, p0, QPointF(std::sin(a0), std::cos(a0)) * sign);
            drawArrow(painter, p1, QPointF(-std::sin(a1), -std::cos(a1)) * sign);
            break;
        }
    }

    // Value label.
    const QFontMetricsF fm(font());
    const QSizeF size(fm.horizontalAdvance(text) + 10, fm.height() + 4);
    const QRectF box(label - QPointF(size.width() / 2, size.height() / 2), size);
    QColor bg = theme::canvas().background;
    bg.setAlpha(230);
    painter.setPen(Qt::NoPen);
    painter.setBrush(bg);
    painter.drawRoundedRect(box, 3, 3);
    painter.setBrush(Qt::NoBrush);
    painter.setPen(color);
    painter.drawText(box, Qt::AlignCenter, text);
    painter.restore();
    if (textRect) {
        *textRect = box;
    }
}

// ---------------------------------------------------------------------------------------------
// Inline dimension editor
// ---------------------------------------------------------------------------------------------

void SketchView::editDimension(Id id)
{
    const core::Sketch& s = m_doc.sketch();
    if (!s.hasConstraint(id)) {
        return;
    }
    if (s.constraint(id).type == ConstraintType::Ratio) {
        // At its first symbol (the one showing the value).
        for (const Annotation& a : layoutAnnotations()) {
            if (a.id == id) {
                editValue(id, a.rect.center());
                return;
            }
        }
        editValue(id, rect().center());  // (symbols hidden)
        return;
    }
    if (!core::isDimension(s.constraint(id).type)) {
        return;
    }
    editValue(id, toScreen(core::layoutDimension(s, s.constraint(id)).label));
}

void SketchView::editText(Id id)
{
    if (m_doc.sketch().hasText(id)) {
        m_editFeature = id;
        setTool(ToolKind::Text);
    }
}

void SketchView::editFeature(Id id)
{
    const core::Sketch& s = m_doc.sketch();
    if (!s.hasConstraint(id) || !core::isFeature(s.constraint(id).type)) {
        return;
    }
    const core::Constraint& c = s.constraint(id);
    if (c.type == ConstraintType::Offset) {
        const std::vector<Vec2> along = core::sample(s, core::feature::offsetRefs(c).offsets.front(), 2);
        editValue(id, toScreen(along[along.size() / 2]));
        return;
    }

    // Patterns and divides: their command, editing them.
    m_editFeature = id;
    switch (c.type) {
        case ConstraintType::CircularPattern: setTool(ToolKind::CircularPattern); break;
        case ConstraintType::Divide: setTool(ToolKind::Divide); break;
        default: setTool(ToolKind::RectangularPattern); break;
    }
}

void SketchView::editValue(Id id, QPointF at)
{
    const core::Sketch& s = m_doc.sketch();
    if (!s.hasConstraint(id) || !core::hasValue(s.constraint(id).type)) {
        return;
    }
    if (m_editor) {
        m_editor->deleteLater();
        m_editor = nullptr;
    }

    const core::Constraint& c = s.constraint(id);
    auto* editor = new DimensionEditor(this);
    m_editor = editor;
    editor->setObjectName(QStringLiteral("dimensionEditor"));
    editor->setText(formatNumber(c.value));
    editor->setAlignment(Qt::AlignCenter);
    editor->resize(90, editor->sizeHint().height());
    editor->move((at - QPointF(45, editor->height() / 2.0)).toPoint());

    // Idempotent: Enter closes, and the resulting focus loss calls it again.
    auto close = [this, editor] {
        if (m_editor == editor) {
            m_editor = nullptr;
            editor->hide();
            editor->deleteLater();
            setFocus();
        }
    };
    editor->onCancel = close;
    editor->onAccept = [this, editor, id, close] {
        double value = 0.0;
        QString error;
        if (!readNumber(editor->text(), value, &error)) {
            emit message(tr("Can't work out \"%1\": %2").arg(editor->text().trimmed(), error));
        }
        else if (!m_doc.setDimensionValue(id, value, &error)) {
            emit message(error);
        }
        close();
    };

    editor->show();
    editor->setFocus();
    editor->selectAll();
}
