// Makes the Windows .ico for the app from its SVG (icons/app/setsquare.svg), and a preview PNG:
//
//   make_app_icon icons/app/setsquare.svg icons/app/setsquare.ico [preview.png]
//
// Build it with `cmake --build --preset windows-msvc --target make_app_icon` (it isn't built by
// default) and run it with Qt's bin directory on PATH. Each size is drawn from the SVG (not
// scaled down from a big one), and stored as PNG inside the .ico, as Windows Vista onwards reads.

#include <QBuffer>
#include <QFile>
#include <QGuiApplication>
#include <QImage>
#include <QPainter>
#include <QSvgRenderer>

#include <cstdio>
#include <vector>

namespace
{
const int kSizes[] = {16, 20, 24, 32, 40, 48, 64, 128, 256};

QImage render(QSvgRenderer& svg, int size)
{
    QImage image(size, size, QImage::Format_ARGB32_Premultiplied);
    image.fill(Qt::transparent);
    QPainter painter(&image);
    painter.setRenderHint(QPainter::Antialiasing);
    svg.render(&painter, QRectF(0, 0, size, size));
    return image;
}

void put16(QByteArray& out, int v)
{
    out.append(char(v & 0xff)).append(char((v >> 8) & 0xff));
}

void put32(QByteArray& out, qint64 v)
{
    put16(out, int(v & 0xffff));
    put16(out, int((v >> 16) & 0xffff));
}
}  // namespace

int main(int argc, char* argv[])
{
    qputenv("QT_QPA_PLATFORM", "offscreen");
    QGuiApplication app(argc, argv);
    const QStringList args = app.arguments();
    if (args.size() < 3) {
        std::fprintf(stderr, "usage: make_app_icon in.svg out.ico [preview.png]\n");
        return 2;
    }
    QSvgRenderer svg(args[1]);
    if (!svg.isValid()) {
        std::fprintf(stderr, "can't read %s\n", qPrintable(args[1]));
        return 1;
    }

    std::vector<QByteArray> pngs;
    for (int size : kSizes) {
        QByteArray png;
        QBuffer buffer(&png);
        buffer.open(QIODevice::WriteOnly);
        render(svg, size).save(&buffer, "PNG");
        pngs.push_back(png);
    }

    // ICONDIR, then an ICONDIRENTRY per image, then the images.
    QByteArray ico;
    put16(ico, 0);  // reserved
    put16(ico, 1);  // icon
    put16(ico, int(pngs.size()));
    qint64 offset = 6 + 16 * qint64(pngs.size());
    for (size_t i = 0; i < pngs.size(); ++i) {
        const int size = kSizes[i];
        ico.append(char(size >= 256 ? 0 : size));  // width (0 means 256)
        ico.append(char(size >= 256 ? 0 : size));  // height
        ico.append(char(0));                       // no palette
        ico.append(char(0));                       // reserved
        put16(ico, 1);                             // colour planes
        put16(ico, 32);                            // bits per pixel
        put32(ico, pngs[i].size());
        put32(ico, offset);
        offset += pngs[i].size();
    }
    for (const QByteArray& png : pngs) {
        ico.append(png);
    }
    QFile out(args[2]);
    if (!out.open(QIODevice::WriteOnly) || out.write(ico) != ico.size()) {
        std::fprintf(stderr, "can't write %s\n", qPrintable(args[2]));
        return 1;
    }
    std::printf("wrote %s (%d sizes, %lld bytes)\n", qPrintable(args[2]), int(pngs.size()), qint64(ico.size()));

    if (args.size() > 3) {
        // Every size side by side, on the dark theme's background.
        int width = 0;
        for (int size : kSizes) {
            width += size + 16;
        }
        QImage sheet(width, 256 + 32, QImage::Format_ARGB32);
        sheet.fill(QColor(0x2b, 0x2f, 0x36));
        QPainter painter(&sheet);
        int x = 8;
        for (int size : kSizes) {
            painter.drawImage(x, 16 + (256 - size) / 2, render(svg, size));
            x += size + 16;
        }
        painter.end();
        sheet.save(args[3]);
    }
    return 0;
}
