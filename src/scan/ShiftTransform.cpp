#include "scan/ShiftTransform.h"

#include <algorithm>
#include <cstring>

namespace breco {

namespace {
qint64 floorDiv8(qint64 value) {
    if (value >= 0) {
        return value / 8;
    }
    return -(((-value) + 7) / 8);
}
}  // namespace

ShiftReadPlan ShiftTransform::makeReadPlan(quint64 outputStart, quint64 outputSize, quint64 fileSize,
                                           const ShiftSettings& shift) {
    ShiftReadPlan plan;
    if (outputSize == 0 || fileSize == 0) {
        return plan;
    }

    if (shift.amount == 0) {
        plan.readStart = qMin(outputStart, fileSize);
        plan.readSize = qMin(outputSize, fileSize - plan.readStart);
        return plan;
    }

    if (shift.unit == ShiftUnit::Bytes) {
        const qint64 minSrc = static_cast<qint64>(outputStart) + shift.amount;
        const qint64 maxSrc = static_cast<qint64>(outputStart + outputSize - 1) + shift.amount;
        const qint64 clampedMin = std::max<qint64>(0, minSrc);
        const qint64 clampedMax = std::min<qint64>(static_cast<qint64>(fileSize) - 1, maxSrc);
        if (clampedMin > clampedMax) {
            return plan;
        }
        plan.readStart = static_cast<quint64>(clampedMin);
        plan.readSize = static_cast<quint64>(clampedMax - clampedMin + 1);
        return plan;
    }

    const qint64 minSrcBit = static_cast<qint64>(outputStart) * 8 + shift.amount;
    const qint64 maxSrcBit = static_cast<qint64>(outputStart + outputSize) * 8 - 1 + shift.amount;
    const qint64 minSrcByte = floorDiv8(minSrcBit);
    const qint64 maxSrcByte = floorDiv8(maxSrcBit);
    const qint64 clampedMin = std::max<qint64>(0, minSrcByte);
    const qint64 clampedMax = std::min<qint64>(static_cast<qint64>(fileSize) - 1, maxSrcByte);
    if (clampedMin > clampedMax) {
        return plan;
    }
    plan.readStart = static_cast<quint64>(clampedMin);
    plan.readSize = static_cast<quint64>(clampedMax - clampedMin + 1);
    return plan;
}

QByteArray ShiftTransform::transformWindow(const QByteArray& rawBytes, quint64 rawStart, quint64 outputStart,
                                           quint64 outputSize, quint64 fileSize,
                                           const ShiftSettings& shift) {
    QByteArray out(static_cast<int>(outputSize), '\0');
    if (outputSize == 0 || fileSize == 0) {
        return out;
    }

    if (shift.amount == 0) {
        // Fast path: when unshifted and read plan exactly matches output, reuse raw bytes directly.
        if (rawStart == outputStart && rawBytes.size() == out.size()) {
            return rawBytes;
        }

        // Generic unshifted copy path without per-byte bounds checks.
        const qint64 begin = static_cast<qint64>(outputStart) - static_cast<qint64>(rawStart);
        qint64 srcStart = begin;
        qint64 dstStart = 0;
        if (srcStart < 0) {
            dstStart = -srcStart;
            srcStart = 0;
        }

        const qint64 rawSize = static_cast<qint64>(rawBytes.size());
        const qint64 outSize = static_cast<qint64>(out.size());
        if (srcStart >= rawSize || dstStart >= outSize) {
            return out;
        }

        const qint64 copyBytes = std::min<qint64>(rawSize - srcStart, outSize - dstStart);
        if (copyBytes > 0) {
            std::memcpy(out.data() + static_cast<int>(dstStart),
                        rawBytes.constData() + static_cast<int>(srcStart),
                        static_cast<size_t>(copyBytes));
        }
        return out;
    }

    if (shift.unit == ShiftUnit::Bytes) {
        for (int i = 0; i < out.size(); ++i) {
            const qint64 srcGlobal = static_cast<qint64>(outputStart + static_cast<quint64>(i)) + shift.amount;
            if (srcGlobal < 0 || srcGlobal >= static_cast<qint64>(fileSize)) {
                continue;
            }
            const qint64 local = srcGlobal - static_cast<qint64>(rawStart);
            if (local >= 0 && local < rawBytes.size()) {
                out[i] = rawBytes.at(static_cast<int>(local));
            }
        }
        return out;
    }

    for (int i = 0; i < out.size(); ++i) {
        unsigned char dstByte = 0;
        for (int bit = 0; bit < 8; ++bit) {
            const qint64 outBitGlobal =
                static_cast<qint64>(outputStart + static_cast<quint64>(i)) * 8 + bit;
            const qint64 srcBitGlobal = outBitGlobal + shift.amount;
            if (srcBitGlobal < 0 || srcBitGlobal >= static_cast<qint64>(fileSize) * 8) {
                continue;
            }
            const qint64 srcByteGlobal = srcBitGlobal / 8;
            const int srcBitInByte = 7 - static_cast<int>(srcBitGlobal % 8);
            const qint64 srcLocal = srcByteGlobal - static_cast<qint64>(rawStart);
            if (srcLocal < 0 || srcLocal >= rawBytes.size()) {
                continue;
            }
            const unsigned char src = static_cast<unsigned char>(rawBytes.at(static_cast<int>(srcLocal)));
            const unsigned char bitVal = (src >> srcBitInByte) & 0x1U;
            dstByte |= static_cast<unsigned char>(bitVal << (7 - bit));
        }
        out[i] = static_cast<char>(dstByte);
    }
    return out;
}

}  // namespace breco
