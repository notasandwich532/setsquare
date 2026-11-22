#include "ImportDialog.h"

#include <QCheckBox>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QFileInfo>
#include <QFormLayout>
#include <QLabel>
#include <QPushButton>

ImportDialog::ImportDialog(const QString& path, const QString& sublayer, bool hasPage, QWidget* parent)
    : QDialog(parent)
    , m_path(path)
{
    setObjectName(QStringLiteral("importDialog"));
    setWindowTitle(tr("Import %1").arg(QFileInfo(path).fileName()));
    m_svg = QFileInfo(path).suffix().compare(QStringLiteral("svg"), Qt::CaseInsensitive) == 0;
    auto* form = new QFormLayout(this);

    m_summary = new QLabel(this);
    m_summary->setObjectName(QStringLiteral("importSummary"));
    m_summary->setWordWrap(true);
    form->addRow(m_summary);

    m_into = new QComboBox(this);
    m_into->setObjectName(QStringLiteral("importInto"));
    m_into->addItem(tr("The sublayer being edited (%1)").arg(sublayer), static_cast<int>(importer::Into::Sublayer));
    m_into->addItem(tr("A new sublayer"), static_cast<int>(importer::Into::NewSublayer));
    m_into->addItem(tr("A new layer"), static_cast<int>(importer::Into::NewLayer));
    m_into->setCurrentIndex(1);
    form->addRow(tr("Into"), m_into);

    m_keepLayers = new QCheckBox(this);
    m_keepLayers->setObjectName(QStringLiteral("importKeepLayers"));
    m_keepLayers->setChecked(true);
    form->addRow(QString(), m_keepLayers);

    // (Shown when the file has a page; ticked when the drawing hasn't one yet.)
    m_sketchHasPage = hasPage;
    m_usePage = new QCheckBox(this);
    m_usePage->setObjectName(QStringLiteral("importUsePage"));
    form->addRow(QString(), m_usePage);

    m_placement = new QComboBox(this);
    m_placement->setObjectName(QStringLiteral("importPlacement"));
    m_placement->addItem(m_svg ? tr("As in the file (its page's corner on the origin)") : tr("As in the file"),
                         static_cast<int>(importer::Placement::AsInFile));
    m_placement->addItem(tr("Centred in the view"), static_cast<int>(importer::Placement::Center));
    if (hasPage) {
        m_placement->addItem(m_svg ? tr("On the page (page to page)") : tr("On the page (its corner on the page's)"),
                             static_cast<int>(importer::Placement::Page));
    }
    form->addRow(tr("Placement"), m_placement);

    m_curves = new QComboBox(this);
    m_curves->setObjectName(QStringLiteral("importCurves"));
    m_curves->addItem(tr("Exact (a spline per curve piece)"), static_cast<int>(importer::Curves::Exact));
    m_curves->addItem(tr("Smooth (fewer splines, very close)"), static_cast<int>(importer::Curves::Smooth));
    m_curves->setToolTip(tr("Bezier curves (SVG paths, DXF splines) have no exact match here: either a control-point "
                            "spline per piece (exactly as drawn, but many pieces), or a fit spline through points along "
                            "each smooth run (fewer pieces, within a hair of the original)"));
    form->addRow(tr("Curves"), m_curves);

    m_pxPerInch = new QComboBox(this);
    m_pxPerInch->setObjectName(QStringLiteral("importPxPerInch"));
    m_pxPerInch->addItem(tr("96 to the inch (the web's; most programs)"), 96.0);
    m_pxPerInch->addItem(tr("90 to the inch (older Inkscape)"), 90.0);
    m_pxPerInch->addItem(tr("72 to the inch (Illustrator, older files)"), 72.0);
    m_units = new QComboBox(this);
    m_units->setObjectName(QStringLiteral("importUnits"));
    m_units->addItem(tr("As the file says (else millimetres)"), 0.0);
    for (const auto& [name, mm] : {std::pair{tr("Millimetres"), 1.0}, {tr("Centimetres"), 10.0}, {tr("Metres"), 1000.0},
                                   {tr("Inches"), 25.4}, {tr("Feet"), 304.8}}) {
        m_units->addItem(name, mm);
    }
    if (m_svg) {
        form->addRow(tr("Pixels"), m_pxPerInch);
    }
    else {
        form->addRow(tr("Units"), m_units);
    }
    m_pxPerInch->setVisible(m_svg);
    m_units->setVisible(!m_svg);

    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    buttons->button(QDialogButtonBox::Ok)->setText(tr("Import"));
    form->addRow(buttons);
    connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    connect(m_pxPerInch, &QComboBox::currentIndexChanged, this, &ImportDialog::reread);
    connect(m_units, &QComboBox::currentIndexChanged, this, &ImportDialog::reread);
    reread();
    buttons->button(QDialogButtonBox::Ok)->setEnabled(m_readable);
}

