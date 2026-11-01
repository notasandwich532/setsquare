// Exported SVG, rendered back with Qt's SVG renderer: is it what the drawing shows?

#include "Document.h"

#include <core/Builders.h>
#include <core/Export.h>
#include <core/Regions.h>

#include <catch2/catch_test_macros.hpp>

#include <QImage>
#include <QPainter>
#include <QSvgRenderer>
#include <QTemporaryDir>

using namespace core;

TEST_CASE("Exported SVG renders like the drawing", "[export][svg]")
{
    Document doc;
    Id arc = kNoId;
    doc.modify("drawing", [&](Sketch& s) {
        const Id a = s.addPoint(0, 0);
        build::rectangle(s, a, s.addPoint(40, 30));
        s.addCircle(20, 15, 5);  // a hole in the fill
        arc = s.addArc(60, 5, 8, 0, 90);
        StrokeStyle red;
        Color::fromHex("#ff0000", red.color);
        red.width = 1.0;
        s.setStroke(arc, red);
        Fill f;
        Color::fromHex("#0000ff", f.color);
        f.seed = Point{5, 5};
        s.addFill(f);
    });
    REQUIRE(doc.sketch().fills().size() == 1);

    QTemporaryDir dir;
    const QString path = dir.filePath("drawing.svg");
    exporter::SvgOptions o;
    o.margin = 0;
    REQUIRE(doc.exportSvg(path, o));

    QSvgRenderer renderer(path);
    REQUIRE(renderer.isValid());
    const double scale = 10.0;  // pixels per mm
    const QRectF box = renderer.viewBoxF();
    QImage image(QSize(int(box.width() * scale), int(box.height() * scale)), QImage::Format_ARGB32);
    image.fill(Qt::white);
    QPainter painter(&image);
    renderer.render(&painter);
    painter.end();

    // Sketch -> image: the left edge is the rectangle's (x = -0.125, half its line), the top
    // its top side (y = 30.125); y runs down.
    auto pixel = [&](double x, double y) { return image.pixelColor(int((x + 0.125) * scale), int((30.125 - y) * scale)); };
    CHECK(pixel(5, 5) == QColor(Qt::blue));     // filled
    CHECK(pixel(20, 15) == QColor(Qt::white));  // the hole
    CHECK(pixel(65.657, 10.657) == QColor(Qt::red));   // the arc, 45 degrees up and right of its centre
    CHECK(pixel(65.657, -0.657) != QColor(Qt::red));   // and not below
}
