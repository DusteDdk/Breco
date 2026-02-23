#pragma once

#include <QByteArray>
#include <QtGlobal>
#include <memory>

namespace breco {

struct ReadBuffer {
    int scanTargetIdx = -1;
    quint64 fileSize = 0;
    quint64 outputStart = 0;
    quint64 outputSize = 0;
    quint64 rawStart = 0;
    QByteArray rawBytes;
};

struct ScanJob {
    std::shared_ptr<ReadBuffer> buffer;
    quint64 bufferToken = 0;
    quint64 fileOffset = 0;
    quint64 offset = 0;
    quint32 size = 0;
    quint32 reportLimit = 0;
};

}  // namespace breco
