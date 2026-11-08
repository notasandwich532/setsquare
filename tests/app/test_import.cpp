// File > Import's dialog (what it reads, its options), and an imported drawing as it shows.

#include "Document.h"
#include "ImportDialog.h"
#include "SketchView.h"
#include "Snapshot.h"

#include <catch2/catch_test_macros.hpp>

#include <QCheckBox>
#include <QComboBox>
#include <QFile>
#include <QLabel>
#include <QTemporaryDir>

namespace
{
QString write(const QTemporaryDir& dir, const QString& name, const QByteArray& text)
{
    const QString path = dir.filePath(name);
    QFile f(path);
    f.open(QIODevice::WriteOnly);
    f.write(text);
    return path;
}

const QByteArray kSvg = R"(<svg xmlns="http://www.w3.org/2000/svg" xmlns:inkscape="http://www.inkscape.org/namespaces/inkscape"
     width="120mm" height="80mm" viewBox="0 0 120 80">
  <g inkscape:groupmode="layer" inkscape:label="Body">
    <path d="M10 70 L60 70 C80 70 100 50 100 30 A20 20 0 0 0 60 10 L10 10 Z" fill="#ffc90e" stroke="#880015" stroke-width="1.5"/>
    <circle cx="35" cy="40" r="12" fill="#00a2e8" stroke="#3f48cc" stroke-width="1"/>
  </g>
  <g inkscape:groupmode="layer" inkscape:label="Marks">
    <line x1="10" y1="76" x2="110" y2="76" stroke="#22b14c" stroke-width="0.7" stroke-dasharray="4 2"/>
    <text x="5" y="5">note</text>
  </g>
</svg>)";
}  // namespace

TEST_CASE("Import dialog: reads the file and offers its options", "[import]")
{
    QTemporaryDir dir;
    const QString svg = write(dir, QStringLiteral("part.svg"), kSvg);
    ImportDialog dialog(svg, QStringLiteral("Sublayer 1"), false);
    REQUIRE(dialog.readable());
    const QString summary = dialog.findChild<QLabel*>(QStringLiteral("importSummary"))->text();
    CHECK(summary.contains(QStringLiteral("2 layers")));
    CHECK(summary.contains(QStringLiteral("1 texts")));
    CHECK(dialog.findChild<QCheckBox*>(QStringLiteral("importKeepLayers"))->isEnabled());
    CHECK(dialog.findChild<QComboBox*>(QStringLiteral("importPlacement"))->count() == 2);  // (no page)

    auto* into = dialog.findChild<QComboBox*>(QStringLiteral("importInto"));
    into->setCurrentIndex(2);
    dialog.findChild<QComboBox*>(QStringLiteral("importCurves"))->setCurrentIndex(1);
    const importer::Options o = dialog.options();
    CHECK(o.into == importer::Into::NewLayer);
    CHECK(o.curves == importer::Curves::Smooth);
    CHECK(o.name == "part");
    CHECK(dialog.pxPerInch() == 96.0);
    CHECK(!dialog.mmPerUnit());

    // A DXF: its units can be chosen; with a page, it can go on it.
    const QString dxf = write(dir, QStringLiteral("line.dxf"),
                              "0\nSECTION\n2\nENTITIES\n0\nLINE\n8\n0\n10\n0\n20\n0\n11\n10\n21\n0\n0\nENDSEC\n0\nEOF\n");
    ImportDialog second(dxf, QStringLiteral("Sublayer 1"), true);
    REQUIRE(second.readable());
    CHECK(second.findChild<QComboBox*>(QStringLiteral("importPlacement"))->count() == 3);
    second.findChild<QComboBox*>(QStringLiteral("importUnits"))->setCurrentIndex(4);  // inches
    CHECK(second.mmPerUnit() == 25.4);
    CHECK(second.findChild<QLabel*>(QStringLiteral("importSummary"))->text().contains(QStringLiteral("254.0")));

    // Not a drawing.
    ImportDialog bad(write(dir, QStringLiteral("bad.svg"), "nonsense"), QStringLiteral("Sublayer 1"), false);
    CHECK(!bad.readable());
}

TEST_CASE("An imported SVG as it shows", "[import]")
{
    QTemporaryDir dir;
    const QString svg = write(dir, QStringLiteral("part.svg"), kSvg);
    Document doc;
    SketchView view(doc);
    view.resize(700, 500);
    view.show();
    importer::Report report;
    REQUIRE(view.tryEdit("import", [&](Editor& ed) { report = ed.importFile(svg, importer::Options{}); }));
    CHECK(report.groups == 2);
    CHECK(report.fills == 2);
    view.fitAll();
    saveSnapshot(view, "_sketch");
    view.setPresentation(true);
    saveSnapshot(view, "_presentation");
}
