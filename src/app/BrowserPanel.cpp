#include "BrowserPanel.h"

#include "Document.h"
#include "SketchView.h"
#include "Theme.h"

#include <QHBoxLayout>
#include <QHeaderView>
#include <QMenu>
#include <QMessageBox>
#include <QPainter>
#include <QPainterPath>
#include <QToolButton>
#include <QTreeWidget>
#include <QVBoxLayout>

using core::Id;

namespace
{
enum Column
{
    kEye,
    kActive,
    kName,
};
constexpr int kIdRole = Qt::UserRole;

// Small icons drawn here: an eye (open, or shut with a slash) and the active marker (a filled dot,
// or a ring for the others).
QIcon eyeIcon(bool open)
{
    QPixmap pm(32, 32);
    pm.fill(Qt::transparent);
    QPainter p(&pm);
    p.setRenderHint(QPainter::Antialiasing);
    const QColor c = open ? QColor(0xd5, 0xd9, 0xde) : QColor(0xd5, 0xd9, 0xde, 110);
    p.setPen(QPen(c, 2.6));
    QPainterPath lids;
    lids.moveTo(4, 16);
    lids.quadTo(16, 4, 28, 16);
    lids.quadTo(16, 28, 4, 16);
    p.drawPath(lids);
    if (open) {
        p.setBrush(c);
        p.drawEllipse(QPointF(16, 16), 4.2, 4.2);
    }
    else {
        p.drawLine(QPointF(7, 26), QPointF(25, 6));
    }
    return QIcon(pm);
}

QIcon activeIcon(bool active)
{
    QPixmap pm(32, 32);
    pm.fill(Qt::transparent);
    QPainter p(&pm);
    p.setRenderHint(QPainter::Antialiasing);
    const QColor c(0x4d, 0xa3, 0xff);
    p.setPen(QPen(active ? c : QColor(0xd5, 0xd9, 0xde, 140), 2.4));
    p.setBrush(Qt::NoBrush);
    p.drawEllipse(QPointF(16, 16), 9, 9);
    if (active) {
        p.setPen(Qt::NoPen);
        p.setBrush(c);
        p.drawEllipse(QPointF(16, 16), 5, 5);
    }
    return QIcon(pm);
}

// How many things a sublayer holds (geometry, constraints, texts), for its tooltip.
QString contents(const core::Sketch& s, Id sublayer)
{
    int curves = 0, points = 0, constraints = 0, texts = 0;
    for (const auto& [id, e] : s.entities()) {
        if (e.sublayer == sublayer) {
            (e.kind() == core::EntityKind::Point ? points : curves)++;
        }
    }
    for (const auto& [id, c] : s.constraints()) {
        constraints += c.sublayer == sublayer;
    }
    for (const auto& [id, t] : s.texts()) {
        texts += t.sublayer == sublayer;
    }
    return QObject::tr("%1 curves, %2 points, %3 constraints, %4 texts").arg(curves).arg(points).arg(constraints).arg(texts);
}
}  // namespace

