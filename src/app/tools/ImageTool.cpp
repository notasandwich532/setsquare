#include "ImageTool.h"

#include "../Document.h"
#include "../SketchView.h"

#include <QCheckBox>
#include <QDoubleSpinBox>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QLabel>
#include <QPainter>
#include <QSpinBox>
#include <QToolButton>

#include <cmath>
#include <functional>

using core::Id;
using core::kNoId;

namespace
{
const QColor kAccent(0x4d, 0xa3, 0xff);
const QColor kCalibration(0xf2, 0xb3, 0x3d);

core::Vec2 vec(const core::Point& p)
{
    return {p.x, p.y};
}
}  // namespace

ImageTool::ImageTool(SketchView& view)
    : Tool(view)
{}

ImageTool::~ImageTool()
{
    view().setImagePreview(std::nullopt);
    if (m_panel) {
        // (It goes later: nothing may reach this tool through it meanwhile.)
        QObject::disconnect(&doc(), nullptr, m_panel, nullptr);
        for (QObject* child : m_panel->findChildren<QObject*>()) {
            child->disconnect(m_panel);
        }
        m_panel->disconnect();
        m_panel->hide();
        m_panel->deleteLater();
    }
}

QString ImageTool::hint() const
{
    switch (m_step) {
        case 1: return QObject::tr("Calibrate: click the first point on the image");
        case 2: return QObject::tr("Calibrate: click the second point");
        case 3: return QObject::tr("Calibrate: type the real distance between the points, then Enter");
        default: break;
    }
    if (m_image == kNoId) {
        return QObject::tr("Image: click an image to place it (File > Insert Image to add one)");
    }
    return QObject::tr("Image: drag it to move it; set its size, angle and opacity in the panel. Calibrate: scale it "
                       "from a known distance. Enter: done");
}

// ---------------------------------------------------------------------------------------------
// The panel
// ---------------------------------------------------------------------------------------------

