#include "io/ProtectedSourceOpener.h"

#ifdef Q_OS_LINUX
#include "io/PrivilegedFileOpener.h"
#include "io/UDisks2DeviceOpener.h"
#endif

namespace breco {

ProtectedOpenResult ProtectedOpenResult::unavailable() {
    ProtectedOpenResult result;
    result.status = Status::Unavailable;
    return result;
}

ProtectedOpenResult ProtectedOpenResult::failed(const QString& message) {
    ProtectedOpenResult result;
    result.status = Status::Failed;
    result.errorMessage = message;
    return result;
}

ProtectedOpenResult ProtectedOpenResult::opened(int fd, quint64 fileSize) {
    ProtectedOpenResult result;
    result.status = Status::Opened;
    result.fd = fd;
    result.fileSize = fileSize;
    return result;
}

bool DefaultProtectedSourceOpener::isAvailable(const QString& path, ProtectedSourceKind kind) const {
#ifdef Q_OS_LINUX
    if (kind == ProtectedSourceKind::BlockDevice) {
        return UDisks2DeviceOpener().isAvailable(path);
    }
    return PrivilegedFileOpener().isAvailable(path);
#else
    Q_UNUSED(path)
    Q_UNUSED(kind)
    return false;
#endif
}

ProtectedOpenResult DefaultProtectedSourceOpener::open(const QString& path,
                                                       ProtectedSourceKind kind) {
#ifdef Q_OS_LINUX
    if (kind == ProtectedSourceKind::BlockDevice) {
        return UDisks2DeviceOpener().open(path);
    }
    return PrivilegedFileOpener().open(path);
#else
    Q_UNUSED(path)
    Q_UNUSED(kind)
    return ProtectedOpenResult::unavailable();
#endif
}

}  // namespace breco
