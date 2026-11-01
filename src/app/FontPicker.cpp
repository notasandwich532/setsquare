#include "FontPicker.h"

#include "TextShapes.h"

#include <QFont>
#include <QLineEdit>
#include <QListWidget>
#include <QVBoxLayout>

FontPicker::FontPicker(QWidget* parent)
    : QWidget(parent)
    , m_search(new QLineEdit(this))
    , m_list(new QListWidget(this))
{
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(3);
    m_search->setObjectName(QStringLiteral("fontSearch"));
    m_search->setPlaceholderText(tr("Search fonts"));
    m_search->setClearButtonEnabled(true);
    m_list->setObjectName(QStringLiteral("fonts"));
    m_list->setFixedHeight(150);
    m_list->setUniformItemSizes(true);
    layout->addWidget(m_search);
    layout->addWidget(m_list);

    connect(m_search, &QLineEdit::textChanged, this, &FontPicker::refill);
    connect(m_list, &QListWidget::currentTextChanged, this, [this](const QString& family) {
        if (!family.isEmpty() && family != m_family) {
            m_family = family;
            emit familyChanged(family);
        }
    });
    refill();
}

void FontPicker::setFamily(const QString& family)
{
    m_family = family;
    const QList<QListWidgetItem*> found = m_list->findItems(family, Qt::MatchExactly);
    if (!found.isEmpty()) {
        const QSignalBlocker block(m_list);
        m_list->setCurrentItem(found.first());
        m_list->scrollToItem(found.first(), QAbstractItemView::PositionAtCenter);
    }
}

void FontPicker::refill()
{
    const QSignalBlocker block(m_list);
    m_list->clear();
    for (const QString& family : textshapes::families(m_search->text().trimmed())) {
        auto* item = new QListWidgetItem(family, m_list);
        QFont font(family);
        font.setPointSizeF(m_list->font().pointSizeF() * 1.15);
        item->setFont(font);  // each shown in itself
        if (family == m_family) {
            m_list->setCurrentItem(item);
        }
    }
    // Typing a search and pressing nothing else: take the first match.
    if (!m_list->currentItem() && m_list->count() > 0 && !m_search->text().trimmed().isEmpty()) {
        m_list->setCurrentRow(0);
        m_family = m_list->item(0)->text();
        emit familyChanged(m_family);
    }
}