void ImageTool::activate()
{
    m_panel = new CommandPanel(QObject::tr("Image"), &view());
    m_file = new QLabel;
    m_file->setObjectName(QStringLiteral("imageFile"));
    m_panel->addWidget(QObject::tr("File"), m_file);

    m_opacity = m_panel->addCount(QObject::tr("Opacity"), "imageOpacity", 50, 0, 100);
    m_opacity->setSuffix(QStringLiteral(" %"));
    auto number = [this](const QString& label, const char* name, double min, double max, const QString& suffix) {
        QDoubleSpinBox* box = m_panel->addNumber(label, name, 0.0, min, max, suffix);
        box->setKeyboardTracking(false);  // (a change when it's typed, not each key)
        return box;
    };
    const QString mm = QStringLiteral(" mm");
    m_width = number(QObject::tr("Width"), "imageWidth", 0.001, 1e6, mm);
    m_height = number(QObject::tr("Height"), "imageHeight", 0.001, 1e6, mm);
    m_angle = number(QObject::tr("Angle"), "imageAngle", -360.0, 360.0, QStringLiteral(" °"));
    m_x = number(QObject::tr("Centre X"), "imageX", -1e6, 1e6, mm);
    m_y = number(QObject::tr("Centre Y"), "imageY", -1e6, 1e6, mm);
    m_opacity->setKeyboardTracking(false);

    auto* flips = new QWidget;
    auto* flipLayout = new QHBoxLayout(flips);
    flipLayout->setContentsMargins(0, 0, 0, 0);
    m_flipH = new QCheckBox(QObject::tr("Left-right"), flips);
    m_flipH->setObjectName(QStringLiteral("imageFlipH"));
    m_flipV = new QCheckBox(QObject::tr("Upside down"), flips);
    m_flipV->setObjectName(QStringLiteral("imageFlipV"));
    flipLayout->addWidget(m_flipH);
    flipLayout->addWidget(m_flipV);
    flipLayout->addStretch();
    m_panel->addWidget(QObject::tr("Flip"), flips);

    auto* buttons = new QWidget;
    auto* buttonLayout = new QHBoxLayout(buttons);
    buttonLayout->setContentsMargins(0, 2, 0, 0);
    m_calibrateButton = new QToolButton(buttons);
    m_calibrateButton->setObjectName(QStringLiteral("imageCalibrate"));
    m_calibrateButton->setText(QObject::tr("Calibrate"));
    m_calibrateButton->setToolTip(QObject::tr("Scale it from a known distance: click two points on it, then type how far "
                                              "apart they really are"));
    auto* remove = new QToolButton(buttons);
    remove->setObjectName(QStringLiteral("imageDelete"));
    remove->setText(QObject::tr("Delete"));
    remove->setToolTip(QObject::tr("Take this image out of the drawing"));
    buttonLayout->addWidget(m_calibrateButton);
    buttonLayout->addWidget(remove);
    buttonLayout->addStretch();
    m_panel->addWidget(QString(), buttons);

    m_distance = m_panel->addNumber(QObject::tr("Real distance"), "imageCalibration", 10.0, 1e-6, 1e9, mm);
    m_panel->setRowVisible(m_distance, false);

    auto field = [this](QDoubleSpinBox* box, std::function<void(core::Image&, double)> set) {
        QObject::connect(box, &QDoubleSpinBox::valueChanged, m_panel, [this, set](double v) {
            if (!m_syncing) {
                edit([&](core::Image& im) { set(im, v); });
            }
        });
    };
    field(m_width, [](core::Image& im, double v) { im.width = v; });
    field(m_height, [](core::Image& im, double v) { im.width = v * im.pixelWidth / im.pixelHeight; });
    field(m_angle, [](core::Image& im, double v) { im.angle = v; });
    field(m_x, [](core::Image& im, double v) { im.x = v; });
    field(m_y, [](core::Image& im, double v) { im.y = v; });
    QObject::connect(m_opacity, &QSpinBox::valueChanged, m_panel, [this](int v) {
        if (!m_syncing) {
            edit([v](core::Image& im) { im.opacity = v / 100.0; });
        }
    });
    QObject::connect(m_flipH, &QCheckBox::toggled, m_panel, [this](bool on) {
        if (!m_syncing) {
            edit([on](core::Image& im) { im.flipH = on; });
        }
    });
    QObject::connect(m_flipV, &QCheckBox::toggled, m_panel, [this](bool on) {
        if (!m_syncing) {
            edit([on](core::Image& im) { im.flipV = on; });
        }
    });
    QObject::connect(m_calibrateButton, &QToolButton::clicked, m_panel, [this] {
        startCalibration();
        view().setFocus();
    });
    QObject::connect(remove, &QToolButton::clicked, m_panel, [this] {
        if (m_image == kNoId) {
            return;
        }
        const Id id = m_image;
        if (view().tryEdit(QObject::tr("Delete image"), [&](Editor& ed) { ed.remove(id); })) {
            setImage(kNoId);
        }
    });
    // Enter: the distance, while calibrating; else done.
    QObject::connect(m_panel, &CommandPanel::accepted, m_panel, [this] {
        if (m_step == 3) {
            applyCalibration();
            view().setFocus();
        }
        else {
            view().setTool(ToolKind::Select);
        }
    });
    QObject::connect(m_panel, &CommandPanel::rejected, m_panel, [this] {
        if (!cancel()) {
            view().setTool(ToolKind::Select);
        }
    });
    QObject::connect(&doc(), &Document::changed, m_panel, [this] { onDocumentChanged(); });
    syncPanel();
    view().setFocus();
}

