#pragma once

#include <QByteArray>
#include <QHash>
#include <QSharedPointer>
#include <QString>
#include <QtGlobal>
#include <optional>
#include <mutex>

class QFile;

namespace breco {

class OpenFilePool {
public:
    explicit OpenFilePool(int maxOpenFilesPerThread = 32);

    std::optional<QByteArray> readChunk(const QString& filePath, quint64 offset,
                                        quint64 bytesToRead) const;
    bool registerExternalReadFd(const QString& filePath, int fd, quint64 fileSize);
    void forgetExternalReadFd(const QString& filePath);
    void clearExternalReadFds();
    bool hasExternalReadFd(const QString& filePath) const;
    std::optional<quint64> externalReadSize(const QString& filePath) const;
    void clearThreadLocal();
    void clearAll();

private:
    struct ExternalReadEntry {
        int fd = -1;
        quint64 fileSize = 0;
    };

    struct FileEntry {
        QSharedPointer<QFile> file;
        quint64 lastUsedTick = 0;
    };

    struct ThreadBucket {
        quint64 tick = 0;
        QHash<QString, FileEntry> files;
    };

    QSharedPointer<QFile> acquireFileForCurrentThread(const QString& filePath) const;
    std::optional<QByteArray> readExternalChunkLocked(const QString& filePath, quint64 offset,
                                                      quint64 bytesToRead) const;
    static void closeExternalReadEntry(ExternalReadEntry& entry);
    void trimBucketIfNeeded(ThreadBucket& bucket, const QString& keepPath) const;
    static quintptr currentThreadKey();

    int m_maxOpenFilesPerThread = 32;
    mutable std::mutex m_mutex;
    mutable QHash<quintptr, ThreadBucket> m_buckets;
    mutable QHash<QString, ExternalReadEntry> m_externalReads;
};

}  // namespace breco
