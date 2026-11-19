// Exported SVG, rendered back with Qt's SVG renderer: is it what the drawing shows?

#include "Document.h"
#include "Editor.h"
#include "TextShapes.h"

#include <core/Builders.h>
#include <core/Export.h>
#include <core/Regions.h>

#include <catch2/catch_test_macros.hpp>

#include <QFile>
#include <QFileInfo>
#include <QImage>
#include <QImageReader>
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

TEST_CASE("PDF export: the page at true size, text as text in its embedded font", "[export][pdf]")
{
    Document doc;
    Id anchor = kNoId;
    doc.edit("page", [&](Editor& ed) {
        Page page;
        page.size = "A5";
        page.width = 148;
        page.height = 210;
        ed.setPage(page);
    });
    doc.modify("drawing", [&](Sketch& s) {
        build::rectangle(s, s.addPoint(10, 10), s.addPoint(60, 40));
        anchor = s.addPoint(15, 20);
    });
    doc.edit("text", [&](Editor& ed) {
        Text t;
        t.text = "Hello PDF";
        t.font = textshapes::defaultFamily().toStdString();  // (as the Text tool sets it)
        t.height = 5;
        t.anchor = anchor;
        ed.text(t);
    });
    doc.edit("fill", [&](Editor& ed) { ed.fill({50, 35}, Color{0x20, 0x80, 0xff, 255}); });

    // The SVG in pieces: the text cut out of it, to be drawn in its font.
    const exporter::SvgPieces pieces = exporter::toSvgPieces(doc.sketch(), {});
    REQUIRE(pieces.svgs.size() == 2);
    REQUIRE(pieces.texts.size() == 2);
    REQUIRE(pieces.texts[0].size() == 1);
    CHECK(pieces.texts[0][0].text == "Hello PDF");
    CHECK(std::abs(pieces.texts[0][0].x - 15.0) < 1e-6);         // (from the page's left)
    CHECK(std::abs(pieces.texts[0][0].y - (210.0 - 20.0)) < 1e-6);  // (from its top)
    CHECK(pieces.texts[1].empty());
    for (const std::string& svg : pieces.svgs) {
        CHECK(svg.find("<text") == std::string::npos);
        CHECK(QSvgRenderer(QByteArray::fromStdString(svg)).isValid());
    }
    CHECK(exporter::toSvg(doc.sketch(), {}).find("<text") != std::string::npos);  // (the SVG itself keeps it)

    QTemporaryDir dir;
    const QString path = dir.filePath(QStringLiteral("page.pdf"));
    QString error;
    REQUIRE(doc.exportPdf(path, {}, &error));
    QFile file(path);
    REQUIRE(file.open(QIODevice::ReadOnly));
    const QByteArray pdf = file.readAll();
    file.close();  // (Windows won't replace an open file)
    CHECK(pdf.startsWith("%PDF-"));
    CHECK(pdf.contains("/MediaBox [0 0 420.000000 595.000000]"));  // A5, in whole points
    CHECK(pdf.contains("/FontFile"));                               // the text's font, embedded
    CHECK(pdf.contains("/Creator"));

    // Without a page: fitted round the drawing (50 x 30 mm, and the margin).
    doc.edit("no page", [](Editor& ed) { ed.setPage(std::nullopt); });
    exporter::SvgOptions o;
    o.margin = 5;
    REQUIRE(doc.exportPdf(path, o, &error));
    QFile again(path);
    REQUIRE(again.open(QIODevice::ReadOnly));
    const QByteArray fitted = again.readAll();
    const auto at = fitted.indexOf("/MediaBox [0 0 ");
    REQUIRE(at >= 0);
    const QByteArray numbers = fitted.mid(at + 15, 40);
    const QList<QByteArray> box = numbers.left(numbers.indexOf(']')).split(' ');
    CHECK(std::abs(box[0].toDouble() - 60.0 * 72 / 25.4) < 1.0);  // 50 + 2 x 5 mm wide (to the point)
    CHECK(std::abs(box[1].toDouble() - 40.0 * 72 / 25.4) < 1.0);  // 30 + 2 x 5 mm high
}

namespace
{
QImage readImage(const QString& path)
{
    QImageReader reader(path);
    QImage image = reader.read();
    REQUIRE(!image.isNull());
    return image;
}
// Pixels in `area` not near `background` (something drawn there).
int drawnIn(const QImage& image, const QRect& area, QColor background)
{
    int n = 0;
    for (int y = area.top(); y <= area.bottom(); ++y) {
        for (int x = area.left(); x <= area.right(); ++x) {
            const QColor p = image.pixelColor(x, y);
            n += std::abs(p.red() - background.red()) + std::abs(p.green() - background.green())
                     + std::abs(p.blue() - background.blue()) > 60;
        }
    }
    return n;
}
}  // namespace

