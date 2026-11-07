#include "BrowserPanel.h"

#include "Document.h"
#include "Icons.h"
#include "SketchView.h"

#include <QFileInfo>

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
    kName,
    kEye,
    kActive,
};
constexpr int kIdRole = Qt::UserRole;
constexpr int kIsLayerRole = Qt::UserRole + 1;
constexpr int kIsImageRole = Qt::UserRole + 2;

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

bool sublayerEmpty(const core::Sketch& s, Id sublayer)
{
    return std::ranges::none_of(s.entities(), [&](const auto& kv) { return kv.second.sublayer == sublayer; })
        && std::ranges::none_of(s.texts(), [&](const auto& kv) { return kv.second.sublayer == sublayer; });
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
    add->setObjectName(QStringLiteral("add"));
    add->setText(QStringLiteral("+"));
    add->setToolTip(tr("New layer or sublayer"));
    add->setAutoRaise(true);
    add->setPopupMode(QToolButton::InstantPopup);
    auto* addMenu = new QMenu(add);
    addMenu->addAction(tr("New Layer"), this, &BrowserPanel::addLayer);
    addMenu->addAction(tr("New Sublayer"), this, &BrowserPanel::addSublayer);
    add->setMenu(addMenu);
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
    m_tree->header()->setStretchLastSection(false);
    m_tree->header()->setSectionResizeMode(kName, QHeaderView::Stretch);
    m_tree->header()->setSectionResizeMode(kEye, QHeaderView::ResizeToContents);
    m_tree->header()->setSectionResizeMode(kActive, QHeaderView::ResizeToContents);

    connect(&m_doc, &Document::changed, this, &BrowserPanel::rebuild);
    connect(m_tree, &QTreeWidget::customContextMenuRequested, this, &BrowserPanel::showMenu);
    connect(m_tree, &QTreeWidget::itemClicked, this, [this](QTreeWidgetItem* item, int column) {
        if (m_rebuilding || !item) {
            return;
        }
        const Id id = item->data(kName, kIdRole).toUInt();
        const core::Sketch& s = m_doc.sketch();
        if (item->data(kName, kIsImageRole).toBool()) {
            if (!s.hasImage(id)) {
                return;
            }
            if (column == kEye) {
                core::Image im = s.image(id);
                im.visible = !im.visible;
                m_view.tryEdit(im.visible ? tr("Show image") : tr("Hide image"), [&](Editor& ed) { ed.setImage(im); });
            }
            else if (s.image(id).visible) {
                // Placing it: in its layer.
                m_doc.setActiveLayer(s.image(id).layer);
                m_view.editImage(id);
            }
            return;
        }
        if (isLayerRow(item)) {
            if (column == kEye) {
                m_doc.setLayerVisible(id, !s.layer(id).visible);
            }
            else {
                m_doc.setActiveLayer(id);
            }
        }
        else if (column == kEye) {
            m_doc.setSublayerVisible(id, !s.sublayer(id).visible);
        }
        else {
            m_doc.setActiveSublayer(id);  // the marker or the name: edit this one (and its layer)
        }
    });
    connect(m_tree, &QTreeWidget::itemChanged, this, [this](QTreeWidgetItem* item, int column) {
        if (m_rebuilding || column != kName || item->data(kName, kIsImageRole).toBool()) {
            return;
        }
        const Id id = item->data(kName, kIdRole).toUInt();
        const QString name = item->text(kName).trimmed();
        const core::Sketch& s = m_doc.sketch();
        const std::string was = isLayerRow(item) ? s.layer(id).name : s.sublayer(id).name;
        if (name.isEmpty() || name.toStdString() == was) {
            m_shownKey.clear();
            rebuild();
            return;
        }
        if (isLayerRow(item)) {
            m_view.tryEdit(tr("Rename layer"), [&](Editor& ed) { ed.renameLayer(id, name); });
        }
        else {
            m_view.tryEdit(tr("Rename sublayer"), [&](Editor& ed) { ed.renameSublayer(id, name); });
        }
    });
    rebuild();
}