BrowserPanel::BrowserPanel(Document& doc, SketchView& view, QWidget* parent)
    : QWidget(parent)
    , m_doc(doc)
    , m_view(view)
    , m_tree(new QTreeWidget(this))
{
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(2);
    auto* bar = new QHBoxLayout;
    bar->setContentsMargins(4, 2, 4, 0);
    auto* add = new QToolButton(this);
    add->setObjectName(QStringLiteral("addSublayer"));
    add->setText(QStringLiteral("+"));
    add->setToolTip(tr("New sublayer (it becomes the one being edited)"));
    add->setAutoRaise(true);
    bar->addWidget(add);
    bar->addStretch();
    layout->addLayout(bar);
    layout->addWidget(m_tree);

    m_tree->setObjectName(QStringLiteral("browser"));
    m_tree->setColumnCount(3);
    m_tree->setHeaderHidden(true);
    m_tree->setRootIsDecorated(true);
    m_tree->setIconSize(QSize(16, 16));
    m_tree->setContextMenuPolicy(Qt::CustomContextMenu);
    m_tree->setEditTriggers(QAbstractItemView::DoubleClicked | QAbstractItemView::EditKeyPressed);
    m_tree->header()->setStretchLastSection(true);
    m_tree->header()->setSectionResizeMode(kEye, QHeaderView::ResizeToContents);
    m_tree->header()->setSectionResizeMode(kActive, QHeaderView::ResizeToContents);

    connect(add, &QToolButton::clicked, this, &BrowserPanel::addSublayer);
    connect(&m_doc, &Document::changed, this, &BrowserPanel::rebuild);
    connect(m_tree, &QTreeWidget::customContextMenuRequested, this, &BrowserPanel::showMenu);
    connect(m_tree, &QTreeWidget::itemClicked, this, [this](QTreeWidgetItem* item, int column) {
        if (m_rebuilding || item == m_layer || !item) {
            return;
        }
        const Id id = item->data(kName, kIdRole).toUInt();
        if (column == kEye) {
            m_doc.setSublayerVisible(id, !m_doc.sketch().sublayer(id).visible);
        }
        else {
            m_doc.setActiveSublayer(id);  // the marker or the name: edit this one
        }
    });
    connect(m_tree, &QTreeWidget::itemChanged, this, [this](QTreeWidgetItem* item, int column) {
        if (m_rebuilding || item == m_layer || column != kName) {
            return;
        }
        const Id id = item->data(kName, kIdRole).toUInt();
        const QString name = item->text(kName).trimmed();
        if (name.isEmpty() || name.toStdString() == m_doc.sketch().sublayer(id).name) {
            rebuild();
            return;
        }
        m_view.tryEdit(tr("Rename sublayer"), [&](Editor& ed) { ed.renameSublayer(id, name); });
    });
    rebuild();
}

QTreeWidgetItem* BrowserPanel::rowFor(Id sublayer) const
{
    for (int i = 0; m_layer && i < m_layer->childCount(); ++i) {
        if (m_layer->child(i)->data(kName, kIdRole).toUInt() == sublayer) {
            return m_layer->child(i);
        }
    }
    return nullptr;
}

void BrowserPanel::rebuild()
{
    const core::Sketch& s = m_doc.sketch();
    const auto& sublayers = s.sublayers();
    const bool same = sublayers.size() == m_shown.size() && s.activeSublayer() == m_shownActive
        && std::ranges::equal(sublayers, m_shown, [](const core::Sublayer& a, const core::Sublayer& b) {
               return a.id == b.id && a.name == b.name && a.visible == b.visible;
           });
    if (same && m_layer) {
        for (int i = 0; i < m_layer->childCount(); ++i) {  // (just the counts)
            QTreeWidgetItem* row = m_layer->child(i);
            row->setToolTip(kName, contents(s, row->data(kName, kIdRole).toUInt()));
        }
        return;
    }
    m_shown = sublayers;
    m_shownActive = s.activeSublayer();

    m_rebuilding = true;
    m_tree->clear();
    m_layer = new QTreeWidgetItem(m_tree);
    m_layer->setText(kName, tr("Layer 1"));
    m_layer->setToolTip(kName, tr("The drawing (layers come later); its sublayers are edited one at a time, "
                                  "and coloured as one"));
    m_layer->setFlags(Qt::ItemIsEnabled);
    QFont layerFont = m_layer->font(kName);
    layerFont.setBold(true);
    m_layer->setFont(kName, layerFont);
    m_layer->setFirstColumnSpanned(false);
    for (const core::Sublayer& l : sublayers) {
        auto* row = new QTreeWidgetItem(m_layer);
        const bool active = l.id == s.activeSublayer();
        row->setData(kName, kIdRole, l.id);
        row->setText(kName, QString::fromStdString(l.name));
        row->setIcon(kEye, eyeIcon(l.visible));
        row->setToolTip(kEye, l.visible ? tr("Shown: click to hide") : tr("Hidden: click to show"));
        row->setIcon(kActive, activeIcon(active));
        row->setToolTip(kActive, active ? tr("Being edited") : tr("Click to edit this sublayer"));
        row->setToolTip(kName, contents(s, l.id));
        row->setFlags(Qt::ItemIsEnabled | Qt::ItemIsSelectable | Qt::ItemIsEditable);
        QFont font = row->font(kName);
        font.setBold(active);
        row->setFont(kName, font);
        if (!l.visible) {
            row->setForeground(kName, QColor(0xd5, 0xd9, 0xde, 110));
        }
        if (active) {
            m_tree->setCurrentItem(row, kName);
        }
    }
    m_tree->expandAll();
    m_rebuilding = false;
}

