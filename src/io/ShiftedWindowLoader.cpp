#include "io/ShiftedWindowLoader.h"

#include "io/OpenFilePool.h"

namespace breco {

ShiftedWindowLoader::ShiftedWindowLoader(OpenFilePool* filePool) : m_filePool(filePool) {}

std::optional<LoadedRawWindow> ShiftedWindowLoader::loadRawWindow(const QString& filePath,
                                                                  quint64 fileSize,
                                                                  quint64 outputStart,
                                                                  quint64 outputSize,
                                                                  const ShiftSettings& shift) const {
    if (m_filePool == nullptr || filePath.isEmpty()) {
        return std::nullopt;
    }

    LoadedRawWindow out;
    out.plan = ShiftTransform::makeReadPlan(outputStart, outputSize, fileSize, shift);
    if (out.plan.readSize == 0) {
        return out;
    }

    const auto raw = m_filePool->readChunk(filePath, out.plan.readStart, out.plan.readSize);
    if (!raw.has_value()) {
        return std::nullopt;
    }
    out.bytes = raw.value();
    return out;
}

std::optional<QByteArray> ShiftedWindowLoader::loadTransformedWindow(
    const QString& filePath, quint64 fileSize, quint64 outputStart, quint64 outputSize,
    const ShiftSettings& shift) const {
    const auto rawWindow = loadRawWindow(filePath, fileSize, outputStart, outputSize, shift);
    if (!rawWindow.has_value()) {
        return std::nullopt;
    }

    const LoadedRawWindow& window = rawWindow.value();
    if (shift.amount == 0 && window.plan.readStart == outputStart && window.plan.readSize == outputSize) {
        return window.bytes;
    }

    return ShiftTransform::transformWindow(window.bytes, window.plan.readStart, outputStart, outputSize,
                                           fileSize, shift);
}

}  // namespace breco
