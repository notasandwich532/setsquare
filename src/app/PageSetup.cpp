#include "PageSetup.h"

#include "NumberInput.h"

#include <QCheckBox>
#include <QColorDialog>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QPixmap>
#include <QPushButton>
#include <QToolButton>

#include <algorithm>
#include <utility>

namespace
{
const QString kNone = QStringLiteral("none");
const QString kCustom = QStringLiteral("custom");
}  // namespace

PageSetupDialog::PageSetupDialog(const std::optional<core::Page>& page, QWidget* parent)
    : QDialog(parent)
{
    setObjectName(QStringLiteral("pageSetupDialog"));
    setWindowTitle(tr("Page Setup"));
    auto* form = new QFormLayout(this);

    m_size = new QComboBox(this);
    m_size->setObjectName(QStringLiteral("pageSize"));
    m_size->addItem(tr("None (endless canvas)"), kNone);
    for (const core::PageSize& s : core::pageSizes()) {
        m_size->addItem(QStringLiteral("%1  (%2 × %3 mm)").arg(QString::fromLatin1(s.name)).arg(s.width).arg(s.height),
                        QString::fromLatin1(s.name));
    }
    m_size->addItem(tr("Custom"), kCustom);
    form->addRow(tr("Size"), m_size);

    m_orientation = new QComboBox(this);
    m_orientation->setObjectName(QStringLiteral("pageOrientation"));
    m_orientation->addItems({tr("Portrait"), tr("Landscape")});
    form->addRow(tr("Orientation"), m_orientation);

    auto size = [this](const char* name) {
        auto* box = new ExpressionSpinBox(this);
        box->setObjectName(QLatin1String(name));
        box->setRange(1.0, 100000.0);
        box->setDecimals(2);
        box->setSuffix(QStringLiteral(" mm"));
        return box;
    };
    m_width = size("pageWidth");
    m_height = size("pageHeight");
    form->addRow(tr("Width"), m_width);
    form->addRow(tr("Height"), m_height);

    m_origin = new QComboBox(this);
    m_origin->setObjectName(QStringLiteral("pageOrigin"));
    m_origin->addItems({tr("Bottom-left corner"), tr("Centre"), tr("Top-left corner")});
    m_origin->setToolTip(tr("Where the sketch's origin is on the page"));
    form->addRow(tr("Origin at"), m_origin);

    auto* colorRow = new QWidget(this);
    auto* colorLayout = new QHBoxLayout(colorRow);
    colorLayout->setContentsMargins(0, 0, 0, 0);
    m_hasColor = new QCheckBox(tr("Coloured"), colorRow);
    m_hasColor->setObjectName(QStringLiteral("pageHasColor"));
    m_hasColor->setToolTip(tr("Off: no paper colour (the SVG has a transparent background)"));
    m_colorButton = new QToolButton(colorRow);
    m_colorButton->setObjectName(QStringLiteral("pageColor"));
    m_colorButton->setIconSize(QSize(28, 14));
    colorLayout->addWidget(m_hasColor);
    colorLayout->addWidget(m_colorButton);
    colorLayout->addStretch();
    form->addRow(tr("Paper"), colorRow);

    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    form->addRow(buttons);
    connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);

    // As it is now.
    m_updating = true;
    if (page) {
        const int i = m_size->findData(QString::fromStdString(page->size));
        m_size->setCurrentIndex(i >= 0 ? i : m_size->findData(kCustom));
        m_orientation->setCurrentIndex(page->landscape() ? 1 : 0);
        m_width->setValue(page->width);
        m_height->setValue(page->height);
        m_origin->setCurrentIndex(static_cast<int>(page->origin));
        m_hasColor->setChecked(page->hasColor);
        m_color = page->color;
    }
    else {
        m_size->setCurrentIndex(0);
        m_width->setValue(210.0);
        m_height->setValue(297.0);
    }
    m_updating = false;
    setColor(m_color);
    update();

    connect(m_size, &QComboBox::currentIndexChanged, this, [this] { update(); });
    connect(m_orientation, &QComboBox::currentIndexChanged, this, [this] {
        if (m_updating) {
            return;
        }
        // Turned: the sides swap (custom too).
        const bool landscape = m_orientation->currentIndex() == 1;
        if ((m_width->value() > m_height->value()) != landscape) {
            m_updating = true;
            const double w = m_width->value();
            m_width->setValue(m_height->value());
            m_height->setValue(w);
            m_updating = false;
        }
    });
    auto sized = [this] {
        if (!m_updating) {
            m_updating = true;
            m_orientation->setCurrentIndex(m_width->value() > m_height->value() ? 1 : 0);
            m_updating = false;
        }
    };
    connect(m_width, &QDoubleSpinBox::valueChanged, this, sized);
    connect(m_height, &QDoubleSpinBox::valueChanged, this, sized);
    connect(m_hasColor, &QCheckBox::toggled, this, [this] { update(); });
    connect(m_colorButton, &QToolButton::clicked, this, [this] {
        const QColor chosen = QColorDialog::getColor(QColor(m_color.r, m_color.g, m_color.b, m_color.a), this,
                                                     tr("Paper colour"), QColorDialog::ShowAlphaChannel);
        if (chosen.isValid()) {
            setColor({static_cast<std::uint8_t>(chosen.red()), static_cast<std::uint8_t>(chosen.green()),
                      static_cast<std::uint8_t>(chosen.blue()), static_cast<std::uint8_t>(chosen.alpha())});
            m_hasColor->setChecked(true);
        }
    });
}

void PageSetupDialog::setColor(const core::Color& color)
{
    m_color = color;
    QPixmap swatch(28, 14);
    swatch.fill(QColor(color.r, color.g, color.b, color.a));
    m_colorButton->setIcon(QIcon(swatch));
}

void PageSetupDialog::update()
{
    const QString size = m_size->currentData().toString();
    const bool none = size == kNone;
    const bool custom = size == kCustom;
    double w = 0, h = 0;
    if (!none && !custom && core::pageSizeByName(size.toStdString(), w, h)) {
        m_updating = true;
        const bool landscape = m_orientation->currentIndex() == 1;
        m_width->setValue(landscape ? h : w);
        m_height->setValue(landscape ? w : h);
        m_updating = false;
    }
    m_orientation->setEnabled(!none);
    m_width->setEnabled(custom);
    m_height->setEnabled(custom);
    m_origin->setEnabled(!none);
    m_hasColor->setEnabled(!none);
    m_colorButton->setEnabled(!none);
}

std::optional<core::Page> PageSetupDialog::page() const
{
    const QString size = m_size->currentData().toString();
    if (size == kNone) {
        return std::nullopt;
    }
    core::Page p;
    p.size = size.toStdString();
    double w = 0, h = 0;
    if (size != kCustom && core::pageSizeByName(p.size, w, h)) {
        const bool landscape = m_orientation->currentIndex() == 1;
        p.width = landscape ? h : w;
        p.height = landscape ? w : h;
    }
    else {
        p.width = m_width->value();
        p.height = m_height->value();
    }
    p.origin = static_cast<core::PageOrigin>(m_origin->currentIndex());
    p.hasColor = m_hasColor->isChecked();
    p.color = m_color;
    return p;
}
