// Tool icons: every one is there, renders, and recolours for the active state.

#include "Icons.h"
#include "MainWindow.h"

#include <catch2/catch_test_macros.hpp>

#include <QDir>
#include <QImage>
#include <QPainter>
#include <QSvgRenderer>

namespace
{
const ToolKind kConstraintTools[] = {ToolKind::Dimension,     ToolKind::Coincident, ToolKind::HorizontalVertical,
                                     ToolKind::Parallel,      ToolKind::Perpendicular, ToolKind::Tangent,
                                     ToolKind::Equal,         ToolKind::Ratio,      ToolKind::Midpoint,   ToolKind::Concentric,
                                     ToolKind::Symmetric,     ToolKind::Collinear,  ToolKind::Fix};

const ToolKind kCreateTools[] = {
    ToolKind::Line,           ToolKind::Point,           ToolKind::Rectangle,           ToolKind::Rectangle3Point,
    ToolKind::RectangleCenter, ToolKind::Circle,         ToolKind::Circle2Point,        ToolKind::Circle3Point,
    ToolKind::CircleTangent2, ToolKind::CircleTangent3, ToolKind::ArcCenter,           ToolKind::Arc3Point,
    ToolKind::ArcTangent,     ToolKind::PolygonInscribed, ToolKind::PolygonCircumscribed, ToolKind::PolygonEdge,
    ToolKind::SlotCenterToCenter, ToolKind::SlotOverall, ToolKind::SlotCenterPoint,    ToolKind::SlotArc3Point,
    ToolKind::SlotArcCenter, ToolKind::Select, ToolKind::Text};

const ToolKind kModifyTools[] = {
    ToolKind::Trim,         ToolKind::TrimToConstruction, ToolKind::Extend,        ToolKind::Break,
    ToolKind::Fillet,       ToolKind::ChamferEqual,       ToolKind::ChamferTwoDistances, ToolKind::ChamferDistanceAngle,
    ToolKind::Move,         ToolKind::Scale,              ToolKind::Mirror,        ToolKind::RectangularPattern,
    ToolKind::CircularPattern, ToolKind::Offset,             ToolKind::Divide};

const ToolKind kAppearanceTools[] = {ToolKind::StrokeBrush, ToolKind::Fill};

QImage render(const QByteArray& svg, int size, const QColor& background)
{
    QImage image(size, size, QImage::Format_ARGB32);
    image.fill(background);
    QSvgRenderer renderer(svg);
    QPainter painter(&image);
    renderer.render(&painter, QRectF(0, 0, size, size));
    return image;
}

int inkPixels(const QImage& image, const QColor& background)
{
    int n = 0;
    for (int y = 0; y < image.height(); ++y) {
        for (int x = 0; x < image.width(); ++x) {
            n += image.pixelColor(x, y) != background;
        }
    }
    return n;
}

void checkIcons(const QList<ToolKind>& kinds)
{
    const QColor dark(0x2b, 0x2f, 0x36);
    for (ToolKind kind : kinds) {
        const QString name = MainWindow::toolName(kind);
        INFO(name.toStdString());
        const QByteArray svg = icons::svg(name);
        REQUIRE(!svg.isEmpty());
        REQUIRE(QSvgRenderer(svg).isValid());
        CHECK(inkPixels(render(svg, 24, dark), dark) > 24);  // something visible at toolbar size
        CHECK_FALSE(icons::forName(name).isNull());
        // Active: all white.
        const QByteArray on = icons::svg(name, QIcon::On);
        for (const char* colour : {"#e8615c", "#d5d9de", "#4da3ff", "#5cc97b", "#f2b33d"}) {
            CHECK(!on.contains(colour));
        }
    }
}
}  // namespace

TEST_CASE("Every constraint tool has an icon that renders", "[icons]")
{
    checkIcons(QList<ToolKind>(std::begin(kConstraintTools), std::end(kConstraintTools)));
}

TEST_CASE("Every create tool has an icon that renders", "[icons]")
{
    checkIcons(QList<ToolKind>(std::begin(kCreateTools), std::end(kCreateTools)));
}

TEST_CASE("Icon contact sheet", "[icons][snapshot]")
{
    // With DRAWAPP_SNAPSHOT_DIR set, writes every icon at 16, 24 and 48 px on the dark theme, and
    // the active version on the green button, for checking by eye.
    const QString dir = qEnvironmentVariable("DRAWAPP_SNAPSHOT_DIR");
    const QStringList names = QDir(QStringLiteral(":/icons")).entryList({QStringLiteral("*.svg")}, QDir::Files, QDir::Name);
    REQUIRE(!names.isEmpty());
    if (dir.isEmpty()) {
        return;
    }
    const int row = 64;
    QImage sheet(16 + 4 * 72 + 200, int(names.size()) * row, QImage::Format_ARGB32);
    sheet.fill(QColor(0x2b, 0x2f, 0x36));
    QPainter painter(&sheet);
    painter.setPen(QColor(0xd5, 0xd9, 0xde));
    for (int i = 0; i < names.size(); ++i) {
        const QString name = QFileInfo(names[i]).completeBaseName();
        const int y = i * row;
        int x = 16;
        for (int size : {16, 24, 48}) {
            painter.drawImage(x, y + (row - size) / 2, render(icons::svg(name), size, QColor(0x2b, 0x2f, 0x36)));
            x += 72;
        }
        painter.fillRect(x - 4, y + 16, 32, 32, QColor(0x2e, 0x7d, 0x32));
        painter.drawImage(x, y + 20, render(icons::svg(name, QIcon::On), 24, QColor(0x2e, 0x7d, 0x32)));
        painter.drawText(x + 48, y + row / 2 + 5, name);
    }
    painter.end();
    CHECK(sheet.save(QDir(dir).filePath(QStringLiteral("icons.png"))));
}

TEST_CASE("Every modify tool has an icon that renders", "[icons]")
{
    checkIcons(QList<ToolKind>(std::begin(kModifyTools), std::end(kModifyTools)));
}
TEST_CASE("The appearance tools and presentation view have icons that render", "[icons]")
{
    checkIcons(QList<ToolKind>(std::begin(kAppearanceTools), std::end(kAppearanceTools)));
    const QByteArray svg = icons::svg(QStringLiteral("presentation"));
    REQUIRE(!svg.isEmpty());
    CHECK(QSvgRenderer(svg).isValid());
    CHECK(!icons::svg(QStringLiteral("presentation"), QIcon::On).contains("#f2b33d"));
}
TEST_CASE("The app icon is compiled in and draws at window/taskbar sizes", "[icons]")
{
    const QIcon icon = icons::appIcon();
    REQUIRE_FALSE(icon.isNull());
    for (int size : {16, 32, 256}) {
        INFO(size);
        const QImage image = icon.pixmap(size).toImage();
        CHECK(image.width() == size);
        CHECK(inkPixels(image, Qt::transparent) > size * size / 2);  // a filled tile, mostly
    }
}