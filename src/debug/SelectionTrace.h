#pragma once

#include <chrono>
#include <iostream>

#include <QByteArray>
#include <QString>
#include <QThread>
#include <QtGlobal>

namespace breco::debug {

inline bool selectionTraceEnabled() {
    static const bool enabled = []() {
        bool ok = false;
        const int asInt = qEnvironmentVariableIntValue("BRECO_SELTRACE", &ok);
        if (ok) {
            return asInt != 0;
        }
        if (!qEnvironmentVariableIsSet("BRECO_SELTRACE")) {
            return false;
        }

        const QByteArray raw = qgetenv("BRECO_SELTRACE").trimmed().toLower();
        if (raw.isEmpty()) {
            return true;
        }
        return !(raw == "0" || raw == "false" || raw == "off" || raw == "no");
    }();
    return enabled;
}

inline quint64 selectionTraceElapsedUs() {
    static const auto kStart = std::chrono::steady_clock::now();
    const auto now = std::chrono::steady_clock::now();
    return static_cast<quint64>(
        std::chrono::duration_cast<std::chrono::microseconds>(now - kStart).count());
}

inline void selectionTraceLog(const QString& message) {
    if (!selectionTraceEnabled()) {
        return;
    }
    const quintptr threadId = reinterpret_cast<quintptr>(QThread::currentThreadId());
    std::cout << "[seltrace +" << selectionTraceElapsedUs() << "us t=0x"
              << std::hex << threadId << std::dec << "] " << message.toStdString()
              << std::endl;
}

inline void selectionTraceLog(const char* message) {
    if (!selectionTraceEnabled()) {
        return;
    }
    const quintptr threadId = reinterpret_cast<quintptr>(QThread::currentThreadId());
    std::cout << "[seltrace +" << selectionTraceElapsedUs() << "us t=0x"
              << std::hex << threadId << std::dec << "] " << message << std::endl;
}

}  // namespace breco::debug

#define BRECO_SELTRACE(MSG) ::breco::debug::selectionTraceLog(MSG)