TEST_CASE("PNG and JPG export: the page at the resolution asked, as the PDF draws it", "[export][image]")
{
    Document doc;
    Id anchor = kNoId;
    doc.edit("page", [&](Editor& ed) {
        Page page;
        page.size = "A5";
        page.width = 148;
        page.height = 210;
        ed.setPage(page);
    });
    doc.modify("drawing", [&](Sketch& s) {
        build::rectangle(s, s.addPoint(10, 10), s.addPoint(60, 40));
        anchor = s.addPoint(15, 100);
    });
    doc.edit("text", [&](Editor& ed) {
        Text t;
        t.text = "HELLO";
        t.font = textshapes::defaultFamily().toStdString();
        t.height = 10;
        t.anchor = anchor;
        ed.text(t);
    });
    doc.edit("fill", [&](Editor& ed) { ed.fill({50, 35}, Color{0x20, 0x80, 0xff, 255}); });
    QTemporaryDir dir;
    const double dpi = 100.0;
    const double perMm = dpi / 25.4;
    // Sketch mm -> pixel (y down, from the page's top).
    auto at = [&](double x, double y) { return QPoint(int(x * perMm), int((210 - y) * perMm)); };

    SECTION("PNG")
    {
        const QString path = dir.filePath(QStringLiteral("page.png"));
        Document::ImageOptions image;
        image.dpi = dpi;
        QString error;
        REQUIRE(doc.exportImage(path, {}, image, &error));
        CHECK(doc.exportImageSize({}, dpi) == QSize(583, 827));  // 148 x 210 mm at 100 dpi
        const QImage png = readImage(path);
        CHECK(png.size() == QSize(583, 827));
        CHECK(QFileInfo(path).size() < 583 * 827 * 4 / 10);  // (compressed: mostly white)
        CHECK(std::abs(png.dotsPerMeterX() - dpi / 0.0254) < 2);  // (prints at true size)
        CHECK(png.pixelColor(at(30, 25)) == QColor(0x20, 0x80, 0xff));  // the fill
        CHECK(png.pixelColor(at(100, 150)) == QColor(Qt::white));      // nothing
        // The text, drawn (in its font) where it is: dark pixels from its anchor up.
        CHECK(drawnIn(png, QRect(at(15, 110), at(60, 100)), Qt::white) > 100);
        CHECK(drawnIn(png, QRect(at(80, 110), at(140, 100)), Qt::white) == 0);
    }
    SECTION("Transparent PNG, without a page: fitted round the drawing, nothing behind it")
    {
        doc.edit("no page", [](Editor& ed) { ed.setPage(std::nullopt); });
        const QString path = dir.filePath(QStringLiteral("clear.png"));
        Document::ImageOptions image;
        image.dpi = dpi;
        image.transparent = true;
        exporter::SvgOptions o;
        o.margin = 5;
        REQUIRE(doc.exportImage(path, o, image));
        const QImage png = readImage(path);
        CHECK(png.hasAlphaChannel());
        CHECK(png.pixelColor(1, 1).alpha() == 0);  // (in the margin)
        CHECK(png.pixelColor(png.width() / 2, png.height() - int(10 * perMm)).alpha() == 255);  // the fill
    }
    SECTION("JPG: no transparency (white behind), and its quality")
    {
        doc.edit("no page", [](Editor& ed) { ed.setPage(std::nullopt); });
        const QString low = dir.filePath(QStringLiteral("low.jpg")), high = dir.filePath(QStringLiteral("high.jpeg"));
        Document::ImageOptions image;
        image.dpi = dpi;
        image.transparent = true;  // (ignored)
        image.quality = 20;
        REQUIRE(doc.exportImage(low, {}, image));
        image.quality = 98;
        REQUIRE(doc.exportImage(high, {}, image));
        const QImage jpg = readImage(high);
        CHECK(!jpg.hasAlphaChannel());
        CHECK(drawnIn(jpg, QRect(0, 0, 3, 3), Qt::white) == 0);  // white in the margin
        CHECK(QFileInfo(low).size() < QFileInfo(high).size());
    }
    SECTION("Errors: an unknown suffix, or too big")
    {
        QString error;
        CHECK(!doc.exportImage(dir.filePath(QStringLiteral("page.bmp")), {}, {}, &error));
        CHECK(error.contains(QStringLiteral(".png")));
        Document::ImageOptions huge;
        huge.dpi = 9000;  // (A5: 52000 pixels high)
        CHECK(!doc.exportImage(dir.filePath(QStringLiteral("huge.png")), {}, huge, &error));
        CHECK(error.contains(QStringLiteral("too big")));
        CHECK(!QFile::exists(dir.filePath(QStringLiteral("huge.png"))));
    }
}
