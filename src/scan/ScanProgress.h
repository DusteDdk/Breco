#pragma once

#include <QString>
#include <QtGlobal>

#include <chrono>
#include <deque>

namespace breco {

struct ScanProgressSnapshot {
    quint64 scannedBytes = 0;
    quint64 totalBytes = 0;
    quint64 rawBytesRead = 0;
    double scanBytesPerSecond = 0.0;
    double rawBytesPerSecond = 0.0;
};

class ScanProgressTracker {
public:
    using Clock = std::chrono::steady_clock;

    void reset(quint64 scannedBytes = 0, quint64 rawBytesRead = 0,
               Clock::time_point now = Clock::now());
    ScanProgressSnapshot sample(quint64 scannedBytes, quint64 totalBytes,
                                quint64 rawBytesRead, Clock::time_point now = Clock::now());

private:
    struct Interval {
        quint64 scannedBytes = 0;
        quint64 rawBytes = 0;
        double seconds = 0.0;
    };

    Clock::time_point m_lastTime{};
    quint64 m_lastScanned = 0;
    quint64 m_lastRaw = 0;
    std::deque<Interval> m_intervals;
};

QString formatScanProgress(const ScanProgressSnapshot& progress);

}  // namespace breco
