#include "scan/ScanWorker.h"

#include <chrono>

#include "scan/MatchUtils.h"
#include "struct/StructVisualizer.h"

namespace breco {

ScanWorker::ScanWorker(int workerId, QByteArray searchTerm, TextInterpretationMode mode,
                       bool ignoreCase, std::atomic<quint64>* totalBytesScanned,
                       std::chrono::steady_clock::time_point scanStartTime,
                       JobCompleteCallback onJobComplete,
                       std::shared_ptr<const StructureGraph> structureGraph,
                       QString structureEntry,
                       std::shared_ptr<const QHash<QString, VisualizationSource>> externalSources)
    : m_workerId(workerId),
      m_totalBytesScanned(totalBytesScanned),
      m_searchTerm(std::move(searchTerm)),
      m_mode(mode),
      m_ignoreCase(ignoreCase),
      m_scanStartTime(scanStartTime),
      m_onJobComplete(std::move(onJobComplete)),
      m_structureGraph(std::move(structureGraph)),
      m_structureEntry(std::move(structureEntry)),
      m_externalSources(std::move(externalSources)) {}

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

        ScanJobResult result = processJob(job);

        m_busy.store(false, std::memory_order_release);
        if (m_onJobComplete != nullptr) {
            m_onJobComplete(m_workerId, std::move(result));
        }
    }
}

ScanJobResult ScanWorker::processJob(const ScanJob& job) {
    ScanJobResult result;
    result.sequence = job.sequence;
    result.bufferToken = job.bufferToken;
    const std::shared_ptr<ReadBuffer>& buffer = job.buffer;
    if (buffer == nullptr || job.size == 0 || job.reportLimit == 0 ||
        (m_searchTerm.isEmpty() && m_structureGraph == nullptr)) {
        return result;
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
        return result;
    }

    if (m_structureGraph != nullptr) {
        for (quint64 pos = 0; pos < job.reportLimit; ++pos) {
            if (m_stopRequested.load(std::memory_order_acquire)) {
                break;
            }
            ++result.bytesScanned;
            const quint64 absolute = job.fileOffset + pos;
            VisualizationSource primary{transformed, QString(), job.fileOffset};
            const VisualizedNode root = visualize(
                *m_structureGraph, m_structureEntry, primary,
                static_cast<size_t>(pos), 1,
                m_externalSources != nullptr
                    ? *m_externalSources
                    : QHash<QString, VisualizationSource>{});
            if (root.children.isEmpty()) {
                continue;
            }
            const VisualizedNode& candidate = root.children.first();
            if (!candidate.valid || candidate.bytesMissing != 0 ||
                !candidate.errorMessage.isEmpty() || candidate.sourceLength == 0) {
                continue;
            }
            MatchRecord match;
            match.scanTargetIdx = buffer->scanTargetIdx;
            match.threadId = m_workerId;
            match.offset = absolute;
            match.searchTimeNs = static_cast<quint64>(
                std::chrono::duration_cast<std::chrono::nanoseconds>(
                    std::chrono::steady_clock::now() - m_scanStartTime).count());
            result.matches.push_back(match);
        }
        if (m_totalBytesScanned != nullptr) {
            m_totalBytesScanned->fetch_add(result.bytesScanned, std::memory_order_relaxed);
        }
        return result;
    }

    int pos = 0;
    while (true) {
        if (m_stopRequested.load(std::memory_order_acquire)) {
            break;
        }
        pos = MatchUtils::indexOf(transformed, m_searchTerm, pos, m_mode, m_ignoreCase);
        if (pos < 0) {
            result.bytesScanned = job.reportLimit;
            break;
        }
        result.bytesScanned = qMin<quint64>(job.reportLimit, static_cast<quint64>(pos + 1));
        if (static_cast<quint32>(pos) < job.reportLimit) {
            MatchRecord match;
            match.scanTargetIdx = buffer->scanTargetIdx;
            match.threadId = m_workerId;
            match.offset = job.fileOffset + static_cast<quint64>(pos);
            match.searchTimeNs = static_cast<quint64>(
                std::chrono::duration_cast<std::chrono::nanoseconds>(
                    std::chrono::steady_clock::now() - m_scanStartTime)
                    .count());
            result.matches.push_back(match);
        }
        ++pos;
    }

    if (m_totalBytesScanned != nullptr) {
        m_totalBytesScanned->fetch_add(result.bytesScanned, std::memory_order_relaxed);
    }
    return result;
}

}  // namespace breco
