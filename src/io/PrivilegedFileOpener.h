#pragma once

#include <QString>

#include "io/ProtectedSourceOpener.h"

namespace breco {

class PrivilegedFileOpener {
public:
    bool isAvailable(const QString& filePath) const;
    ProtectedOpenResult open(const QString& filePath) const;

private:
    QString helperPath() const;
};

}  // namespace breco