void ImageTool::syncPanel()
{
    if (!m_panel) {
        return;
    }
    m_syncing = true;
    const core::Sketch& s = doc().sketch();
    const bool has = s.hasImage(m_image);
    for (QWidget* w : std::initializer_list<QWidget*>{m_opacity, m_width, m_height, m_angle, m_x, m_y, m_flipH,
                                                      m_flipV, m_calibrateButton}) {
        w->setEnabled(has);
    }
    if (has) {
        const core::Image& im = s.image(m_image);
        m_file->setText(QFileInfo(QString::fromStdString(im.source)).fileName());
        m_file->setToolTip(QString::fromStdString(im.source));
        m_opacity->setValue(static_cast<int>(std::lround(im.opacity * 100)));
        m_width->setValue(im.width);
        m_height->setValue(im.height());
        m_angle->setValue(im.angle);
        m_x->setValue(im.x);
        m_y->setValue(im.y);
        m_flipH->setChecked(im.flipH);
        m_flipV->setChecked(im.flipV);
    }
    else {
        m_file->setText(QObject::tr("(click an image)"));
    }
    m_panel->setRowVisible(m_distance, m_step == 3);
    m_syncing = false;
}

void ImageTool::setImage(Id image)
{
    m_image = doc().sketch().hasImage(image) ? image : kNoId;
    m_step = 0;
    m_dragging = false;
    view().setImagePreview(std::nullopt);
    syncPanel();
    refresh();
}

void ImageTool::edit(const std::function<void(core::Image&)>& change)
{
    if (!doc().sketch().hasImage(m_image)) {
        return;
    }
    core::Image im = doc().sketch().image(m_image);
    change(im);
    if (im == doc().sketch().image(m_image)) {
        return;
    }
    view().tryEdit(QObject::tr("Image"), [&](Editor& ed) { ed.setImage(im); });
}

void ImageTool::onDocumentChanged()
{
    if (m_image != kNoId && !doc().sketch().hasImage(m_image)) {
        m_image = kNoId;  // (undone, or deleted)
        m_step = 0;
    }
    syncPanel();
    refresh();
}

// ---------------------------------------------------------------------------------------------
// Calibrate
// ---------------------------------------------------------------------------------------------

void ImageTool::startCalibration()
{
    if (m_image == kNoId) {
        return;
    }
    m_step = 1;
    syncPanel();
    refresh();
}

void ImageTool::calibrate(core::Vec2 a, core::Vec2 b, double distance)
{
    const double now = std::hypot(b.x - a.x, b.y - a.y);
    if (!(now > 0.0) || !(distance > 0.0)) {
        emit view().message(QObject::tr("Calibrate needs two different points and a distance more than 0"));
        return;
    }
    const double k = distance / now;
    edit([&](core::Image& im) {
        // Scaled about the first point, which stays where it is.
        im.width *= k;
        im.x = a.x + (im.x - a.x) * k;
        im.y = a.y + (im.y - a.y) * k;
    });
}

void ImageTool::applyCalibration()
{
    m_distance->interpretText();
    calibrate(m_calA, m_calB, m_distance->value());
    m_step = 0;
    syncPanel();
    refresh();
}

// ---------------------------------------------------------------------------------------------
// Input
// ---------------------------------------------------------------------------------------------

void ImageTool::press(const ToolEvent& e)
{
    if (e.button != Qt::LeftButton) {
        return;
    }
    if (m_step == 1 || m_step == 2) {
        (m_step == 1 ? m_calA : m_calB) = e.world;
        if (m_step == 2 && std::hypot(m_calB.x - m_calA.x, m_calB.y - m_calA.y) < 1e-9) {
            return;  // (the same place again)
        }
        ++m_step;
        if (m_step == 3) {
            syncPanel();
            m_distance->setValue(std::hypot(m_calB.x - m_calA.x, m_calB.y - m_calA.y));
            m_distance->setFocus();
            m_distance->selectAll();
        }
        refresh();
        return;
    }
    const core::Sketch& s = doc().sketch();
    const Id hit = view().imageAt(e.world);
    if (hit == kNoId) {
        return;
    }
    if (hit != m_image) {
        setImage(hit);
    }
    m_dragging = true;
    m_pressWorld = e.world;
    m_dragStart = s.image(m_image);
}

void ImageTool::move(const ToolEvent& e)
{
    m_hoverWorld = e.world;
    if (m_dragging) {
        core::Image im = m_dragStart;
        im.x += e.world.x - m_pressWorld.x;
        im.y += e.world.y - m_pressWorld.y;
        view().setImagePreview(im);
    }
    refresh();
}

