#include "ConstraintPanel.h"

#include "Document.h"
#include "SketchView.h"
#include "Theme.h"

#include <core/Features.h>

#include <QListWidget>
#include <QShortcut>
#include <QVBoxLayout>

#include <algorithm>

namespace
{
QString typeName(core::ConstraintType type)
{
    using T = core::ConstraintType;
    switch (type) {
        case T::Coincident: return QObject::tr("Coincident");
        case T::Horizontal: return QObject::tr("Horizontal");
        case T::Vertical: return QObject::tr("Vertical");
        case T::Parallel: return QObject::tr("Parallel");
        case T::Perpendicular: return QObject::tr("Perpendicular");
        case T::Tangent: return QObject::tr("Tangent");
        case T::Equal: return QObject::tr("Equal");
        case T::Ratio: return QObject::tr("Ratio");
        case T::Midpoint: return QObject::tr("Midpoint");
        case T::Concentric: return QObject::tr("Concentric");
        case T::Symmetric: return QObject::tr("Symmetric");
        case T::Collinear: return QObject::tr("Collinear");
        case T::Distance: return QObject::tr("Distance");
        case T::HorizontalDistance: return QObject::tr("Horizontal distance");
        case T::VerticalDistance: return QObject::tr("Vertical distance");
        case T::Radius: return QObject::tr("Radius");
        case T::Diameter: return QObject::tr("Diameter");
        case T::Angle: return QObject::tr("Angle");
        case T::Offset: return QObject::tr("Offset");
        case T::LinearPattern: return QObject::tr("Rectangular pattern");
        case T::CircularPattern: return QObject::tr("Circular pattern");
        case T::Divide: return QObject::tr("Divide");
    }
    return {};
}

// "3 x 2, spacing 10 / 15 at 0°", "6 over 360°", "5 mm".
QString featureSummary(const core::Constraint& c)
{
    using T = core::ConstraintType;
    auto num = [](double v) { return QString::number(v, 'g', 6); };
    switch (c.type) {
        case T::Offset: return num(c.value);
        case T::LinearPattern: {
            const auto p = core::feature::linearParams(c);
            QString text = QObject::tr("%1, spacing %2").arg(p.count).arg(num(p.spacing));
            if (p.count2 > 1) {
                text = QObject::tr("%1 x %2, spacing %3 / %4").arg(p.count).arg(p.count2).arg(num(p.spacing), num(p.spacing2));
            }
            return p.angle != 0.0 ? text + QObject::tr(" at %1°").arg(num(p.angle)) : text;
        }
        case T::CircularPattern: {
            const auto p = core::feature::circularParams(c);
            return QObject::tr("%1 over %2°").arg(p.count).arg(num(p.angle));
        }
        case T::Divide: {
            const auto p = core::feature::divideParams(c);
            if (!p.byRatio) {
                return QObject::tr("%n point(s) evenly", nullptr, static_cast<int>(p.parts.size()) - 1);
            }
            return QObject::tr("ratio %1%2").arg(QString::fromStdString(core::feature::ratioText(p.parts)),
                                                 p.reversed ? QObject::tr(", reversed") : QString());
        }
        default: return {};
    }
}

bool contains(const std::vector<core::Id>& v, core::Id id)
{
    return std::ranges::find(v, id) != v.end();
}
}  // namespace

ConstraintPanel::ConstraintPanel(Document& doc, SketchView& view, QWidget* parent)
    : QWidget(parent)
    , m_doc(doc)
    , m_view(view)
    , m_list(new QListWidget(this))
{
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->addWidget(m_list);
    m_list->setSelectionMode(QAbstractItemView::ExtendedSelection);
    m_list->setObjectName(QStringLiteral("constraintList"));

    connect(&m_doc, &Document::changed, this, &ConstraintPanel::rebuild);
    connect(&m_view, &SketchView::selectionChanged, this, &ConstraintPanel::pullSelectionFromView);
    connect(m_list, &QListWidget::itemSelectionChanged, this, &ConstraintPanel::pushSelectionToView);
    connect(m_list, &QListWidget::itemDoubleClicked, this, [this](QListWidgetItem* item) {
        const core::Id id = item->data(Qt::UserRole).toUInt();
        if (m_doc.sketch().hasConstraint(id) && core::isFeature(m_doc.sketch().constraint(id).type)) {
            m_view.editFeature(id);
        }
        else {
            m_view.editDimension(id);
        }
    });
    auto* del = new QShortcut(QKeySequence::Delete, m_list, nullptr, nullptr, Qt::WidgetShortcut);
    connect(del, &QShortcut::activated, &m_view, &SketchView::deleteSelection);

    rebuild();
}

