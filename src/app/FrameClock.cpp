#include "FrameClock.h"

#include <QGuiApplication>
#include <QScreen>
#include <QTimer>
#include <QWidget>
#include <QWindow>

#include <atomic>
#include <condition_variable>
#include <mutex>
#include <thread>

#ifdef _WIN32
# ifndef NOMINMAX
#  define NOMINMAX
# endif
# include <windows.h>
# include <dwmapi.h>
# include <dxgi.h>
#endif

namespace
{
int frameIntervalMs(const QWidget* widget)
{
    const QScreen* screen = widget->screen() ? widget->screen() : QGuiApplication::primaryScreen();
    const double hz = screen ? screen->refreshRate() : 60.0;
    return std::max(1, static_cast<int>(1000.0 / (hz > 1.0 ? hz : 60.0)));
}
}  // namespace

#ifdef _WIN32

// Waits for the vertical blank of one monitor. Falls back to DwmFlush (next desktop composition)
// when the monitor can't be matched to a DXGI output.
class VBlankWaiter
{
public:
    VBlankWaiter() { CreateDXGIFactory1(__uuidof(IDXGIFactory1), reinterpret_cast<void**>(&m_factory)); }
    ~VBlankWaiter()
    {
        if (m_output) {
            m_output->Release();
        }
        if (m_factory) {
            m_factory->Release();
        }
    }

    // Returns false if neither DXGI nor DWM could wait (caller should sleep instead).
    bool wait(HMONITOR monitor)
    {
        if (monitor != m_monitor) {
            selectOutput(monitor);
        }
        if (m_output && SUCCEEDED(m_output->WaitForVBlank())) {
            return true;
        }
        return SUCCEEDED(DwmFlush());
    }

private:
    void selectOutput(HMONITOR monitor)
    {
        m_monitor = monitor;
        if (m_output) {
            m_output->Release();
            m_output = nullptr;
        }
        if (!m_factory || !monitor) {
            return;
        }
        IDXGIAdapter1* adapter = nullptr;
        for (UINT a = 0; !m_output && m_factory->EnumAdapters1(a, &adapter) != DXGI_ERROR_NOT_FOUND; ++a) {
            IDXGIOutput* output = nullptr;
            for (UINT o = 0; adapter->EnumOutputs(o, &output) != DXGI_ERROR_NOT_FOUND; ++o) {
                DXGI_OUTPUT_DESC desc{};
                if (SUCCEEDED(output->GetDesc(&desc)) && desc.Monitor == monitor) {
                    m_output = output;
                    break;
                }
                output->Release();
            }
            adapter->Release();
        }
    }

    IDXGIFactory1* m_factory = nullptr;
    IDXGIOutput* m_output = nullptr;
    HMONITOR m_monitor = nullptr;
};

struct FrameClock::Impl
{
    FrameClock* owner = nullptr;
    QWidget* widget = nullptr;
    std::thread thread;
    std::mutex mutex;
    std::condition_variable cv;
    bool requested = false;
    bool stop = false;
    std::atomic<HMONITOR> monitor{nullptr};
    std::atomic<int> fallbackMs{16};

    void run()
    {
        VBlankWaiter waiter;
        for (;;) {
            {
                std::unique_lock lock(mutex);
                cv.wait(lock, [this] { return requested || stop; });
                if (stop) {
                    return;
                }
                requested = false;  // requests made while we wait below roll into the next frame
            }
            if (!waiter.wait(monitor.load())) {
                std::this_thread::sleep_for(std::chrono::milliseconds(fallbackMs.load()));
            }
            QMetaObject::invokeMethod(owner, &FrameClock::frame, Qt::QueuedConnection);
        }
    }
};

FrameClock::FrameClock(QWidget* widget)
    : QObject(widget)
    , d(std::make_unique<Impl>())
{
    d->owner = this;
    d->widget = widget;
    d->thread = std::thread([this] { d->run(); });
}

FrameClock::~FrameClock()
{
    {
        std::lock_guard lock(d->mutex);
        d->stop = true;
    }
    d->cv.notify_all();
    d->thread.join();
}

void FrameClock::request()
{
    // Track the monitor on the GUI thread (cheap); the worker picks up changes.
    QWidget* top = d->widget->window();
    if (top->windowHandle() && QGuiApplication::platformName() == QLatin1String("windows")) {
        d->monitor = MonitorFromWindow(reinterpret_cast<HWND>(top->winId()), MONITOR_DEFAULTTONEAREST);
    }
    d->fallbackMs = frameIntervalMs(d->widget);
    {
        std::lock_guard lock(d->mutex);
        d->requested = true;
    }
    d->cv.notify_one();
}

#else  // !_WIN32

struct FrameClock::Impl
{
    QWidget* widget = nullptr;
    QTimer timer;
};

FrameClock::FrameClock(QWidget* widget)
    : QObject(widget)
    , d(std::make_unique<Impl>())
{
    d->widget = widget;
    d->timer.setSingleShot(true);
    d->timer.setTimerType(Qt::PreciseTimer);
    connect(&d->timer, &QTimer::timeout, this, &FrameClock::frame);
}

FrameClock::~FrameClock() = default;

void FrameClock::request()
{
    if (!d->timer.isActive()) {
        d->timer.start(frameIntervalMs(d->widget));
    }
}

#endif
