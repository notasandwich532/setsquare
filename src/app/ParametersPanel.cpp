#include "ParametersPanel.h"

#include "Document.h"
#include "Editor.h"

#include <core/Parameters.h>

#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QPushButton>
#include <QTableWidget>
#include <QTimer>
#include <QVBoxLayout>

namespace
{
enum Column { kName, kExpression, kValue };
}

ParametersPanel::ParametersPanel(Document& doc, QWidget* parent)
    : QWidget(parent)
    , m_doc(doc)
    , m_table(new QTableWidget(0, 3, this))
    , m_remove(new QPushButton(tr("Remove"), this))
    , m_problem(new QLabel(this))
{
    m_table->setObjectName(QStringLiteral("parametersTable"));
    m_table->setHorizontalHeaderLabels({tr("Name"), tr("Expression"), tr("Value")});
    m_table->horizontalHeader()->setSectionResizeMode(kName, QHeaderView::ResizeToContents);
    m_table->horizontalHeader()->setSectionResizeMode(kExpression, QHeaderView::Stretch);
    m_table->horizontalHeader()->setSectionResizeMode(kValue, QHeaderView::ResizeToContents);
    m_table->verticalHeader()->hide();
    m_table->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_table->setSelectionMode(QAbstractItemView::SingleSelection);
    m_table->setEditTriggers(QAbstractItemView::DoubleClicked | QAbstractItemView::EditKeyPressed
                             | QAbstractItemView::AnyKeyPressed);
    m_table->setToolTip(tr("Named values for dimensions to use: type a name into a dimension (\"width / 2\") and it "
                           "follows the parameter. Parameters can use each other."));

    auto* add = new QPushButton(tr("Add"), this);
    add->setObjectName(QStringLiteral("addParameter"));
    m_remove->setObjectName(QStringLiteral("removeParameter"));
    m_problem->setObjectName(QStringLiteral("parameterProblem"));
    m_problem->setWordWrap(true);
    m_problem->setStyleSheet(QStringLiteral("color: #f2b33d;"));  // (a warning's amber)
    m_problem->hide();

    auto* buttons = new QHBoxLayout;
    buttons->addWidget(add);
    buttons->addWidget(m_remove);
    buttons->addStretch();
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(4, 4, 4, 4);
    layout->addWidget(m_table);
    layout->addWidget(m_problem);
    layout->addLayout(buttons);

    connect(add, &QPushButton::clicked, this, &ParametersPanel::add);
    connect(m_remove, &QPushButton::clicked, this, &ParametersPanel::removeSelected);
    connect(m_table, &QTableWidget::cellChanged, this, &ParametersPanel::cellEdited);
    connect(m_table, &QTableWidget::itemSelectionChanged, this,
            [this] { m_remove->setEnabled(!m_table->selectedItems().isEmpty()); });
    connect(&m_doc, &Document::changed, this, &ParametersPanel::rebuild);
    rebuild();
    m_remove->setEnabled(false);
}

void ParametersPanel::rebuild()
{
    const core::Sketch& s = m_doc.sketch();
    std::vector<Shown> shown;
    for (const core::Parameter& p : s.parameters()) {
        shown.push_back({p.name, p.expression, p.value, core::parameters::usersOf(s, p.name).dimensions});
    }
    if (shown == m_shown) {
        return;  // (geometry dragged: nothing here changes)
    }
    m_shown = std::move(shown);
    const int selected = m_table->currentRow();
    m_filling = true;
    m_table->setRowCount(static_cast<int>(m_shown.size()));
    for (int row = 0; row < static_cast<int>(m_shown.size()); ++row) {
        const Shown& p = m_shown[static_cast<size_t>(row)];
        auto* name = new QTableWidgetItem(QString::fromStdString(p.name));
        auto* expression = new QTableWidgetItem(QString::fromStdString(p.expression));
        auto* value = new QTableWidgetItem(Editor::number(p.value));
        value->setFlags(value->flags() & ~Qt::ItemIsEditable);
        value->setTextAlignment(Qt::AlignRight | Qt::AlignVCenter);
        QStringList users;
        for (core::Id d : p.users) {
            users << Editor::name(s, d);
        }
        const core::parameters::Users all = core::parameters::usersOf(s, p.name);
        for (const std::string& q : all.parameters) {
            users << QString::fromStdString(q);
        }
        name->setToolTip(users.isEmpty() ? tr("Not used yet") : tr("Used by %1").arg(users.join(QStringLiteral(", "))));
        m_table->setItem(row, kName, name);
        m_table->setItem(row, kExpression, expression);
        m_table->setItem(row, kValue, value);
    }
    m_filling = false;
    if (selected >= 0 && selected < m_table->rowCount()) {
        m_table->selectRow(selected);
    }
}

void ParametersPanel::cellEdited(int row, int column)
{
    if (m_filling || row < 0 || row >= static_cast<int>(m_shown.size())) {
        return;
    }
    const QString was = QString::fromStdString(m_shown[static_cast<size_t>(row)].name);
    const QString typed = m_table->item(row, column)->text().trimmed();
    QString error;
    bool done = true;
    if (column == kName && typed != was) {
        done = m_doc.renameParameter(was, typed, &error);
    }
    else if (column == kExpression && typed.toStdString() != m_shown[static_cast<size_t>(row)].expression) {
        done = m_doc.setParameter(was, typed, &error);
    }
    say(done ? QString() : error);
    if (!done) {
        // Back to what it was (after this edit's done with).
        m_shown.clear();
        QTimer::singleShot(0, this, &ParametersPanel::rebuild);
    }
}

void ParametersPanel::add()
{
    // The next free "param1", "param2", ..., to be renamed.
    QString name;
    for (int n = 1;; ++n) {
        name = QStringLiteral("param%1").arg(n);
        if (core::parameters::find(m_doc.sketch(), name.toStdString()) < 0) {
            break;
        }
    }
    QString error;
    if (!m_doc.setParameter(name, QStringLiteral("10"), &error)) {
        say(error);
        return;
    }
    say({});
    const int row = core::parameters::find(m_doc.sketch(), name.toStdString());
    if (row >= 0 && row < m_table->rowCount()) {
        m_table->setCurrentCell(row, kName);
        m_table->editItem(m_table->item(row, kName));
    }
}

void ParametersPanel::removeSelected()
{
    const int row = m_table->currentRow();
    if (row < 0 || row >= static_cast<int>(m_shown.size())) {
        return;
    }
    QString error;
    const bool done = m_doc.deleteParameter(QString::fromStdString(m_shown[static_cast<size_t>(row)].name), &error);
    say(done ? QString() : error);
}

void ParametersPanel::say(const QString& problem)
{
    m_problem->setText(problem);
    m_problem->setVisible(!problem.isEmpty());
}