void ImageTool::release(const ToolEvent& e)
{
    if (e.button != Qt::LeftButton || !m_dragging) {
        return;
    }
    m_dragging = false;
    const double dx = e.world.x - m_pressWorld.x, dy = e.world.y - m_pressWorld.y;
    view().setImagePreview(std::nullopt);
    if (std::hypot(dx, dy) * view().zoom() >= 1.0) {
        edit([&](core::Image& im) {
            im.x = m_dragStart.x + dx;
            im.y = m_dragStart.y + dy;
        });
    }
    refresh();
}

bool ImageTool::cancel()
{
    if (m_dragging) {
        m_dragging = false;
        view().setImagePreview(std::nullopt);
        refresh();
        return true;
    }
    if (m_step != 0) {
        m_step = 0;
        syncPanel();
        refresh();
        return true;
    }
    return false;
}

bool ImageTool::keyPress(int key, const QString& /*text*/)
{
    if (key == Qt::Key_Return || key == Qt::Key_Enter) {
        if (m_step == 3) {
            applyCalibration();
        }
        else {
            view().setTool(ToolKind::Select);  // done (this tool is gone after this)
        }
        return true;
    }
    return false;
}

// ---------------------------------------------------------------------------------------------
// Drawing
// ---------------------------------------------------------------------------------------------

void ImageTool::paintOverlay(QPainter& painter)
{
    const core::Sketch& s = doc().sketch();
    if (s.hasImage(m_image)) {
        const core::Image& im = view().imagePreview() ? *view().imagePreview() : s.image(m_image);
        QPolygonF outline;
        for (const auto& [u, v] : {std::pair{0.0, 0.0}, std::pair{1.0, 0.0}, std::pair{1.0, 1.0}, std::pair{0.0, 1.0}}) {
            outline << view().toScreen(vec(im.at(u, v)));
        }
        painter.setBrush(Qt::NoBrush);
        painter.setPen(QPen(kAccent, 1.5));
        painter.drawPolygon(outline);
        painter.setBrush(kAccent);
        for (const QPointF& corner : outline) {
            painter.drawRect(QRectF(corner - QPointF(3, 3), QSizeF(6, 6)));
        }
    }
    else if (const Id hover = view().imageAt(m_hoverWorld); hover != kNoId && m_step == 0) {
        const core::Image& im = s.image(hover);
        QPolygonF outline;
        for (const auto& [u, v] : {std::pair{0.0, 0.0}, std::pair{1.0, 0.0}, std::pair{1.0, 1.0}, std::pair{0.0, 1.0}}) {
            outline << view().toScreen(vec(im.at(u, v)));
        }
        painter.setBrush(Qt::NoBrush);
        painter.setPen(QPen(QColor(0xff, 0xff, 0xff, 200), 1.5, Qt::DashLine));
        painter.drawPolygon(outline);
    }

    // Calibration: the points and the line between them, with its length now.
    if (m_step >= 2) {
        const core::Vec2 b = m_step == 2 ? m_hoverWorld : m_calB;
        painter.setPen(QPen(QColor(0, 0, 0, 160), 3.5));
        painter.drawLine(view().toScreen(m_calA), view().toScreen(b));
        painter.setPen(QPen(kCalibration, 1.8));
        painter.drawLine(view().toScreen(m_calA), view().toScreen(b));
        const double length = std::hypot(b.x - m_calA.x, b.y - m_calA.y);
        view().drawLabel(painter, {(m_calA.x + b.x) / 2, (m_calA.y + b.y) / 2},
                         QStringLiteral("%1 mm").arg(length, 0, 'f', 2));
    }
    if (m_step >= 2 || m_step == 1) {
        painter.setPen(QPen(QColor(0xff, 0xff, 0xff), 1.5));
        painter.setBrush(kCalibration);
        if (m_step >= 2) {
            painter.drawEllipse(view().toScreen(m_calA), 4.5, 4.5);
        }
        if (m_step == 3) {
            painter.drawEllipse(view().toScreen(m_calB), 4.5, 4.5);
        }
    }
}
