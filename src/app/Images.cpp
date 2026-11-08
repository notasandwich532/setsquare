#include "Images.h"

#include <QBuffer>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QImageReader>

namespace images
{

core::Image load(const QString& path)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        throw core::SketchError("can't open " + path.toStdString());
    }
    return fromBytes(file.readAll(), QDir::toNativeSeparators(QFileInfo(path).absoluteFilePath()),
                     QFileInfo(path).fileName());
}

core::Image fromBytes(const QByteArray& bytes, const QString& source, const QString& name)
{
    QBuffer buffer;
    buffer.setData(bytes);
    buffer.open(QIODevice::ReadOnly);
    QImageReader reader(&buffer);
    reader.setAutoTransform(true);  // (photos turned by their EXIF tag)
    const QImage picture = reader.read();
    if (picture.isNull()) {
        throw core::SketchError("can't read " + name.toStdString() + " as an image (" + reader.errorString().toStdString()
                                + ")");
    }
    core::Image image;
    image.source = source.toStdString();
    image.data = std::make_shared<const std::string>(bytes.constData(), static_cast<size_t>(bytes.size()));
    image.pixelWidth = picture.width();
    image.pixelHeight = picture.height();
    // Its size from its resolution when it has a sensible one, else as a screen shows it.
    const double perMetre = picture.dotsPerMeterX();
    const double pixelsPerMm = perMetre > 1000.0 ? perMetre / 1000.0 : 96.0 / 25.4;
    image.width = picture.width() / pixelsPerMm;
    return image;
}

const QImage& Cache::get(const core::Image& image, double pixelsWide)
{
    if (!image.data) {
        return m_null;
    }
    Entry& e = m_entries[image.data.get()];
    e.used = true;
    if (e.levels.empty()) {
        QBuffer buffer;
        buffer.setData(QByteArray::fromRawData(image.data->data(), static_cast<qsizetype>(image.data->size())));
        buffer.open(QIODevice::ReadOnly);
        QImageReader reader(&buffer);
        reader.setAutoTransform(true);  // (turned as the file says, as load() reads it)
        const QImage picture = reader.read();
        if (picture.isNull()) {
            m_entries.erase(image.data.get());
            return m_null;
        }
        e.levels.push_back(picture.convertToFormat(QImage::Format_ARGB32_Premultiplied));
    }
    // The smallest level still at least as wide as it's shown (made as needed).
    size_t level = 0;
    while (e.levels[level].width() / 2 >= pixelsWide && e.levels[level].width() > 64) {
        if (level + 1 == e.levels.size()) {
            const QImage& big = e.levels[level];
            e.levels.push_back(big.scaled(big.width() / 2, std::max(1, big.height() / 2), Qt::IgnoreAspectRatio,
                                          Qt::SmoothTransformation));
        }
        ++level;
    }
    return e.levels[level];
}

void Cache::prune()
{
    std::erase_if(m_entries, [](const auto& kv) { return !kv.second.used; });
    for (auto& [key, e] : m_entries) {
        e.used = false;
    }
}

}  // namespace images
