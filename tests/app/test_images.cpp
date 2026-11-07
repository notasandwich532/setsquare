// Reference images in the UI: the Image tool (panel, dragging, Calibrate), double-click to place
// one, the browser's rows, and how they're drawn.

#include "BrowserPanel.h"
#include "CommandPanel.h"
#include "Document.h"
#include "Images.h"
#include "SketchView.h"
#include "Snapshot.h"
#include "tools/ImageTool.h"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <QDoubleSpinBox>
#include <QSpinBox>
#include <QTemporaryDir>
#include <QTest>
#include <QToolButton>
#include <QTreeWidget>

using namespace core;
using Catch::Approx;

namespace
{

// A 40 x 20 picture: red on the left half, blue on the right.
QString makePicture(const QTemporaryDir& dir)
{
    QImage picture(40, 20, QImage::Format_RGB32);
    for (int x = 0; x < 40; ++x) {
        for (int y = 0; y < 20; ++y) {
            picture.setPixelColor(x, y, x < 20 ? QColor(220, 30, 30) : QColor(30, 30, 220));
        }
    }
    const QString path = dir.filePath(QStringLiteral("plan.png"));
    picture.save(path);
    return path;
}

struct Fixture
{
    QTemporaryDir dir;
    Document doc;
    SketchView view{doc};
    Id image = kNoId;

    Fixture()
    {
        view.resize(800, 600);
        view.show();
        view.resetView();
        core::Image im = images::load(makePicture(dir));
        im.width = 80;  // 80 x 40 mm, round the origin
        im.opacity = 1.0;
        REQUIRE(view.tryEdit("insert", [&](Editor& ed) { image = ed.addImage(im); }));
    }
    ~Fixture() { saveSnapshot(view); }

    QPoint at(double x, double y) const { return view.toScreen({x, y}).toPoint(); }
    void click(double x, double y)
    {
        QTest::mouseMove(&view, at(x, y));
        QTest::mouseClick(&view, Qt::LeftButton, {}, at(x, y));
    }
    void drag(double x0, double y0, double x1, double y1)
    {
        QTest::mouseMove(&view, at(x0, y0));
        QTest::mousePress(&view, Qt::LeftButton, {}, at(x0, y0));
        for (int i = 1; i <= 5; ++i) {
            QTest::mouseMove(&view, at(x0 + (x1 - x0) * i / 5, y0 + (y1 - y0) * i / 5));
        }
        QTest::mouseRelease(&view, Qt::LeftButton, {}, at(x1, y1));
    }
    const core::Image& im() const { return doc.sketch().image(image); }
    template <typename T>
    T* input(const char* name) const
    {
        T* w = nullptr;
        for (CommandPanel* p : view.findChildren<CommandPanel*>()) {
            if (p->isVisible()) {
                w = p->findChild<T*>(QLatin1String(name));
            }
        }
        REQUIRE(w);
        return w;
    }
};

}  // namespace

TEST_CASE("Images are drawn under the sketch, not in presentation view", "[image]")
{
    Fixture f;
    const QImage shot = f.view.grab().toImage();
    const QColor left = shot.pixelColor(f.at(-17.3, 7.3));
    const QColor right = shot.pixelColor(f.at(17.3, 7.3));
    CHECK(left.red() > 150);
    CHECK(left.blue() < 100);
    CHECK(right.blue() > 150);
    CHECK(right.red() < 100);

    f.view.setPresentation(true);
    const QColor hidden = f.view.grab().toImage().pixelColor(f.at(-17.3, 7.3));
    CHECK(hidden.red() < 100);
    f.view.setPresentation(false);

    // Flipped left to right: blue on the left.
    core::Image flipped = f.im();
    flipped.flipH = true;
    REQUIRE(f.view.tryEdit("flip", [&](Editor& ed) { ed.setImage(flipped); }));
    CHECK(f.view.grab().toImage().pixelColor(f.at(-17.3, 7.3)).blue() > 150);
}