bool BrowserPanel::isLayerRow(const QTreeWidgetItem* item) const
{
    return item && item->data(kName, kIsLayerRole).toBool();
}

QTreeWidgetItem* BrowserPanel::rowForLayer(Id layer) const
{
    for (int i = 0; i < m_tree->topLevelItemCount(); ++i) {
        if (m_tree->topLevelItem(i)->data(kName, kIdRole).toUInt() == layer) {
            return m_tree->topLevelItem(i);
        }
    }
    return nullptr;
}

QTreeWidgetItem* BrowserPanel::rowForImage(Id image) const
{
    for (int i = 0; i < m_tree->topLevelItemCount(); ++i) {
        QTreeWidgetItem* layer = m_tree->topLevelItem(i);
        for (int j = 0; j < layer->childCount(); ++j) {
            QTreeWidgetItem* row = layer->child(j);
            if (row->data(kName, kIsImageRole).toBool() && row->data(kName, kIdRole).toUInt() == image) {
                return row;
            }
        }
    }
    return nullptr;
}

QTreeWidgetItem* BrowserPanel::rowFor(Id sublayer) const
{
    for (int i = 0; i < m_tree->topLevelItemCount(); ++i) {
        QTreeWidgetItem* layer = m_tree->topLevelItem(i);
        for (int j = 0; j < layer->childCount(); ++j) {
            if (!layer->child(j)->data(kName, kIsImageRole).toBool() && layer->child(j)->data(kName, kIdRole).toUInt() == sublayer) {
                return layer->child(j);
            }
        }
    }
    return nullptr;
}

