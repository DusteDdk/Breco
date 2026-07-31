#pragma once

#include <QString>

#include "io/ProtectedSourceOpener.h"

namespace breco {

class UDisks2DeviceOpener {
public:
    bool isAvailable(const QString& devicePath) const;
    ProtectedOpenResult open(const QString& devicePath) const;
};

}  // namespace breco
