#include "scan/ScanProgress.h"

#include <algorithm>
#include <array>

namespace breco {

void ScanProgressTracker::reset(quint64 scannedBytes, quint64 rawBytesRead,
                                Clock::time_point now) {
    m_lastTime = now;
    m_lastScanned = scannedBytes;
    m_lastRaw = rawBytesRead;
    m_intervals.clear();
}

ScanProgressSnapshot ScanProgressTracker::sample(quint64 scannedBytes, quint64 totalBytes,
                                                  quint64 rawBytesRead, Clock::time_point now) {
    if (m_lastTime == Clock::time_point{}) {
        reset(scannedBytes, rawBytesRead, now);
    } else {
        const double seconds = std::chrono::duration<double>(now - m_lastTime).count();
        if (seconds > 0.0) {
            m_intervals.push_back(
                {scannedBytes >= m_lastScanned ? scannedBytes - m_lastScanned : 0,
                 rawBytesRead >= m_lastRaw ? rawBytesRead - m_lastRaw : 0, seconds});
            while (m_intervals.size() > 4) {
                m_intervals.pop_front();
            }
        }
        m_lastTime = now;
        m_lastScanned = scannedBytes;
        m_lastRaw = rawBytesRead;
    }

    quint64 scannedDelta = 0;
    quint64 rawDelta = 0;
    double elapsed = 0.0;
    for (const Interval& interval : m_intervals) {
        scannedDelta += interval.scannedBytes;
        rawDelta += interval.rawBytes;
        elapsed += interval.seconds;
    }
    ScanProgressSnapshot snapshot;
    snapshot.scannedBytes = std::min(scannedBytes, totalBytes);
    snapshot.totalBytes = totalBytes;
    snapshot.rawBytesRead = rawBytesRead;
    if (elapsed > 0.0) {
        snapshot.scanBytesPerSecond = static_cast<double>(scannedDelta) / elapsed;
        snapshot.rawBytesPerSecond = static_cast<double>(rawDelta) / elapsed;
    }
    return snapshot;
}

namespace {
QString binaryValue(double bytes) {
    static constexpr std::array<const char*, 4> units = {"KiB", "MiB", "GiB", "TiB"};
    double value = std::max(0.0, bytes) / 1024.0;
    int unit = 0;
    while (value >= 1024.0 && unit + 1 < static_cast<int>(units.size())) {
        value /= 1024.0;
        ++unit;
    }
    return QStringLiteral("%1 %2").arg(value, 0, 'f', 2).arg(QString::fromLatin1(units[unit]));
}
}  // namespace

QString formatScanProgress(const ScanProgressSnapshot& progress) {
    const quint64 scanned = std::min(progress.scannedBytes, progress.totalBytes);
    const double percent = progress.totalBytes == 0
                               ? 0.0
                               : std::clamp(100.0 * static_cast<double>(scanned) /
                                                static_cast<double>(progress.totalBytes),
                                            0.0, 100.0);
    return QStringLiteral("%1 / %2 @ %3/s ( Disk: %4/s )  - %5 %")
        .arg(binaryValue(static_cast<double>(scanned)))
        .arg(binaryValue(static_cast<double>(progress.totalBytes)))
        .arg(binaryValue(progress.scanBytesPerSecond))
        .arg(binaryValue(progress.rawBytesPerSecond))
        .arg(percent, 0, 'f', 2);
}

}  // namespace breco