void BrowserPanel::rebuild()
{
    const core::Sketch& s = m_doc.sketch();
    // Rebuilt only when the layers or sublayers change (not on every edit): just tooltips then.
    QString key = QString::number(s.activeSublayer());
    for (const core::Layer& l : s.layers()) {
        key += QStringLiteral("|L%1:%2:%3").arg(l.id).arg(QString::fromStdString(l.name)).arg(l.visible);
        for (Id id : s.sublayersOf(l.id)) {
            const core::Sublayer& sub = s.sublayer(id);
            key += QStringLiteral("|S%1:%2:%3").arg(id).arg(QString::fromStdString(sub.name)).arg(sub.visible);
        }
    }
    for (const auto& [id, im] : s.images()) {
        key += QStringLiteral("|I%1:%2:%3").arg(id).arg(im.layer).arg(im.visible);
    }
    if (key == m_shownKey) {
        for (const core::Sublayer& l : s.sublayers()) {
            if (QTreeWidgetItem* row = rowFor(l.id)) {
                row->setToolTip(kName, contents(s, l.id));
            }
        }
        return;
    }
    m_shownKey = key;

    m_rebuilding = true;
    m_tree->clear();
    const Id activeLayer = s.activeLayer();
    for (const core::Layer& l : s.layers()) {
        auto* layerRow = new QTreeWidgetItem(m_tree);
        const bool layerActive = l.id == activeLayer;
        layerRow->setData(kName, kIdRole, l.id);
        layerRow->setData(kName, kIsLayerRole, true);
        layerRow->setText(kName, QString::fromStdString(l.name));
        layerRow->setToolTip(kName, tr("A layer: its own sketch, drawn over the ones below it. Other layers' "
                                       "geometry can be used here only by projecting it (Project)."));
        layerRow->setIcon(kEye, eyeIcon(l.visible));
        layerRow->setToolTip(kEye, l.visible ? tr("Shown: click to hide") : tr("Hidden: click to show"));
        layerRow->setIcon(kActive, activeIcon(layerActive));
        layerRow->setToolTip(kActive, layerActive ? tr("Being edited") : tr("Click to edit this layer"));
        layerRow->setFlags(Qt::ItemIsEnabled | Qt::ItemIsSelectable | Qt::ItemIsEditable);
        QFont layerFont = layerRow->font(kName);
        layerFont.setBold(layerActive);
        layerRow->setFont(kName, layerFont);
        if (!l.visible) {
            layerRow->setForeground(kName, QColor(0xd5, 0xd9, 0xde, 110));
        }
        for (Id id : s.sublayersOf(l.id)) {
            const core::Sublayer& sub = s.sublayer(id);
            auto* row = new QTreeWidgetItem(layerRow);
            const bool active = id == s.activeSublayer();
            row->setData(kName, kIdRole, id);
            row->setData(kName, kIsLayerRole, false);
            row->setText(kName, QString::fromStdString(sub.name));
            row->setIcon(kEye, eyeIcon(sub.visible));
            row->setToolTip(kEye, sub.visible ? tr("Shown: click to hide") : tr("Hidden: click to show"));
            row->setIcon(kActive, activeIcon(active));
            row->setToolTip(kActive, active ? tr("Being edited") : tr("Click to edit this sublayer"));
            row->setToolTip(kName, contents(s, id));
            row->setFlags(Qt::ItemIsEnabled | Qt::ItemIsSelectable | Qt::ItemIsEditable);
            QFont font = row->font(kName);
            font.setBold(active);
            row->setFont(kName, font);
            if (!sub.visible || !l.visible || !layerActive) {
                row->setForeground(kName, QColor(0xd5, 0xd9, 0xde, layerActive ? 110 : 150));
            }
            if (active) {
                m_tree->setCurrentItem(row, kName);
            }
        }
        // Its reference images, after its sublayers.
        for (const auto& [id, im] : s.images()) {
            if (im.layer != l.id) {
                continue;
            }
            auto* row = new QTreeWidgetItem(layerRow);
            row->setData(kName, kIdRole, id);
            row->setData(kName, kIsLayerRole, false);
            row->setData(kName, kIsImageRole, true);
            row->setText(kName, QFileInfo(QString::fromStdString(im.source)).fileName());
            row->setIcon(kName, icons::forName(QStringLiteral("image")));
            row->setToolTip(kName, tr("A reference image (not part of the drawing): click to place it, or calibrate it"));
            row->setIcon(kEye, eyeIcon(im.visible));
            row->setToolTip(kEye, im.visible ? tr("Shown: click to hide") : tr("Hidden: click to show"));
            row->setFlags(Qt::ItemIsEnabled | Qt::ItemIsSelectable);
            if (!im.visible || !l.visible) {
                row->setForeground(kName, QColor(0xd5, 0xd9, 0xde, 110));
            }
        }
        layerRow->setExpanded(true);
    }
    m_rebuilding = false;
}

void BrowserPanel::addLayer()
{
    m_view.tryEdit(tr("New layer"), [&](Editor& ed) {
        ed.addLayer(tr("Layer %1").arg(m_doc.sketch().layers().size() + 1));
    });
}

void BrowserPanel::addSublayer()
{
    m_view.tryEdit(tr("New sublayer"), [&](Editor& ed) {
        ed.addSublayer(tr("Sublayer %1").arg(m_doc.sketch().sublayersOf(m_doc.sketch().activeLayer()).size() + 1));
    });
}

void BrowserPanel::deleteLayer(Id layer, bool ask)
{
    const core::Sketch& s = m_doc.sketch();
    if (!s.hasLayer(layer) || s.layers().size() < 2) {
        emit m_view.message(tr("The last layer can't be deleted"));
        return;
    }
    const std::vector<Id> own = s.sublayersOf(layer);
    const bool empty = std::ranges::all_of(own, [&](Id id) { return sublayerEmpty(s, id); })
        && std::ranges::none_of(s.fills(), [&](const auto& kv) { return kv.second.layer == layer; });
    if (ask && !empty
        && QMessageBox::question(this, tr("Delete layer"),
                                 tr("Delete \"%1\" and everything in it?").arg(QString::fromStdString(s.layer(layer).name)))
            != QMessageBox::Yes) {
        return;
    }
    m_view.tryEdit(tr("Delete layer"), [&](Editor& ed) { ed.removeLayer(layer); });
}

