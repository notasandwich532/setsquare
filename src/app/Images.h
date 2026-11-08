#pragma once

#include <core/Sketch.h>

#include <QByteArray>
#include <QImage>
#include <QString>

#include <map>
#include <vector>

// Reference images (core::Image) on the app side: reading the files, and decoding them to draw.

namespace images
{

// Reads an image file into an Image: its bytes, pixel size, and a natural width in mm (from the
// resolution the file gives, else 96 pixels per inch). Throws core::SketchError if it can't be
// read.
core::Image load(const QString& path);
// The same from a file's bytes (e.g. embedded in another file): `source` is where it came from,
// `name` what the error calls it.
core::Image fromBytes(const QByteArray& bytes, const QString& source, const QString& name);

// Decoded pictures, kept between paints. Each is kept at a few sizes (halving), so drawing one
// zoomed out doesn't scale the whole full-size picture down every frame.
class Cache
{
public:
    // The picture for `image`, at least `pixelsWide` across where that's smaller than it is.
    // Null if it can't be decoded.
    const QImage& get(const core::Image& image, double pixelsWide);
    // Forgets pictures not asked for since the last call (deleted images, old undo steps).
    void prune();

private:
    struct Entry
    {
        std::vector<QImage> levels;  // full size, then halves
        bool used = true;
    };
    std::map<const std::string*, Entry> m_entries;  // by the image's data (shared between copies)
    QImage m_null;
};

}  // namespace images
