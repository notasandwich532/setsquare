#include "Icons.h"

#include <QFile>
#include <QIconEngine>
#include <QPainter>
#include <QPixmap>
#include <QSvgRenderer>

namespace icons
{

namespace
{
// The colours icons are drawn in (see icons/build_icons.py).
const QByteArray kRed = "#e8615c";
const QByteArray kGrey = "#d5d9de";
const QByteArray kBlue = "#4da3ff";
const QByteArray kGreen = "#5cc97b";

QByteArray whitened(QByteArray svg)
{
    for (const QByteArray& colour : {kRed, kGrey, kBlue, kGreen}) {
        svg.replace(colour, "#ffffff");
    }
    return svg;
}

QByteArray load(const QString& name)
{
    QFile file(QStringLiteral(":/icons/%1.svg").arg(name));
    return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray();
}

// Draws an SVG at exactly the size asked for.
class SvgIconEngine : public QIconEngine
{
public:
    explicit SvgIconEngine(QByteArray normal)
        : m_normal(std::move(normal))
    {
        m_checked = whitened(m_normal);
    }

    void paint(QPainter* painter, const QRect& rect, QIcon::Mode mode, QIcon::State state) override
    {
        QSvgRenderer renderer(state == QIcon::On ? m_checked : m_normal);
        painter->save();
        if (mode == QIcon::Disabled) {
            painter->setOpacity(0.35);
        }
        renderer.render(painter, rect);
        painter->restore();
    }

    QPixmap pixmap(const QSize& size, QIcon::Mode mode, QIcon::State state) override
    {
        return scaledPixmap(size, mode, state, 1.0);
    }

    QPixmap scaledPixmap(const QSize& size, QIcon::Mode mode, QIcon::State state, qreal scale) override
    {
        QPixmap pm(size * scale);
        pm.setDevicePixelRatio(scale);
        pm.fill(Qt::transparent);
        QPainter painter(&pm);
        painter.setRenderHint(QPainter::Antialiasing);
        paint(&painter, QRect(QPoint(0, 0), size), mode, state);
        return pm;
    }

    QIconEngine* clone() const override { return new SvgIconEngine(m_normal); }
    QString key() const override { return QStringLiteral("drawapp-svg"); }

private:
    QByteArray m_normal;
    QByteArray m_checked;
};
}  // namespace

QIcon forName(const QString& name)
{
    QByteArray data = load(name);
    if (data.isEmpty()) {
        return {};
    }
    return QIcon(new SvgIconEngine(std::move(data)));
}

QByteArray svg(const QString& name, QIcon::State state)
{
    QByteArray data = load(name);
    return state == QIcon::On ? whitened(std::move(data)) : data;
}

}  // namespace icons
