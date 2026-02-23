#include "io/OpenFilePool.h"

#include <QFile>
#include <QThread>

#include <limits>

namespace breco {

OpenFilePool::OpenFilePool(int maxOpenFilesPerThread)
    : m_maxOpenFilesPerThread(qMax(1, maxOpenFilesPerThread)) {}

std::optional<QByteArray> OpenFilePool::readChunk(const QString& filePath, quint64 offset,
                                                  quint64 bytesToRead) const {
    if (bytesToRead == 0) {
        return QByteArray();
    }
    if (filePath.isEmpty()) {
        return std::nullopt;
    }
    if (offset > static_cast<quint64>(std::numeric_limits<qint64>::max())) {
        return std::nullopt;
    }
    if (bytesToRead > static_cast<quint64>(std::numeric_limits<qint64>::max())) {
        return std::nullopt;
    }

    const QSharedPointer<QFile> file = acquireFileForCurrentThread(filePath);
    if (file.isNull()) {
        return std::nullopt;
    }

    if (!file->seek(static_cast<qint64>(offset))) {
        return std::nullopt;
    }
    return file->read(static_cast<qint64>(bytesToRead));
}

void OpenFilePool::clearThreadLocal() {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_buckets.remove(currentThreadKey());
}

void OpenFilePool::clearAll() {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_buckets.clear();
}

QSharedPointer<QFile> OpenFilePool::acquireFileForCurrentThread(const QString& filePath) const {
    std::lock_guard<std::mutex> lock(m_mutex);
    ThreadBucket& bucket = m_buckets[currentThreadKey()];
    ++bucket.tick;

    auto it = bucket.files.find(filePath);
    if (it == bucket.files.end()) {
        trimBucketIfNeeded(bucket, filePath);

        FileEntry entry;
        entry.file = QSharedPointer<QFile>::create(filePath);
        if (!entry.file->open(QIODevice::ReadOnly)) {
            return {};
        }
        entry.lastUsedTick = bucket.tick;
        it = bucket.files.insert(filePath, entry);
    } else {
        it->lastUsedTick = bucket.tick;
        if (!it->file->isOpen() && !it->file->open(QIODevice::ReadOnly)) {
            bucket.files.erase(it);
            return {};
        }
    }

    return it->file;
}

void OpenFilePool::trimBucketIfNeeded(ThreadBucket& bucket, const QString& keepPath) const {
    while (bucket.files.size() >= m_maxOpenFilesPerThread) {
        auto lru = bucket.files.end();
        for (auto it = bucket.files.begin(); it != bucket.files.end(); ++it) {
            if (it.key() == keepPath) {
                continue;
            }
            if (lru == bucket.files.end() || it->lastUsedTick < lru->lastUsedTick) {
                lru = it;
            }
        }
        if (lru == bucket.files.end()) {
            break;
        }
        bucket.files.erase(lru);
    }
}

quintptr OpenFilePool::currentThreadKey() {
    return reinterpret_cast<quintptr>(QThread::currentThreadId());
}

}  // namespace breco
