#pragma once

#include <QObject>

#include <memory>

class QWidget;

// Paces repaints to the display: request() asks for one frame() signal at the next vertical
// blank of the monitor the widget's window is on (like requestAnimationFrame). Multiple
// requests before that vblank collapse into one frame.
//
// Windows: a worker thread waits on IDXGIOutput::WaitForVBlank for the window's monitor.
// Elsewhere (or if that fails): a precise timer at the screen's refresh rate.
class FrameClock : public QObject
{
    Q_OBJECT

public:
    explicit FrameClock(QWidget* widget);
    ~FrameClock() override;

    void request();

signals:
    void frame();

private:
    struct Impl;
    std::unique_ptr<Impl> d;
};
