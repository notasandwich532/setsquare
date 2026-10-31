#include "ToolSearch.h"

#include <QAction>
#include <QGuiApplication>
#include <QKeyEvent>
#include <QLineEdit>
#include <QListWidget>
#include <QScreen>
#include <QVBoxLayout>

#include <algorithm>

namespace
{
constexpr int kVisibleRows = 10;

const char* kStyle = R"(
ToolSearch { background: #2b2f36; border: 1px solid #4da3ff; border-radius: 3px; }
ToolSearch QLineEdit { background: #1f2227; border: 1px solid #454b55; padding: 3px 6px; color: #e6ebf0; }
ToolSearch QListWidget { background: #2b2f36; border: none; color: #d5dbe2; }
ToolSearch QListWidget::item { padding: 3px 6px; }
ToolSearch QListWidget::item:selected { background: #2f4a66; color: #ffffff; }
)";
}  // namespace

ToolSearch::ToolSearch(QWidget* parent)
    : QFrame(parent, Qt::Popup)
    , m_input(new QLineEdit(this))
    , m_list(new QListWidget(this))
{
    setObjectName(QStringLiteral("toolSearch"));
    setStyleSheet(QString::fromLatin1(kStyle));
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(6, 6, 6, 6);
    layout->setSpacing(4);
    m_input->setObjectName(QStringLiteral("toolSearchInput"));
    m_input->setPlaceholderText(tr("Search tools and commands"));
    m_list->setObjectName(QStringLiteral("toolSearchList"));
    m_list->setFocusPolicy(Qt::NoFocus);  // typing stays in the box; arrows are forwarded
    m_list->setUniformItemSizes(true);
    layout->addWidget(m_input);
    layout->addWidget(m_list);
    resize(320, 60 + kVisibleRows * 24);

    m_input->installEventFilter(this);
    connect(m_input, &QLineEdit::textChanged, this, &ToolSearch::refill);
    connect(m_list, &QListWidget::itemClicked, this, [this] { runCurrent(); });
}

void ToolSearch::setEntries(std::vector<Entry> entries)
{
    m_entries = std::move(entries);
    refill();
}

std::vector<const ToolSearch::Entry*> ToolSearch::matches(const QString& query) const
{
    const QStringList words = query.simplified().split(QLatin1Char(' '), Qt::SkipEmptyParts);
    // Lower ranks first: the name starts with the query, then a word of the name does, then the
    // name contains it, then only its menu path does.
    std::vector<std::pair<int, const Entry*>> ranked;
    for (const Entry& e : m_entries) {
        if (!e.action->isEnabled()) {
            continue;
        }
        bool all = true;
        bool inName = true;
        for (const QString& w : words) {
            const bool name = e.name.contains(w, Qt::CaseInsensitive);
            all = all && (name || e.context.contains(w, Qt::CaseInsensitive));
            inName = inName && name;
        }
        if (!all) {
            continue;
        }
        int rank = 3;
        if (words.isEmpty() || e.name.startsWith(words.join(QLatin1Char(' ')), Qt::CaseInsensitive)) {
            rank = 0;
        }
        else if (std::ranges::any_of(e.name.split(QLatin1Char(' ')), [&](const QString& part) {
                     return part.startsWith(words.front(), Qt::CaseInsensitive);
                 })) {
            rank = 1;
        }
        else if (inName) {
            rank = 2;
        }
        ranked.emplace_back(rank, &e);
    }
    std::ranges::stable_sort(ranked, {}, &std::pair<int, const Entry*>::first);
    std::vector<const Entry*> out;
    for (const auto& [rank, e] : ranked) {
        out.push_back(e);
    }
    return out;
}

void ToolSearch::refill()
{
    m_list->clear();
    for (const Entry* e : matches(m_input->text())) {
        QString text = e->name;
        if (const QKeySequence key = e->action->shortcut(); !key.isEmpty()) {
            text += QStringLiteral("    ") + key.toString(QKeySequence::NativeText);
        }
        auto* item = new QListWidgetItem(text, m_list);
        item->setToolTip(e->context);
        item->setData(Qt::UserRole, QVariant::fromValue(static_cast<void*>(e->action)));
    }
    m_list->setCurrentRow(m_list->count() > 0 ? 0 : -1);
}

void ToolSearch::popup(QPoint globalPos)
{
    m_input->clear();
    refill();
    // Keep it on the screen.
    QRect area(globalPos, size());
    if (const QScreen* screen = QGuiApplication::screenAt(globalPos)) {
        const QRect avail = screen->availableGeometry();
        area.moveRight(std::min(area.right(), avail.right()));
        area.moveBottom(std::min(area.bottom(), avail.bottom()));
    }
    move(area.topLeft());
    show();
    m_input->setFocus();
}

void ToolSearch::runCurrent()
{
    QListWidgetItem* item = m_list->currentItem();
    if (!item) {
        return;
    }
    auto* action = static_cast<QAction*>(item->data(Qt::UserRole).value<void*>());
    hide();
    action->trigger();
}

bool ToolSearch::eventFilter(QObject* watched, QEvent* event)
{
    if (watched == m_input && event->type() == QEvent::KeyPress) {
        const int key = static_cast<QKeyEvent*>(event)->key();
        const int row = m_list->currentRow();
        switch (key) {
            case Qt::Key_Down: m_list->setCurrentRow(std::min(row + 1, m_list->count() - 1)); return true;
            case Qt::Key_Up: m_list->setCurrentRow(std::max(row - 1, 0)); return true;
            case Qt::Key_Return:
            case Qt::Key_Enter: runCurrent(); return true;
            case Qt::Key_Escape: hide(); return true;
            default: break;
        }
    }
    return QFrame::eventFilter(watched, event);
}