void ImportDialog::reread()
{
    try {
        const importer::Drawing d = importer::read(m_path, pxPerInch(), mmPerUnit());
        double x0 = 0, y0 = 0, x1 = 0, y1 = 0;
        if (!d.bounds(x0, y0, x1, y1)) {
            throw core::SketchError("there's nothing in it to import");
        }
        m_readable = true;
        const int shapes = static_cast<int>(d.shapes.size()), layers = static_cast<int>(d.layers.size());
        QString text = tr("<b>%1 × %2 mm</b> (units: %3); %4")
                           .arg(x1 - x0, 0, 'f', 1)
                           .arg(y1 - y0, 0, 'f', 1)
                           .arg(QString::fromStdString(d.units), shapes == 1 ? tr("1 shape") : tr("%1 shapes").arg(shapes));
        if (layers > 1) {
            text += tr(" on %1 layers").arg(layers);
        }
        if (!d.skipped.empty()) {
            QStringList what;
            for (const auto& [kind, count] : d.skipped) {
                what << QStringLiteral("%1 %2").arg(count).arg(QString::fromStdString(kind));
            }
            text += tr("<br>Can't be brought in (skipped): %1").arg(what.join(QStringLiteral(", ")));
        }
        for (const std::string& n : d.notes) {
            text += QStringLiteral("<br>") + QString::fromStdString(n);
        }
        m_summary->setText(text);
        m_keepLayers->setText(layers > 1 ? tr("Each of its %1 layers in a sublayer of its own (each a group)").arg(layers)
                                         : tr("Its layers in sublayers of their own (it has one)"));
        m_keepLayers->setEnabled(layers > 1);
        const bool page = d.hasPage && d.canvas;
        if (page) {
            const auto& c = *d.canvas;
            m_usePage->setText(m_sketchHasPage ? tr("Use its page instead (%1 × %2 mm)").arg(c[2] - c[0], 0, 'f', 1).arg(c[3] - c[1], 0, 'f', 1)
                                               : tr("Use its page (%1 × %2 mm)").arg(c[2] - c[0], 0, 'f', 1).arg(c[3] - c[1], 0, 'f', 1));
            m_usePage->setChecked(!m_sketchHasPage);
        }
        m_usePage->setVisible(page);
    }
    catch (const core::SketchError& e) {
        m_readable = false;
        m_error = QString::fromStdString(e.what());
        m_summary->setText(tr("Can't import this: %1").arg(m_error));
    }
}

importer::Options ImportDialog::options() const
{
    importer::Options o;
    o.into = static_cast<importer::Into>(m_into->currentData().toInt());
    o.keepLayers = m_keepLayers->isChecked();
    o.usePage = m_usePage->isVisible() && m_usePage->isChecked();
    o.placement = static_cast<importer::Placement>(m_placement->currentData().toInt());
    o.curves = static_cast<importer::Curves>(m_curves->currentData().toInt());
    o.name = QFileInfo(m_path).completeBaseName().toStdString();
    return o;
}

double ImportDialog::pxPerInch() const
{
    return m_pxPerInch->currentData().toDouble();
}

std::optional<double> ImportDialog::mmPerUnit() const
{
    const double mm = m_units->currentData().toDouble();
    return !m_svg && mm > 0 ? std::optional<double>(mm) : std::nullopt;
}