void BrowserPanel::deleteSublayer(Id sublayer, bool ask)
{
    const core::Sketch& s = m_doc.sketch();
    if (!s.hasSublayer(sublayer) || s.sublayersOf(s.layerOfSublayer(sublayer)).size() < 2) {
        emit m_view.message(tr("A layer's last sublayer can't be deleted"));
        return;
    }
    if (ask && !sublayerEmpty(s, sublayer)
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
    if (m_view.tryEdit(tr("Move to sublayer"), [&](Editor& ed) { ed.moveToSublayer(items, sublayer); })) {
        m_view.setSelection({});
    }
}

void BrowserPanel::showMenu(const QPoint& at)
{
    QTreeWidgetItem* item = m_tree->itemAt(at);
    QMenu menu(this);
    menu.addAction(tr("New Layer"), this, &BrowserPanel::addLayer);
    menu.addAction(tr("New Sublayer"), this, &BrowserPanel::addSublayer);
    const core::Sketch& s = m_doc.sketch();
    if (item && isLayerRow(item)) {
        const Id id = item->data(kName, kIdRole).toUInt();
        const auto& all = s.layers();
        const int index = static_cast<int>(std::ranges::find_if(all, [&](const core::Layer& l) { return l.id == id; }) - all.begin());
        menu.addSeparator();
        menu.addAction(tr("Edit This Layer"), this, [this, id] { m_doc.setActiveLayer(id); });
        menu.addAction(s.layer(id).visible ? tr("Hide") : tr("Show"), this,
                       [this, id] { m_doc.setLayerVisible(id, !m_doc.sketch().layer(id).visible); });
        menu.addAction(tr("Rename"), this, [this, item] { m_tree->editItem(item, kName); });
        menu.addSeparator();
        menu.addAction(tr("Move Up"), this, [this, id, index] {
                m_view.tryEdit(tr("Move layer"), [&](Editor& ed) { ed.moveLayer(id, index - 1); });
            })->setEnabled(index > 0);
        menu.addAction(tr("Move Down"), this, [this, id, index] {
                m_view.tryEdit(tr("Move layer"), [&](Editor& ed) { ed.moveLayer(id, index + 1); });
            })->setEnabled(index + 1 < static_cast<int>(all.size()));
        menu.addSeparator();
        menu.addAction(tr("Delete"), this, [this, id] { deleteLayer(id); })->setEnabled(all.size() > 1);
    }
    else if (item && item->data(kName, kIsImageRole).toBool()) {
        const Id id = item->data(kName, kIdRole).toUInt();
        if (s.hasImage(id)) {
            const bool visible = s.image(id).visible;
            menu.addSeparator();
            menu.addAction(tr("Place / Calibrate"), this, [this, id] {
                    m_doc.setActiveLayer(m_doc.sketch().image(id).layer);
                    m_view.editImage(id);
                })->setEnabled(visible);
            menu.addAction(visible ? tr("Hide") : tr("Show"), this, [this, id] {
                core::Image im = m_doc.sketch().image(id);
                im.visible = !im.visible;
                m_view.tryEdit(im.visible ? tr("Show image") : tr("Hide image"), [&](Editor& ed) { ed.setImage(im); });
            });
            menu.addSeparator();
            menu.addAction(tr("Delete"), this,
                           [this, id] { m_view.tryEdit(tr("Delete image"), [&](Editor& ed) { ed.remove(id); }); });
        }
    }
    else if (item) {
        const Id id = item->data(kName, kIdRole).toUInt();
        const std::vector<Id> siblings = s.sublayersOf(s.layerOfSublayer(id));
        const int index = static_cast<int>(std::ranges::find(siblings, id) - siblings.begin());
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
            })->setEnabled(index + 1 < static_cast<int>(siblings.size()));
        menu.addSeparator();
        menu.addAction(tr("Delete"), this, [this, id] { deleteSublayer(id); })->setEnabled(siblings.size() > 1);
    }
    menu.exec(m_tree->viewport()->mapToGlobal(at));
}
