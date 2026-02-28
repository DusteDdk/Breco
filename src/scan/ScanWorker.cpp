#include "scan/ScanWorker.h"

#include <chrono>

#include "scan/MatchUtils.h"

namespace breco {

ScanWorker::ScanWorker(int workerId, QByteArray searchTerm, TextInterpretationMode mode,
                       bool ignoreCase, std::atomic<quint64>* totalBytesScanned,
                       std::chrono::steady_clock::time_point scanStartTime,
                       JobCompleteCallback onJobComplete)
    : m_workerId(workerId),
      m_totalBytesScanned(totalBytesScanned),
      m_searchTerm(std::move(searchTerm)),
      m_mode(mode),
      m_ignoreCase(ignoreCase),
      m_scanStartTime(scanStartTime),
      m_onJobComplete(std::move(onJobComplete)) {}

ScanWorker::~ScanWorker() {
    requestStop();
    wakeForStop();
    join();
}

void ScanWorker::start() { m_thread = std::thread([this]() { runLoop(); }); }

void ScanWorker::join() {
    if (m_thread.joinable()) {
        m_thread.join();
    }
}

void ScanWorker::assignJob(const ScanJob& job) {
    {
        std::lock_guard<std::mutex> lock(m_jobMutex);
        m_pendingJob = job;
        m_hasPendingJob = true;
        m_busy.store(true, std::memory_order_release);
    }
    m_workProvided.release();
}

void ScanWorker::requestStop() { m_stopRequested.store(true, std::memory_order_release); }

void ScanWorker::wakeForStop() { m_workProvided.release(); }

bool ScanWorker::isBusy() const { return m_busy.load(std::memory_order_acquire); }

const QVector<MatchRecord>& ScanWorker::matches() const { return m_matches; }

void ScanWorker::runLoop() {
    for (;;) {
        m_workProvided.acquire();

        ScanJob job;
        bool hasJob = false;
        {
            std::lock_guard<std::mutex> lock(m_jobMutex);
            if (m_hasPendingJob) {
                job = m_pendingJob;
                m_hasPendingJob = false;
                hasJob = true;
            }
        }

        if (!hasJob) {
            if (m_stopRequested.load(std::memory_order_acquire)) {
                return;
            }
            continue;
        }

        processJob(job);

        m_busy.store(false, std::memory_order_release);
        if (m_onJobComplete != nullptr) {
            m_onJobComplete(m_workerId, job.bufferToken);
        }
    }
}

void ScanWorker::processJob(const ScanJob& job) {
    const std::shared_ptr<ReadBuffer>& buffer = job.buffer;
    if (buffer == nullptr || job.size == 0 || job.reportLimit == 0 || m_searchTerm.isEmpty()) {
        if (m_totalBytesScanned != nullptr) {
            m_totalBytesScanned->fetch_add(job.reportLimit, std::memory_order_relaxed);
        }
        return;
    }

    QByteArray transformed;
    const qint64 localStart =
        static_cast<qint64>(job.fileOffset) - static_cast<qint64>(buffer->rawStart);
    const qint64 localEnd = localStart + static_cast<qint64>(job.size);
    if (localStart >= 0 && localEnd >= localStart && localEnd <= buffer->rawBytes.size()) {
        transformed = QByteArray::fromRawData(
            buffer->rawBytes.constData() + static_cast<int>(localStart),
            static_cast<int>(job.size));
    } else {
        if (m_totalBytesScanned != nullptr) {
            m_totalBytesScanned->fetch_add(job.reportLimit, std::memory_order_relaxed);
        }
        return;
    }

    int pos = 0;
    while (true) {
        pos = MatchUtils::indexOf(transformed, m_searchTerm, pos, m_mode, m_ignoreCase);
        if (pos < 0) {
            break;
        }
        if (static_cast<quint32>(pos) < job.reportLimit) {
            MatchRecord match;
            match.scanTargetIdx = buffer->scanTargetIdx;
            match.threadId = m_workerId;
            match.offset = job.fileOffset + static_cast<quint64>(pos);
            match.searchTimeNs = static_cast<quint64>(
                std::chrono::duration_cast<std::chrono::nanoseconds>(
                    std::chrono::steady_clock::now() - m_scanStartTime)
                    .count());
            m_matches.push_back(match);
        }
        ++pos;
    }

    if (m_totalBytesScanned != nullptr) {
        m_totalBytesScanned->fetch_add(job.reportLimit, std::memory_order_relaxed);
    }
}

}  // namespace breco
