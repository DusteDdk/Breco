#pragma once

#include <QByteArray>
#include <QVector>
#include <atomic>
#include <chrono>
#include <functional>
#include <memory>
#include <mutex>
#include <semaphore>
#include <thread>

#include "model/ResultTypes.h"
#include "brecolang/runtime/ByteSource.h"
#include "brecolang/runtime/ProbeScan.h"
#include "scan/ScanTypes.h"

namespace breco {

class ScanWorker {
public:
    using JobCompleteCallback = std::function<void(int workerId, ScanJobResult result)>;

    ScanWorker(int workerId, QByteArray searchTerm, TextInterpretationMode mode, bool ignoreCase,
               std::atomic<quint64>* totalBytesScanned,
               std::chrono::steady_clock::time_point scanStartTime,
               JobCompleteCallback onJobComplete,
               std::shared_ptr<const lang::ProbeScanPlan> probePlan = {});

    ~ScanWorker();

    void start();
    void join();
    void assignJob(const ScanJob& job);
    void requestStop();
    void wakeForStop();
    bool isBusy() const;

private:
    void runLoop();
    ScanJobResult processJob(const ScanJob& job);

    int m_workerId = 0;
    std::atomic<bool> m_stopRequested{false};
    std::atomic<bool> m_busy{false};
    std::atomic<quint64>* m_totalBytesScanned = nullptr;
    QByteArray m_searchTerm;
    TextInterpretationMode m_mode = TextInterpretationMode::Ascii;
    bool m_ignoreCase = false;
    std::chrono::steady_clock::time_point m_scanStartTime{};
    JobCompleteCallback m_onJobComplete;

    std::binary_semaphore m_workProvided{0};
    mutable std::mutex m_jobMutex;
    ScanJob m_pendingJob;
    bool m_hasPendingJob = false;
    std::shared_ptr<const lang::ProbeScanPlan> m_probePlan;
    QString m_probeTargetPath;
    QVector<std::shared_ptr<lang::ByteSource>> m_probeInputs;
    std::thread m_thread;
};

}  // namespace breco
