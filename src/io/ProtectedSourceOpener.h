#pragma once

#include <QString>
#include <QtGlobal>

namespace breco {

enum class ProtectedSourceKind { RegularFile, BlockDevice };

struct ProtectedOpenResult {
    enum class Status { Unavailable, Failed, Opened };

    Status status = Status::Unavailable;
    int fd = -1;
    quint64 fileSize = 0;
    QString errorMessage;

    static ProtectedOpenResult unavailable();
    static ProtectedOpenResult failed(const QString& message = {});
    static ProtectedOpenResult opened(int fd, quint64 fileSize);
};

class ProtectedSourceOpener {
public:
    virtual ~ProtectedSourceOpener() = default;

    virtual bool isAvailable(const QString& path, ProtectedSourceKind kind) const = 0;
    virtual ProtectedOpenResult open(const QString& path, ProtectedSourceKind kind) = 0;
};

class DefaultProtectedSourceOpener final : public ProtectedSourceOpener {
public:
    bool isAvailable(const QString& path, ProtectedSourceKind kind) const override;
    ProtectedOpenResult open(const QString& path, ProtectedSourceKind kind) override;
};

}  // namespace breco
