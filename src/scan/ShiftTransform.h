#pragma once

#include <QByteArray>
#include <QtGlobal>

#include "model/ResultTypes.h"

namespace breco {

struct ShiftReadPlan {
    quint64 readStart = 0;
    quint64 readSize = 0;
};

class ShiftTransform {
public:
    static ShiftReadPlan makeReadPlan(quint64 outputStart, quint64 outputSize, quint64 fileSize,
                                      const ShiftSettings& shift);
    static QByteArray transformWindow(const QByteArray& rawBytes, quint64 rawStart, quint64 outputStart,
                                      quint64 outputSize, quint64 fileSize, const ShiftSettings& shift);
};

}  // namespace breco
