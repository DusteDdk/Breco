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
    void clearThreadLocal();
    void clearAll();

private:
    struct FileEntry {
        QSharedPointer<QFile> file;
        quint64 lastUsedTick = 0;
    };

    struct ThreadBucket {
        quint64 tick = 0;
        QHash<QString, FileEntry> files;
    };

    QSharedPointer<QFile> acquireFileForCurrentThread(const QString& filePath) const;
    void trimBucketIfNeeded(ThreadBucket& bucket, const QString& keepPath) const;
    static quintptr currentThreadKey();

    int m_maxOpenFilesPerThread = 32;
    mutable std::mutex m_mutex;
    mutable QHash<quintptr, ThreadBucket> m_buckets;
};

}  // namespace breco
