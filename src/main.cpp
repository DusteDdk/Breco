#include <atomic>
#include <chrono>
#include <mutex>
#include <thread>

#include <QApplication>
#include <QEvent>
#include <QObject>
#include <QString>
#include <QStringList>
#include <QtGlobal>

#include "app/MainWindow.h"
#include "debug/SelectionTrace.h"

namespace {

int eventTraceSlowThresholdMs() {
    static const int value = []() {
        bool ok = false;
        const int parsed = qEnvironmentVariableIntValue("BRECO_EVENTTRACE_SLOW_MS", &ok);
        if (!ok || parsed <= 0) {
            return 50;
        }
        return parsed;
    }();
    return value;
}

int eventTracePingMs() {
    static const int value = []() {
        bool ok = false;
        const int parsed = qEnvironmentVariableIntValue("BRECO_EVENTTRACE_PING_MS", &ok);
        if (!ok || parsed <= 0) {
            return 50;
        }
        return parsed;
    }();
    return value;
}

int eventTraceRepeatMs() {
    static const int value = []() {
        bool ok = false;
        const int parsed = qEnvironmentVariableIntValue("BRECO_EVENTTRACE_REPEAT_MS", &ok);
        if (!ok || parsed <= 0) {
            return 250;
        }
        return parsed;
    }();
    return value;
}

const char* eventTypeName(QEvent::Type type) {
    switch (type) {
        case QEvent::None:
            return "None";
        case QEvent::Timer:
            return "Timer";
        case QEvent::MouseButtonPress:
            return "MouseButtonPress";
        case QEvent::MouseButtonRelease:
            return "MouseButtonRelease";
        case QEvent::MouseMove:
            return "MouseMove";
        case QEvent::Wheel:
            return "Wheel";
        case QEvent::KeyPress:
            return "KeyPress";
        case QEvent::KeyRelease:
            return "KeyRelease";
        case QEvent::FocusIn:
            return "FocusIn";
        case QEvent::FocusOut:
            return "FocusOut";
        case QEvent::Paint:
            return "Paint";
        case QEvent::Move:
            return "Move";
        case QEvent::Resize:
            return "Resize";
        case QEvent::Show:
            return "Show";
        case QEvent::Hide:
            return "Hide";
        case QEvent::Close:
            return "Close";
        case QEvent::PolishRequest:
            return "PolishRequest";
        case QEvent::Polish:
            return "Polish";
        case QEvent::LayoutRequest:
            return "LayoutRequest";
        case QEvent::UpdateRequest:
            return "UpdateRequest";
        case QEvent::MetaCall:
            return "MetaCall";
        case QEvent::DeferredDelete:
            return "DeferredDelete";
        case QEvent::StyleChange:
            return "StyleChange";
        default:
            return "Other";
    }
}

class BrecoApplication : public QApplication {
public:
    using QApplication::QApplication;

    ~BrecoApplication() override {
        m_stopWatchdog.store(true, std::memory_order_release);
        if (m_watchdogThread.joinable()) {
            m_watchdogThread.join();
        }
    }

    bool notify(QObject* receiver, QEvent* event) override {
        if (!breco::debug::selectionTraceEnabled()) {
            return QApplication::notify(receiver, event);
        }
        startWatchdogIfNeeded();

        const quint64 startUs = breco::debug::selectionTraceElapsedUs();
        const QString receiverClass =
            (receiver != nullptr && receiver->metaObject() != nullptr)
                ? QString::fromLatin1(receiver->metaObject()->className())
                : QStringLiteral("-");
        const QString receiverName =
            (receiver != nullptr && !receiver->objectName().isEmpty())
                ? receiver->objectName()
                : QStringLiteral("-");
        const int eventType = (event != nullptr) ? static_cast<int>(event->type()) : -1;
        const char* eventName = (event != nullptr) ? eventTypeName(event->type()) : "null";

        {
            std::lock_guard<std::mutex> lock(m_activeEventMutex);
            m_activeEvent.inProgress = true;
            m_activeEvent.startUs = startUs;
            m_activeEvent.lastProgressLogUs = startUs;
            m_activeEvent.receiverClass = receiverClass;
            m_activeEvent.receiverName = receiverName;
            m_activeEvent.eventType = eventType;
            m_activeEvent.eventName = eventName;
        }

        const bool handled = QApplication::notify(receiver, event);

        const quint64 elapsedUs = breco::debug::selectionTraceElapsedUs() - startUs;
        {
            std::lock_guard<std::mutex> lock(m_activeEventMutex);
            m_activeEvent.inProgress = false;
        }

        if (elapsedUs >= static_cast<quint64>(eventTraceSlowThresholdMs()) * 1000ULL) {
            BRECO_SELTRACE(QStringLiteral(
                               "event slow-finish: receiver=%1(%2) event=%3(%4) elapsed=%5us")
                               .arg(receiverClass)
                               .arg(receiverName)
                               .arg(QString::fromLatin1(eventName))
                               .arg(eventType)
                               .arg(elapsedUs));
        }
        return handled;
    }

private:
    struct ActiveEventState {
        bool inProgress = false;
        quint64 startUs = 0;
        quint64 lastProgressLogUs = 0;
        QString receiverClass;
        QString receiverName;
        int eventType = -1;
        const char* eventName = "null";
    };

    void startWatchdogIfNeeded() {
        if (m_watchdogStarted.exchange(true, std::memory_order_acq_rel)) {
            return;
        }
        m_watchdogThread = std::thread([this]() {
            while (!m_stopWatchdog.load(std::memory_order_acquire)) {
                std::this_thread::sleep_for(
                    std::chrono::milliseconds(eventTracePingMs()));
                if (!breco::debug::selectionTraceEnabled()) {
                    continue;
                }

                ActiveEventState snapshot;
                bool shouldLog = false;
                const quint64 nowUs = breco::debug::selectionTraceElapsedUs();
                {
                    std::lock_guard<std::mutex> lock(m_activeEventMutex);
                    if (!m_activeEvent.inProgress) {
                        continue;
                    }
                    const quint64 elapsedUs = nowUs - m_activeEvent.startUs;
                    if (elapsedUs < static_cast<quint64>(eventTraceSlowThresholdMs()) * 1000ULL) {
                        continue;
                    }
                    if (nowUs - m_activeEvent.lastProgressLogUs <
                        static_cast<quint64>(eventTraceRepeatMs()) * 1000ULL) {
                        continue;
                    }
                    m_activeEvent.lastProgressLogUs = nowUs;
                    snapshot = m_activeEvent;
                    shouldLog = true;
                }

                if (!shouldLog) {
                    continue;
                }
                const quint64 elapsedUs = nowUs - snapshot.startUs;
                BRECO_SELTRACE(QStringLiteral(
                                   "event in-progress: receiver=%1(%2) event=%3(%4) elapsed=%5us")
                                   .arg(snapshot.receiverClass)
                                   .arg(snapshot.receiverName)
                                   .arg(QString::fromLatin1(snapshot.eventName))
                                   .arg(snapshot.eventType)
                                   .arg(elapsedUs));
            }
        });
    }

    std::atomic<bool> m_stopWatchdog{false};
    std::atomic<bool> m_watchdogStarted{false};
    std::thread m_watchdogThread;
    std::mutex m_activeEventMutex;
    ActiveEventState m_activeEvent;
};

}  // namespace

int main(int argc, char* argv[]) {
    BrecoApplication app(argc, argv);
    breco::MainWindow window;
    const QStringList args = app.arguments();
    if (args.size() >= 2) {
        window.selectSourcePath(args.at(1));
    }
    window.show();
    return app.exec();
}
