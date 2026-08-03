#include "scan/ScanController.h"

#include <algorithm>
#include <iostream>
#include <limits>
#include <utility>

#include <QThread>

#include "io/OpenFilePool.h"
#include "io/ShiftedWindowLoader.h"

namespace breco {

namespace {
constexpr quint64 kMergeGapBytes = 16ULL * 1024ULL * 1024ULL;
constexpr quint64 kResultPaddingBytes = 8ULL * 1024ULL * 1024ULL;
constexpr quint64 kMaxResultBufferBytes = 128ULL * 1024ULL * 1024ULL;
constexpr auto kUiPublishInterval = std::chrono::seconds(2);
}

ScanController::ScanController(OpenFilePool* filePool, QObject* parent) : QObject(parent) {
    if (filePool != nullptr) {
        m_filePool = filePool;
    } else {
        m_ownedFilePool = std::make_unique<OpenFilePool>();
        m_filePool = m_ownedFilePool.get();
    }
    m_windowLoader = std::make_unique<ShiftedWindowLoader>(m_filePool);
    m_tickTimer.setInterval(100);
    connect(&m_tickTimer, &QTimer::timeout, this, &ScanController::onTick);
}

ScanController::~ScanController() {
    requestStop();
    joinReaderAndWorkers();
}

void ScanController::startScan(const QVector<ScanTarget>& targets, const QByteArray& searchTerm,
                               quint32 blockSize, int workerCount, TextInterpretationMode mode,
                               bool ignoreCase, bool prefillOnMerge,
                               std::chrono::steady_clock::time_point scanButtonPressTime,
                               std::shared_ptr<const StructureGraph> structureGraph,
                               QString structureEntry,
                               std::shared_ptr<const QHash<QString, VisualizationSource>> externalSources) {
    if (m_running) {
        emit scanError(QStringLiteral("Scan already running"));
        return;
    }
    if (searchTerm.isEmpty() && structureGraph == nullptr) {
        emit scanError(QStringLiteral("Search term must not be empty"));
        return;
    }

    clearRuntimeState();

    m_targets.clear();
    m_totalBytes = 0;
    for (const ScanTarget& target : targets) {
        if (target.filePath.isEmpty() || target.fileSize == 0) {
            continue;
        }
        m_targets.push_back(target);
        m_totalBytes += target.fileSize;
    }
    m_fileCount = m_targets.size();
    if (m_targets.isEmpty()) {
        emit scanError(QStringLiteral("No readable files to scan"));
        return;
    }

    m_searchTerm = searchTerm;
    m_structureGraph = std::move(structureGraph);
    m_structureEntry = std::move(structureEntry);
    m_externalSources = std::move(externalSources);
    m_matchLength = 1;
    if (m_structureGraph != nullptr) {
        m_matchLength = static_cast<quint32>(qMax(
            1, m_structureGraph->staticStructLayoutSizeBytes(m_structureEntry).value_or(1)));
    }
    m_blockSize = qMax<quint32>(1, blockSize);
    m_textMode = mode;
    m_ignoreCase = ignoreCase;
    m_prefillOnMerge = prefillOnMerge;
    m_totalScanned.store(0, std::memory_order_release);
    m_totalRawBytesRead.store(0, std::memory_order_release);
    m_stopRequested.store(false, std::memory_order_release);
    m_readerDone.store(false, std::memory_order_release);
    m_userStopped = false;

    if (workerCount <= 0) {
        workerCount = qMax(1, QThread::idealThreadCount());
    }
    m_workerCount = qMax(1, workerCount);
    m_scanStartTime = scanButtonPressTime;
    if (m_scanStartTime == std::chrono::steady_clock::time_point{}) {
        m_scanStartTime = std::chrono::steady_clock::now();
    }
    m_chunkCounter.store(0, std::memory_order_release);

    m_idleWorkers.clear();
    for (int i = 0; i < m_workerCount; ++i) {
        m_idleWorkers.push_back(i);
    }
    m_idleWorkerCount.store(m_workerCount, std::memory_order_release);

    auto onJobComplete = [this](int workerId, ScanJobResult result) {
        const quint64 bufferToken = result.bufferToken;
        {
            std::lock_guard<std::mutex> completedLock(m_completedJobsMutex);
            m_completedJobs.emplace(result.sequence, std::move(result));
        }
        markJobTokenCompleted(bufferToken);
        m_pendingCv.notify_all();

        ScanJob queuedJob;
        bool hasQueuedJob = false;
        {
            std::lock_guard<std::mutex> queuedLock(m_queuedJobsMutex);
            if (!m_stopRequested.load(std::memory_order_acquire) && !m_queuedJobs.empty()) {
                queuedJob = m_queuedJobs.front();
                m_queuedJobs.pop_front();
                hasQueuedJob = true;
            }
        }

        if (hasQueuedJob && workerId >= 0 && workerId < static_cast<int>(m_workers.size())) {
            m_workers[workerId]->assignJob(queuedJob);
            return;
        }
        if (hasQueuedJob) {
            std::lock_guard<std::mutex> queuedLock(m_queuedJobsMutex);
            m_queuedJobs.push_front(queuedJob);
        }
        if (workerId < 0 || workerId >= static_cast<int>(m_workers.size())) {
            std::cerr << "[scan][warn] invalid worker id in completion callback: " << workerId
                      << std::endl;
            m_pendingCv.notify_all();
            return;
        }

        {
            std::lock_guard<std::mutex> idleLock(m_idleMutex);
            m_idleWorkers.push_back(workerId);
            m_idleWorkerCount.fetch_add(1, std::memory_order_acq_rel);
        }
        m_pendingCv.notify_all();
    };

    m_workers.reserve(m_workerCount);
    for (int i = 0; i < m_workerCount; ++i) {
        m_workers.push_back(std::make_unique<ScanWorker>(i, m_searchTerm, m_textMode, m_ignoreCase,
                                                         &m_totalScanned,
                                                         m_scanStartTime,
                                                         onJobComplete, m_structureGraph,
                                                         m_structureEntry,
                                                         m_externalSources));
    }
    for (const auto& worker : m_workers) {
        worker->start();
    }

    m_readerThread = std::thread([this]() { readerLoop(); });

    m_running = true;
    const auto now = std::chrono::steady_clock::now();
    m_progressTracker.reset(0, 0, now);
    m_lastUiPublish = now;
    m_tickTimer.start();
    emit scanStarted(m_fileCount, m_totalBytes);
    logLifecycle(QStringLiteral("[scan] started: files=%1 totalBytes=%2 workers=%3 blockSize=%4 "
                                "prefillOnMerge=%5")
                     .arg(m_fileCount)
                     .arg(m_totalBytes)
                     .arg(m_workerCount)
                     .arg(m_blockSize)
                     .arg(m_prefillOnMerge ? QStringLiteral("true") : QStringLiteral("false")));
    emit progressUpdated(ScanProgressSnapshot{0, m_totalBytes, 0, 0.0, 0.0});
}

void ScanController::requestStop() {
    if (!m_running) {
        return;
    }
    stopInternal(true);
}

bool ScanController::isRunning() const { return m_running; }

quint64 ScanController::totalPlannedBytes() const { return m_totalBytes; }

int ScanController::fileCount() const { return m_fileCount; }

const QVector<ScanTarget>& ScanController::scanTargets() const { return m_targets; }

const QVector<ResultBuffer>& ScanController::resultBuffers() const { return m_resultBuffers; }

const QVector<int>& ScanController::matchBufferIndices() const { return m_matchBufferIndices; }

quint32 ScanController::searchTermLength() const {
    return m_structureGraph != nullptr ? m_matchLength
                                       : static_cast<quint32>(qMax(1, m_searchTerm.size()));
}

void ScanController::onTick() {
    if (!m_running) {
        return;
    }

    const auto now = std::chrono::steady_clock::now();
    if (now - m_lastUiPublish >= kUiPublishInterval) {
        publishCompletedResults(false);
        emitProgress();
        m_lastUiPublish = now;
    }

    if (!m_readerDone.load(std::memory_order_acquire)) {
        return;
    }

    m_tickTimer.stop();
    joinReaderAndWorkers();
    publishCompletedResults(true);
    emitProgress();
    logLifecycle(QStringLiteral("[scan] merging started: results=%1").arg(m_finalMatches.size()));
    QTimer::singleShot(1, this, &ScanController::finalizeScan);
}

void ScanController::finalizeScan() {
    if (!m_running) {
        return;
    }
    buildFinalResults();
    logLifecycle(QStringLiteral("[scan] merging finished: matches=%1 buffers=%2")
                     .arg(m_finalMatches.size())
                     .arg(m_resultBuffers.size()));

    m_running = false;
    emit resultsBatchReady({}, m_finalMatches.size());
    logLifecycle(QStringLiteral("[scan] finished: stoppedByUser=%1 scannedBytes=%2 totalBytes=%3")
                     .arg(m_userStopped ? QStringLiteral("true") : QStringLiteral("false"))
                     .arg(m_totalScanned.load(std::memory_order_relaxed))
                     .arg(m_totalBytes));
    emit scanFinished(m_userStopped, false);
}

void ScanController::clearRuntimeState() {
    m_tickTimer.stop();
    joinReaderAndWorkers();

    m_targets.clear();
    m_workers.clear();
    m_idleWorkers.clear();
    {
        std::lock_guard<std::mutex> lock(m_queuedJobsMutex);
        m_queuedJobs.clear();
    }
    m_idleWorkerCount.store(0, std::memory_order_release);

    {
        std::lock_guard<std::mutex> lock(m_pendingMutex);
        m_pendingBufferCount = 0;
    }

    m_finalMatches.clear();
    m_resultBuffers.clear();
    m_matchBufferIndices.clear();
    {
        std::lock_guard<std::mutex> lock(m_trackerMutex);
        m_bufferJobsRemaining.clear();
    }

    m_totalBytes = 0;
    m_fileCount = 0;
    m_workerCount = 0;
    m_running = false;
    m_userStopped = false;
    m_stopRequested.store(false, std::memory_order_release);
    m_readerDone.store(false, std::memory_order_release);
    m_totalScanned.store(0, std::memory_order_release);
    m_totalRawBytesRead.store(0, std::memory_order_release);
    m_nextBufferToken.store(1, std::memory_order_release);
    m_nextJobSequence.store(0, std::memory_order_release);
    {
        std::lock_guard<std::mutex> lock(m_completedJobsMutex);
        m_completedJobs.clear();
    }
    m_nextPublishSequence = 0;
    m_chunkCounter.store(0, std::memory_order_release);
    m_scanStartTime = std::chrono::steady_clock::time_point{};
    if (m_filePool != nullptr) {
        m_filePool->clearAll();
    }
}

void ScanController::joinReaderAndWorkers() {
    if (m_readerThread.joinable()) {
        m_readerThread.join();
    }

    for (const auto& worker : m_workers) {
        worker->requestStop();
    }
    for (const auto& worker : m_workers) {
        worker->wakeForStop();
    }
    for (const auto& worker : m_workers) {
        worker->join();
    }
}

void ScanController::readerLoop() {
    const quint32 overlap = m_structureGraph != nullptr
                                ? (m_structureGraph->staticStructLayoutSizeBytes(m_structureEntry)
                                           .has_value()
                                       ? m_matchLength - 1
                                       : 64U * 1024U)
                                : static_cast<quint32>(m_searchTerm.size() > 0
                                                          ? m_searchTerm.size() - 1 : 0);
    const int maxPendingBuffers = qMax(1, m_workerCount * 2);

    for (int targetIdx = 0; targetIdx < m_targets.size(); ++targetIdx) {
        if (m_stopRequested.load(std::memory_order_acquire)) {
            break;
        }

        const ScanTarget& target = m_targets.at(targetIdx);
        if (target.filePath.isEmpty() || target.fileSize == 0) {
            continue;
        }

        quint64 fileOffset = 0;
        while (fileOffset < target.fileSize) {
            {
                std::unique_lock<std::mutex> lock(m_pendingMutex);
                m_pendingCv.wait(lock, [this, maxPendingBuffers]() {
                    return m_stopRequested.load(std::memory_order_acquire) ||
                           m_pendingBufferCount < maxPendingBuffers;
                });
            }

            if (m_stopRequested.load(std::memory_order_acquire)) {
                break;
            }

            const quint64 primarySize = qMin<quint64>(m_blockSize, target.fileSize - fileOffset);
            quint64 outputSize = primarySize;
            if (fileOffset + primarySize < target.fileSize) {
                outputSize += overlap;
            }

            const quint64 chunkId = m_chunkCounter.fetch_add(1, std::memory_order_acq_rel) + 1;

            auto rawWindow = m_windowLoader->loadRawWindow(
                target.filePath, target.fileSize, fileOffset, outputSize, ShiftSettings{});
            if (!rawWindow.has_value()) {
                std::cerr << "[scan][warn] read failed: targetIdx=" << targetIdx
                          << " offset=" << fileOffset
                          << " outputSize=" << outputSize << std::endl;
                break;
            }
            m_totalRawBytesRead.fetch_add(static_cast<quint64>(rawWindow->bytes.size()),
                                         std::memory_order_relaxed);
            if (m_stopRequested.load(std::memory_order_acquire)) {
                break;
            }

            auto buffer = std::make_shared<ReadBuffer>();
            buffer->scanTargetIdx = targetIdx;
            buffer->fileSize = target.fileSize;
            buffer->outputStart = fileOffset;
            buffer->outputSize = outputSize;
            buffer->rawStart = rawWindow->plan.readStart;
            buffer->rawBytes = std::move(rawWindow->bytes);
            const quint64 bufferToken = m_nextBufferToken.fetch_add(1, std::memory_order_acq_rel);

            const int jobTargetCount = qMax(1, m_workerCount * 2);
            const quint64 baseChunk = primarySize / static_cast<quint64>(jobTargetCount);
            const quint64 remainder = primarySize % static_cast<quint64>(jobTargetCount);

            QVector<ScanJob> jobs;
            jobs.reserve(jobTargetCount);
            quint64 localPrimaryOffset = 0;
            for (int i = 0; i < jobTargetCount; ++i) {
                quint64 jobPrimary = baseChunk;
                if (static_cast<quint64>(i) < remainder) {
                    ++jobPrimary;
                }
                if (jobPrimary == 0) {
                    continue;
                }

                const quint64 jobStart = localPrimaryOffset;
                const quint64 desiredEnd = jobStart + jobPrimary + overlap;
                const quint64 actualEnd = qMin(desiredEnd, outputSize);
                const quint64 jobSize64 = (actualEnd > jobStart) ? (actualEnd - jobStart) : 0;

                ScanJob job;
                job.buffer = buffer;
                job.bufferToken = bufferToken;
                job.fileOffset = fileOffset + jobStart;
                job.offset = jobStart;
                job.size = static_cast<quint32>(
                    qMin<quint64>(jobSize64, std::numeric_limits<quint32>::max()));
                job.reportLimit = static_cast<quint32>(
                    qMin<quint64>(jobPrimary, std::numeric_limits<quint32>::max()));
                job.sequence = m_nextJobSequence.fetch_add(1, std::memory_order_acq_rel);
                if (job.size > 0 && job.reportLimit > 0) {
                    jobs.push_back(job);
                }

                localPrimaryOffset += jobPrimary;
            }

            bool partitionsValid = true;
            quint64 expectedOffset = 0;
            for (int i = 0; i < jobs.size(); ++i) {
                const ScanJob& job = jobs.at(i);
                if (job.offset != expectedOffset) {
                    partitionsValid = false;
                    break;
                }
                if (job.size < job.reportLimit) {
                    partitionsValid = false;
                    break;
                }
                const quint64 trailingOverlap =
                    static_cast<quint64>(job.size - job.reportLimit);
                if (trailingOverlap > overlap) {
                    partitionsValid = false;
                    break;
                }
                expectedOffset += job.reportLimit;
            }
            if (expectedOffset != primarySize) {
                partitionsValid = false;
            }
            if (!partitionsValid) {
                std::cerr << "[scan][warn] invalid job partitioning for chunk " << chunkId
                          << " primarySize=" << primarySize << " jobs=" << jobs.size()
                          << " overlap=" << overlap << std::endl;
            }

            if (m_stopRequested.load(std::memory_order_acquire)) {
                for (const ScanJob& job : jobs) {
                    ScanJobResult result;
                    result.sequence = job.sequence;
                    result.bufferToken = job.bufferToken;
                    std::lock_guard<std::mutex> lock(m_completedJobsMutex);
                    m_completedJobs.emplace(result.sequence, std::move(result));
                }
                break;
            }

            if (!jobs.isEmpty()) {
                {
                    std::lock_guard<std::mutex> trackerLock(m_trackerMutex);
                    m_bufferJobsRemaining[bufferToken] = jobs.size();
                }
                {
                    std::lock_guard<std::mutex> lock(m_pendingMutex);
                    ++m_pendingBufferCount;
                }

                for (const ScanJob& job : jobs) {
                    if (!dispatchJob(job)) {
                        std::lock_guard<std::mutex> queuedLock(m_queuedJobsMutex);
                        m_queuedJobs.push_back(job);
                    }
                }
            }

            fileOffset += primarySize;
        }
    }

    {
        std::unique_lock<std::mutex> lock(m_pendingMutex);
        m_pendingCv.wait(lock, [this]() { return m_pendingBufferCount == 0; });
    }

    for (const auto& worker : m_workers) {
        worker->requestStop();
    }
    for (const auto& worker : m_workers) {
        worker->wakeForStop();
    }

    m_readerDone.store(true, std::memory_order_release);
    m_pendingCv.notify_all();
    if (m_filePool != nullptr) {
        m_filePool->clearThreadLocal();
    }
}

bool ScanController::dispatchJob(const ScanJob& job) {
    if (m_workers.empty()) {
        return false;
    }

    int workerId = -1;
    {
        std::lock_guard<std::mutex> lock(m_idleMutex);
        if (!m_idleWorkers.empty()) {
            workerId = m_idleWorkers.front();
            m_idleWorkers.pop_front();
        }
    }

    if (workerId < 0 || workerId >= m_workers.size()) {
        return false;
    }

    m_idleWorkerCount.fetch_sub(1, std::memory_order_acq_rel);
    m_workers[workerId]->assignJob(job);
    return true;
}

void ScanController::markJobTokenCompleted(quint64 bufferToken) {
    bool bufferDone = false;
    {
        std::lock_guard<std::mutex> lock(m_trackerMutex);
        auto it = m_bufferJobsRemaining.find(bufferToken);
        if (it != m_bufferJobsRemaining.end()) {
            --(it->second);
            if (it->second <= 0) {
                m_bufferJobsRemaining.erase(it);
                bufferDone = true;
            }
        }
    }
    if (!bufferDone) {
        return;
    }

    {
        std::lock_guard<std::mutex> lock(m_pendingMutex);
        if (m_pendingBufferCount > 0) {
            --m_pendingBufferCount;
        }
    }
    m_pendingCv.notify_all();
}

void ScanController::buildFinalResults() {
    buildResultBuffers();
}

void ScanController::buildResultBuffers() {
    m_resultBuffers.clear();
    m_matchBufferIndices.fill(-1, m_finalMatches.size());
    if (m_finalMatches.isEmpty()) {
        return;
    }

    if (!m_prefillOnMerge) {
        m_resultBuffers.reserve(m_finalMatches.size());
        for (int i = 0; i < m_finalMatches.size(); ++i) {
            const MatchRecord& match = m_finalMatches.at(i);
            ResultBuffer resultBuffer;
            resultBuffer.scanTargetIdx = match.scanTargetIdx;
            resultBuffer.fileOffset = match.offset;
            resultBuffer.bytes.clear();
            resultBuffer.dirty = false;
            const int bufferIndex = m_resultBuffers.size();
            m_resultBuffers.push_back(std::move(resultBuffer));
            m_matchBufferIndices[i] = bufferIndex;
        }
        std::cout << "[scan] merge mode: prefill disabled, created zero-length buffers per result="
                  << m_resultBuffers.size() << std::endl;
        return;
    }

    const quint64 termLen = static_cast<quint64>(searchTermLength());

    int startIdx = 0;
    while (startIdx < m_finalMatches.size()) {
        const int targetIdx = m_finalMatches.at(startIdx).scanTargetIdx;
        const quint64 targetSize = fileSizeForTarget(targetIdx);
        if (targetIdx < 0 || targetSize == 0) {
            ++startIdx;
            continue;
        }

        int endIdx = startIdx + 1;
        quint64 clusterFirst = m_finalMatches.at(startIdx).offset;
        quint64 clusterLast = m_finalMatches.at(startIdx).offset;

        while (endIdx < m_finalMatches.size() && m_finalMatches.at(endIdx).scanTargetIdx == targetIdx) {
            const quint64 nextOffset = m_finalMatches.at(endIdx).offset;
            const bool nearEnough = nextOffset <= (clusterLast + kMergeGapBytes);

            const quint64 rangeStart =
                (clusterFirst > kResultPaddingBytes) ? (clusterFirst - kResultPaddingBytes) : 0;
            const quint64 rangeEnd =
                qMin(targetSize, nextOffset + termLen + kResultPaddingBytes);
            const quint64 rangeSize = (rangeEnd > rangeStart) ? (rangeEnd - rangeStart) : 0;
            const bool fitsMax = rangeSize <= kMaxResultBufferBytes;

            if (!nearEnough || !fitsMax) {
                break;
            }

            clusterLast = nextOffset;
            ++endIdx;
        }

        const quint64 bufferStart =
            (clusterFirst > kResultPaddingBytes) ? (clusterFirst - kResultPaddingBytes) : 0;
        quint64 bufferEnd = qMin(targetSize, clusterLast + termLen + kResultPaddingBytes);
        if (bufferEnd < bufferStart) {
            bufferEnd = bufferStart;
        }
        quint64 bufferSize = bufferEnd - bufferStart;
        if (bufferSize > kMaxResultBufferBytes) {
            bufferSize = kMaxResultBufferBytes;
        }

        const int bufferIndex = m_resultBuffers.size();
        std::cout << "[scan] merge prefill start: buffer#" << bufferIndex
                  << " targetIdx=" << targetIdx
                  << " fileOffset=" << bufferStart
                  << " requestedSize=" << bufferSize
                  << " matchCount=" << (endIdx - startIdx) << std::endl;

        ResultBuffer resultBuffer;
        resultBuffer.scanTargetIdx = targetIdx;
        resultBuffer.fileOffset = bufferStart;
        resultBuffer.bytes = loadRawWindow(targetIdx, bufferStart, bufferSize);
        resultBuffer.dirty = false;
        std::cout << "[scan] merge prefill done: buffer#" << bufferIndex
                  << " loadedSize=" << resultBuffer.bytes.size() << std::endl;

        m_resultBuffers.push_back(resultBuffer);
        for (int i = startIdx; i < endIdx; ++i) {
            m_matchBufferIndices[i] = bufferIndex;
        }

        startIdx = endIdx;
    }
}

QByteArray ScanController::loadRawWindow(int scanTargetIdx, quint64 start, quint64 size) const {
    if (scanTargetIdx < 0 || scanTargetIdx >= m_targets.size() || size == 0) {
        return {};
    }

    const ScanTarget& target = m_targets.at(scanTargetIdx);
    if (target.filePath.isEmpty() || target.fileSize == 0) {
        return {};
    }

    if (m_windowLoader == nullptr) {
        return {};
    }
    const auto rawWindow =
        m_windowLoader->loadRawWindow(target.filePath, target.fileSize, start, size, ShiftSettings{});
    if (!rawWindow.has_value()) {
        return {};
    }
    return rawWindow->bytes;
}

quint64 ScanController::fileSizeForTarget(int scanTargetIdx) const {
    if (scanTargetIdx < 0 || scanTargetIdx >= m_targets.size()) {
        return 0;
    }
    return m_targets.at(scanTargetIdx).fileSize;
}

void ScanController::stopInternal(bool userStop) {
    m_stopRequested.store(true, std::memory_order_release);
    m_userStopped = m_userStopped || userStop;
    cancelQueuedJobs();
    for (const auto& worker : m_workers) {
        worker->requestStop();
        worker->wakeForStop();
    }
    m_pendingCv.notify_all();
}

void ScanController::emitProgress() {
    emit progressUpdated(m_progressTracker.sample(
        m_totalScanned.load(std::memory_order_relaxed), m_totalBytes,
        m_totalRawBytesRead.load(std::memory_order_relaxed)));
}

void ScanController::publishCompletedResults(bool) {
    QVector<MatchRecord> batch;
    {
        std::lock_guard<std::mutex> lock(m_completedJobsMutex);
        for (;;) {
            auto it = m_completedJobs.find(m_nextPublishSequence);
            if (it == m_completedJobs.end()) {
                break;
            }
            batch += it->second.matches;
            m_completedJobs.erase(it);
            ++m_nextPublishSequence;
        }
    }
    if (batch.isEmpty()) {
        return;
    }
    for (const MatchRecord& match : batch) {
        m_finalMatches.push_back(match);
        ResultBuffer buffer;
        buffer.scanTargetIdx = match.scanTargetIdx;
        buffer.fileOffset = match.offset;
        m_matchBufferIndices.push_back(m_resultBuffers.size());
        m_resultBuffers.push_back(std::move(buffer));
    }
    emit resultsBatchReady(batch, m_finalMatches.size());
    logLifecycle(QStringLiteral("[scan] results found: %1").arg(m_finalMatches.size()));
}

void ScanController::cancelQueuedJobs() {
    std::deque<ScanJob> cancelled;
    {
        std::lock_guard<std::mutex> lock(m_queuedJobsMutex);
        cancelled.swap(m_queuedJobs);
    }
    for (const ScanJob& job : cancelled) {
        ScanJobResult result;
        result.sequence = job.sequence;
        result.bufferToken = job.bufferToken;
        {
            std::lock_guard<std::mutex> lock(m_completedJobsMutex);
            m_completedJobs.emplace(result.sequence, std::move(result));
        }
        markJobTokenCompleted(job.bufferToken);
    }
    m_pendingCv.notify_all();
}

void ScanController::logLifecycle(const QString& message) {
    std::cout << message.toStdString() << std::endl;
    emit lifecycleMessage(message);
}

}  // namespace breco