TEST_CASE("The image tool: double-click an image, set it in the panel, drag it", "[image]")
{
    Fixture f;
    QTest::mouseDClick(&f.view, Qt::LeftButton, {}, f.at(10, 5));
    REQUIRE(f.view.toolKind() == ToolKind::Image);
    f.input<QDoubleSpinBox>("imageWidth")->setValue(120);
    CHECK(f.im().width == Approx(120));
    CHECK(f.input<QDoubleSpinBox>("imageHeight")->value() == Approx(60));
    f.input<QDoubleSpinBox>("imageHeight")->setValue(30);
    CHECK(f.im().width == Approx(60));
    f.input<QSpinBox>("imageOpacity")->setValue(40);
    CHECK(f.im().opacity == Approx(0.4));
    f.input<QDoubleSpinBox>("imageAngle")->setValue(30);
    CHECK(f.im().angle == Approx(30));
    f.input<QDoubleSpinBox>("imageAngle")->setValue(0);

    // Dragged: moved by as much, as one step.
    f.drag(5, 5, 25, -5);
    CHECK(f.im().x == Approx(20).margin(0.3));
    CHECK(f.im().y == Approx(-10).margin(0.3));
    f.doc.undo();
    CHECK(f.im().x == Approx(0));

    // Enter: done.
    QTest::keyClick(&f.view, Qt::Key_Return);
    CHECK(f.view.toolKind() == ToolKind::Select);
}

TEST_CASE("Calibrate: two points on the image and the real distance between them", "[image]")
{
    Fixture f;
    f.view.editImage(f.image);
    REQUIRE(f.view.toolKind() == ToolKind::Image);
    f.input<QToolButton>("imageCalibrate")->click();
    // Two marks 40 mm apart (as it's placed now) that are really 100 mm apart.
    f.click(-20, 0);
    f.click(20, 0);
    auto* distance = f.input<QDoubleSpinBox>("imageCalibration");
    CHECK(distance->isVisible());
    CHECK(distance->value() == Approx(40).margin(0.5));
    distance->setValue(100);
    for (CommandPanel* p : f.view.findChildren<CommandPanel*>()) {
        if (p->isVisible()) {
            emit p->accepted();  // (Enter in it)
        }
    }
    // Scaled 2.5x about the first point, which stays put.
    const double k = 100.0 / 40.0;
    CHECK(f.im().width == Approx(80 * k).epsilon(0.02));
    const Point first = f.im().at(0.25, 0.5);
    CHECK(first.x == Approx(-20).margin(0.5));
    CHECK(first.y == Approx(0).margin(0.5));
    CHECK(!distance->isVisible());
    CHECK(f.view.toolKind() == ToolKind::Image);  // (Enter applied the distance, it didn't close the tool)

    // Esc part way: no change.
    const double width = f.im().width;
    f.input<QToolButton>("imageCalibrate")->click();
    f.click(0, 0);
    QTest::keyClick(&f.view, Qt::Key_Escape);
    CHECK(f.im().width == width);
    CHECK(f.view.toolKind() == ToolKind::Image);
}

TEST_CASE("Images in the browser: listed under their layer, hidden with the eye", "[image]")
{
    Fixture f;
    BrowserPanel browser(f.doc, f.view);
    QTreeWidgetItem* row = browser.rowForImage(f.image);
    REQUIRE(row);
    CHECK(row->text(0) == QStringLiteral("plan.png"));
    auto* tree = browser.findChild<QTreeWidget*>(QStringLiteral("browser"));
    tree->itemClicked(row, 1);  // the eye
    CHECK(!f.im().visible);
    CHECK(f.view.grab().toImage().pixelColor(f.at(-17.3, 7.3)).red() < 100);
    row = browser.rowForImage(f.image);
    tree->itemClicked(row, 1);
    CHECK(f.im().visible);
    row = browser.rowForImage(f.image);
    tree->itemClicked(row, 0);  // the name: place it
    CHECK(f.view.toolKind() == ToolKind::Image);
}
