// The page in the UI: Page Setup, undo, fitting the view, drawing it.

#include "Document.h"
#include "PageSetup.h"
#include "SketchView.h"
#include "Snapshot.h"

#include <core/Builders.h>

#include <catch2/catch_test_macros.hpp>

#include <QCheckBox>
#include <QComboBox>
#include <QDoubleSpinBox>

using namespace core;

namespace
{
template <typename T>
T* child(QWidget& w, const char* name)
{
    T* found = w.findChild<T*>(QLatin1String(name));
    REQUIRE(found);
    return found;
}
}  // namespace

TEST_CASE("Page Setup: none, a standard size turned, or a custom one", "[page]")
{
    {
        PageSetupDialog dialog(std::nullopt);
        CHECK(!dialog.page());
        CHECK(!child<QComboBox>(dialog, "pageOrientation")->isEnabled());

        auto* size = child<QComboBox>(dialog, "pageSize");
        size->setCurrentIndex(size->findData(QStringLiteral("A3")));
        child<QComboBox>(dialog, "pageOrientation")->setCurrentIndex(1);
        child<QComboBox>(dialog, "pageOrigin")->setCurrentIndex(1);
        child<QCheckBox>(dialog, "pageHasColor")->setChecked(true);
        const auto page = dialog.page();
        REQUIRE(page);
        CHECK(page->size == "A3");
        CHECK(page->width == 420);
        CHECK(page->height == 297);
        CHECK(page->origin == PageOrigin::Center);
        CHECK(page->hasColor);
        // The sizes show, but only a custom size can be typed.
        CHECK(child<QDoubleSpinBox>(dialog, "pageWidth")->value() == 420);
        CHECK(!child<QDoubleSpinBox>(dialog, "pageWidth")->isEnabled());

        size->setCurrentIndex(size->findData(QStringLiteral("custom")));
        CHECK(child<QDoubleSpinBox>(dialog, "pageWidth")->isEnabled());
        child<QDoubleSpinBox>(dialog, "pageWidth")->setValue(100);
        child<QDoubleSpinBox>(dialog, "pageHeight")->setValue(500);
        CHECK(child<QComboBox>(dialog, "pageOrientation")->currentIndex() == 0);  // (taller: portrait)
        child<QComboBox>(dialog, "pageOrientation")->setCurrentIndex(1);         // turned: the sides swap
        CHECK(dialog.page()->width == 500);
        CHECK(dialog.page()->height == 100);
        CHECK(dialog.page()->size == "custom");
    }
    {
        // Opened on an existing page: shows it.
        Page letter;
        letter.size = "Letter";
        letter.width = 279.4;
        letter.height = 215.9;
        letter.origin = PageOrigin::TopLeft;
        PageSetupDialog dialog(letter);
        CHECK(child<QComboBox>(dialog, "pageSize")->currentData().toString() == QStringLiteral("Letter"));
        CHECK(child<QComboBox>(dialog, "pageOrientation")->currentIndex() == 1);
        CHECK(dialog.page() == letter);
    }
}

TEST_CASE("The page on the canvas: undoable, fitted to, drawn", "[page]")
{
    Document doc;
    SketchView view(doc);
    view.resize(800, 600);
    view.show();
    doc.modify("shape", [](Sketch& s) {
        const Id a = s.addPoint(20, 20);
        build::rectangle(s, a, s.addPoint(80, 60));
    });
    Page page;  // A4, bottom-left on the origin
    page.hasColor = true;
    page.color = {0xff, 0xfd, 0xf5, 255};
    REQUIRE(view.tryEdit("page", [&](Editor& ed) { ed.setPage(page); }));
    CHECK(doc.sketch().page() == page);

    // Fit all shows the whole page.
    view.fitAll();
    const QPointF top = view.toScreen({0, 297});
    const QPointF bottom = view.toScreen({210, 0});
    CHECK(top.y() >= 0);
    CHECK(bottom.y() <= view.height());
    saveSnapshot(view, "_sketch");
    view.setPresentation(true);
    // Presentation: the paper in its colour, off it dimmed.
    const QImage image = view.grab().toImage();
    const QColor paper = image.pixelColor(view.toScreen({5, 290}).toPoint());
    CHECK(paper.red() > 240);
    CHECK(paper.blue() > 230);
    const QColor off = image.pixelColor(QPoint(2, 2));
    CHECK(off.red() < 60);
    saveSnapshot(view, "_presentation");

    doc.undo();
    CHECK(!doc.sketch().page());
}