QString ConstraintPanel::describe(const core::Constraint& c) const
{
    QString text = typeName(c.type);
    if (core::isDimension(c.type)) {
        text += QStringLiteral("  ") + m_view.formatDimension(c);
    }
    if (c.type == core::ConstraintType::Ratio) {
        text += QStringLiteral("  %1 : 1").arg(QString::number(c.value, 'g', 6));
    }
    const core::Sketch& s = m_doc.sketch();
    if (c.type == core::ConstraintType::Divide) {
        return text + QStringLiteral("  ") + featureSummary(c) + QStringLiteral("   (line %1)").arg(c.refs.front());
    }
    if (core::isFeature(c.type)) {
        const size_t sources = c.type == core::ConstraintType::Offset ? core::feature::offsetRefs(c).sources.size()
                                                                       : core::feature::pattern(c).sources.size();
        return text + QStringLiteral("  ") + featureSummary(c) + QStringLiteral("   (")
            + tr("%n item(s)", nullptr, static_cast<int>(sources)) + QLatin1Char(')');
    }
    QStringList refs;
    for (core::Id r : c.refs) {
        if (s.isOrigin(r)) {
            refs << tr("origin");
        }
        else if (r == s.xAxis()) {
            refs << tr("X axis");
        }
        else if (r == s.yAxis()) {
            refs << tr("Y axis");
        }
        else if (s.hasEntity(r)) {
            refs << QStringLiteral("%1 %2").arg(QString::fromLatin1(core::toString(s.entity(r).kind()))).arg(r);
        }
    }
    return text + QStringLiteral("   (") + refs.join(QStringLiteral(", ")) + QLatin1Char(')');
}

void ConstraintPanel::rebuild()
{
    const core::Sketch& s = m_doc.sketch();
    const core::SolveResult& r = m_doc.solveResult();

    std::vector<Shown> shown;
    for (const auto& [id, c] : s.constraints()) {
        shown.push_back({id, c.value, c.params, contains(r.conflicting, id), contains(r.redundant, id)});
    }
    if (shown == m_shown) {
        return;  // e.g. a drag: geometry moved, constraints didn't change
    }
    m_shown = std::move(shown);

    m_syncing = true;
    m_list->clear();
    for (const Shown& item : m_shown) {
        auto* row = new QListWidgetItem(describe(s.constraint(item.id)), m_list);
        row->setData(Qt::UserRole, item.id);
        if (item.conflicting) {
            row->setForeground(theme::canvas().conflict);
        }
        else if (item.redundant) {
            row->setForeground(theme::canvas().dimension);
            row->setToolTip(tr("Redundant: implied by other constraints"));
        }
    }
    m_syncing = false;
    pullSelectionFromView();
}

void ConstraintPanel::pushSelectionToView()
{
    if (m_syncing) {
        return;
    }
    // Keep selected geometry; replace the constraint part with what the list has selected.
    std::set<core::Id> selection;
    for (core::Id id : m_view.selection()) {
        if (m_doc.sketch().hasEntity(id)) {
            selection.insert(id);
        }
    }
    for (QListWidgetItem* item : m_list->selectedItems()) {
        selection.insert(item->data(Qt::UserRole).toUInt());
    }
    m_syncing = true;
    m_view.setSelection(std::move(selection));
    m_syncing = false;
}

void ConstraintPanel::pullSelectionFromView()
{
    if (m_syncing) {
        return;
    }
    m_syncing = true;
    const auto& selection = m_view.selection();
    for (int i = 0; i < m_list->count(); ++i) {
        QListWidgetItem* item = m_list->item(i);
        item->setSelected(selection.contains(item->data(Qt::UserRole).toUInt()));
    }
    m_syncing = false;
}
