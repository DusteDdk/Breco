#pragma once

#include <QByteArray>
#include <QString>
#include <optional>

#include "model/ResultTypes.h"
#include "scan/ShiftTransform.h"

namespace breco {

class OpenFilePool;

struct LoadedRawWindow {
    ShiftReadPlan plan;
    QByteArray bytes;
};

class ShiftedWindowLoader {
public:
    explicit ShiftedWindowLoader(OpenFilePool* filePool);

    std::optional<LoadedRawWindow> loadRawWindow(const QString& filePath, quint64 fileSize,
                                                 quint64 outputStart, quint64 outputSize,
                                                 const ShiftSettings& shift) const;
    std::optional<QByteArray> loadTransformedWindow(const QString& filePath, quint64 fileSize,
                                                    quint64 outputStart, quint64 outputSize,
                                                    const ShiftSettings& shift) const;

private:
    OpenFilePool* m_filePool = nullptr;
};

}  // namespace breco
