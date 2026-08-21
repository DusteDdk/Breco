#include "io/OpenFilePool.h"

#include <QFile>
#include <QThread>

#include <cerrno>
#include <limits>

#ifdef Q_OS_UNIX
#include <unistd.h>
#endif

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

    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (m_externalReads.contains(filePath)) {
            return readExternalChunkLocked(filePath, offset, bytesToRead);
        }
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

bool OpenFilePool::registerExternalReadFd(const QString& filePath, int fd, quint64 fileSize) {
    if (filePath.isEmpty() || fd < 0 || fileSize == 0) {
        return false;
    }

    std::lock_guard<std::mutex> lock(m_mutex);
    if (auto it = m_externalReads.find(filePath); it != m_externalReads.end()) {
        closeExternalReadEntry(*it);
        m_externalReads.erase(it);
    }

    ExternalReadEntry entry;
    entry.fd = fd;
    entry.fileSize = fileSize;
    m_externalReads.insert(filePath, entry);
    return true;
}

void OpenFilePool::forgetExternalReadFd(const QString& filePath) {
    std::lock_guard<std::mutex> lock(m_mutex);
    auto it = m_externalReads.find(filePath);
    if (it == m_externalReads.end()) {
        return;
    }
    closeExternalReadEntry(*it);
    m_externalReads.erase(it);
}

void OpenFilePool::clearExternalReadFds() {
    std::lock_guard<std::mutex> lock(m_mutex);
    for (ExternalReadEntry& entry : m_externalReads) {
        closeExternalReadEntry(entry);
    }
    m_externalReads.clear();
}

bool OpenFilePool::hasExternalReadFd(const QString& filePath) const {
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_externalReads.contains(filePath);
}

std::optional<quint64> OpenFilePool::externalReadSize(const QString& filePath) const {
    std::lock_guard<std::mutex> lock(m_mutex);
    const auto it = m_externalReads.constFind(filePath);
    if (it == m_externalReads.constEnd()) {
        return std::nullopt;
    }
    return it->fileSize;
}

void OpenFilePool::closePath(const QString& filePath) {
    if (filePath.isEmpty()) {
        return;
    }
    std::lock_guard<std::mutex> lock(m_mutex);
    for (auto bucket = m_buckets.begin(); bucket != m_buckets.end(); ++bucket) {
        bucket->files.remove(filePath);
    }
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

std::optional<QByteArray> OpenFilePool::readExternalChunkLocked(const QString& filePath,
                                                               quint64 offset,
                                                               quint64 bytesToRead) const {
#ifdef Q_OS_UNIX
    const auto it = m_externalReads.constFind(filePath);
    if (it == m_externalReads.constEnd() || it->fd < 0) {
        return std::nullopt;
    }

    QByteArray bytes;
    bytes.resize(static_cast<qsizetype>(bytesToRead));
    ssize_t nread = 0;
    do {
        nread = ::pread(it->fd, bytes.data(), static_cast<size_t>(bytes.size()),
                        static_cast<off_t>(offset));
    } while (nread < 0 && errno == EINTR);
    if (nread < 0) {
        return std::nullopt;
    }
    bytes.resize(static_cast<qsizetype>(nread));
    return bytes;
#else
    Q_UNUSED(filePath);
    Q_UNUSED(offset);
    Q_UNUSED(bytesToRead);
    return std::nullopt;
#endif
}

void OpenFilePool::closeExternalReadEntry(ExternalReadEntry& entry) {
#ifdef Q_OS_UNIX
    if (entry.fd >= 0) {
        ::close(entry.fd);
        entry.fd = -1;
    }
#else
    Q_UNUSED(entry);
#endif
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