void BrowserPanel::addSublayer()
{
    m_view.tryEdit(tr("New sublayer"), [&](Editor& ed) { 
        ed.addSublayer(tr("Sublayer %1").arg(m_doc.sketch().sublayers().size() + 1));
    });
}

void BrowserPanel::deleteSublayer(Id sublayer, bool ask)
{
    const core::Sketch& s = m_doc.sketch();
    if (!s.hasSublayer(sublayer) || s.sublayers().size() < 2) {
        emit m_view.message(tr("The last sublayer can't be deleted"));
        return;
    }
    const bool empty = std::ranges::none_of(s.entities(), [&](const auto& kv) { return kv.second.sublayer == sublayer; })
        && std::ranges::none_of(s.texts(), [&](const auto& kv) { return kv.second.sublayer == sublayer; });
    if (ask && !empty
        && QMessageBox::question(this, tr("Delete sublayer"),
                                 tr("Delete \"%1\" and everything in it?").arg(QString::fromStdString(s.sublayer(sublayer).name)))
            != QMessageBox::Yes) {
        return;
    }
    m_view.tryEdit(tr("Delete sublayer"), [&](Editor& ed) { ed.removeSublayer(sublayer); });
}

void BrowserPanel::moveSelectionTo(Id sublayer)
{
    std::vector<Id> items;
    const core::Sketch& s = m_doc.sketch();
    for (Id id : m_view.selection()) {
        if ((s.hasEntity(id) && !s.isReference(id)) || s.hasConstraint(id) || s.hasText(id)) {
            items.push_back(id);
        }
    }
    if (items.empty()) {
        emit m_view.message(tr("Select what to move first"));
        return;
    }
    m_view.tryEdit(tr("Move to sublayer"), [&](Editor& ed) { ed.moveToSublayer(items, sublayer); });
    m_view.setSelection({});
}

void BrowserPanel::showMenu(const QPoint& at)
{
    QTreeWidgetItem* item = m_tree->itemAt(at);
    QMenu menu(this);
    menu.addAction(tr("New Sublayer"), this, &BrowserPanel::addSublayer);
    if (item && item != m_layer) {
        const Id id = item->data(kName, kIdRole).toUInt();
        const core::Sketch& s = m_doc.sketch();
        const auto& all = s.sublayers();
        const int index = static_cast<int>(std::ranges::find_if(all, [&](const core::Sublayer& l) { return l.id == id; }) - all.begin());
        menu.addSeparator();
        menu.addAction(tr("Edit This Sublayer"), this, [this, id] { m_doc.setActiveSublayer(id); });
        menu.addAction(s.sublayer(id).visible ? tr("Hide") : tr("Show"), this,
                       [this, id] { m_doc.setSublayerVisible(id, !m_doc.sketch().sublayer(id).visible); });
        menu.addAction(tr("Rename"), this, [this, item] { m_tree->editItem(item, kName); });
        menu.addAction(tr("Move Selection Here"), this, [this, id] { moveSelectionTo(id); })
            ->setEnabled(!m_view.selection().empty());
        menu.addSeparator();
        menu.addAction(tr("Move Up"), this, [this, id, index] {
                m_view.tryEdit(tr("Move sublayer"), [&](Editor& ed) { ed.moveSublayer(id, index - 1); });
            })->setEnabled(index > 0);
        menu.addAction(tr("Move Down"), this, [this, id, index] {
                m_view.tryEdit(tr("Move sublayer"), [&](Editor& ed) { ed.moveSublayer(id, index + 1); });
            })->setEnabled(index + 1 < static_cast<int>(all.size()));
        menu.addSeparator();
        menu.addAction(tr("Delete"), this, [this, id] { deleteSublayer(id); })->setEnabled(all.size() > 1);
    }
    menu.exec(m_tree->viewport()->mapToGlobal(at));
}
