#pragma once

#include <QByteArray>
#include <QObject>
#include <QTimer>
#include <QVector>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <deque>
#include <mutex>
#include <memory>
#include <thread>
#include <unordered_map>

#include "model/ResultTypes.h"
#include "scan/ScanWorker.h"

namespace breco {

class OpenFilePool;
class ShiftedWindowLoader;

class ScanController : public QObject {
    Q_OBJECT

public:
    explicit ScanController(OpenFilePool* filePool = nullptr, QObject* parent = nullptr);
    ~ScanController() override;

    void startScan(const QVector<ScanTarget>& targets, const QByteArray& searchTerm, quint32 blockSize,
                   int workerCount, TextInterpretationMode mode, bool ignoreCase,
                   bool prefillOnMerge,
                   std::chrono::steady_clock::time_point scanButtonPressTime =
                       std::chrono::steady_clock::time_point{});
    void requestStop();
    bool isRunning() const;
    quint64 totalPlannedBytes() const;
    int fileCount() const;
    const QVector<ScanTarget>& scanTargets() const;
    const QVector<ResultBuffer>& resultBuffers() const;
    const QVector<int>& matchBufferIndices() const;
    quint32 searchTermLength() const;

signals:
    void scanStarted(int fileCount, quint64 totalBytes);
    void progressUpdated(quint64 scannedBytes, quint64 totalBytes);
    void resultsBatchReady(const QVector<MatchRecord>& matches, int mergedTotal);
    void scanFinished(bool stoppedByUser, bool autoStoppedLimitExceeded);
    void scanError(const QString& message);

private slots:
    void onTick();

private:
    void clearRuntimeState();
    void joinReaderAndWorkers();
    void readerLoop();
    bool dispatchJob(const ScanJob& job);
    void markJobTokenCompleted(quint64 bufferToken);
    void buildFinalResults();
    void buildResultBuffers();
    QByteArray loadRawWindow(int scanTargetIdx, quint64 start, quint64 size) const;
    quint64 fileSizeForTarget(int scanTargetIdx) const;
    void stopInternal(bool userStop);
    void emitProgress();

    QVector<ScanTarget> m_targets;
    QByteArray m_searchTerm;
    quint32 m_blockSize = 4096;
    TextInterpretationMode m_textMode = TextInterpretationMode::Ascii;
    bool m_ignoreCase = false;
    bool m_prefillOnMerge = true;
    std::chrono::steady_clock::time_point m_scanStartTime{};
    std::atomic<quint64> m_chunkCounter{0};
    std::atomic<quint64> m_totalScanned{0};
    std::atomic<bool> m_stopRequested{false};
    std::atomic<bool> m_readerDone{false};
    std::atomic<int> m_idleWorkerCount{0};

    int m_workerCount = 0;
    mutable std::mutex m_idleMutex;
    std::deque<int> m_idleWorkers;
    mutable std::mutex m_queuedJobsMutex;
    std::deque<ScanJob> m_queuedJobs;

    mutable std::mutex m_pendingMutex;
    std::condition_variable m_pendingCv;
    int m_pendingBufferCount = 0;
    mutable std::mutex m_trackerMutex;
    std::unordered_map<quint64, int> m_bufferJobsRemaining;
    std::atomic<quint64> m_nextBufferToken{1};

    std::vector<std::unique_ptr<ScanWorker>> m_workers;
    std::thread m_readerThread;

    QTimer m_tickTimer;
    bool m_running = false;
    bool m_userStopped = false;
    quint64 m_totalBytes = 0;
    int m_fileCount = 0;
    QVector<MatchRecord> m_finalMatches;
    QVector<ResultBuffer> m_resultBuffers;
    QVector<int> m_matchBufferIndices;
    OpenFilePool* m_filePool = nullptr;
    std::unique_ptr<OpenFilePool> m_ownedFilePool;
    std::unique_ptr<ShiftedWindowLoader> m_windowLoader;
};

}  // namespace breco
