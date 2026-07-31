#include "io/UDisks2DeviceOpener.h"

#include <QDBusConnection>
#include <QDBusConnectionInterface>
#include <QDBusInterface>
#include <QDBusObjectPath>
#include <QDBusReply>
#include <QDBusUnixFileDescriptor>
#include <QDBusVariant>
#include <QVariantMap>

#include <fcntl.h>
#include <linux/fs.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include <optional>

namespace breco {

namespace {

constexpr const char* kUDisksService = "org.freedesktop.UDisks2";
constexpr const char* kUDisksManagerPath = "/org/freedesktop/UDisks2/Manager";
constexpr const char* kUDisksManagerInterface = "org.freedesktop.UDisks2.Manager";
constexpr const char* kUDisksBlockInterface = "org.freedesktop.UDisks2.Block";
constexpr const char* kDBusPropertiesInterface = "org.freedesktop.DBus.Properties";

QString devicePathFromProperty(const QVariant& value) {
    QByteArray bytes = value.toByteArray();
    const int nul = bytes.indexOf('\0');
    if (nul >= 0) {
        bytes.truncate(nul);
    }
    return QString::fromLocal8Bit(bytes);
}

std::optional<QDBusObjectPath> objectPathForDevice(const QString& devicePath) {
    QDBusInterface manager(QString::fromLatin1(kUDisksService),
                           QString::fromLatin1(kUDisksManagerPath),
                           QString::fromLatin1(kUDisksManagerInterface),
                           QDBusConnection::systemBus());
    const QDBusReply<QList<QDBusObjectPath>> devices =
        manager.call(QStringLiteral("GetBlockDevices"), QVariantMap{});
    if (!devices.isValid()) {
        return std::nullopt;
    }

    for (const QDBusObjectPath& objectPath : devices.value()) {
        QDBusInterface properties(QString::fromLatin1(kUDisksService), objectPath.path(),
                                  QString::fromLatin1(kDBusPropertiesInterface),
                                  QDBusConnection::systemBus());
        const QDBusReply<QDBusVariant> deviceProperty =
            properties.call(QStringLiteral("Get"), QString::fromLatin1(kUDisksBlockInterface),
                            QStringLiteral("Device"));
        if (!deviceProperty.isValid()) {
            continue;
        }
        if (devicePathFromProperty(deviceProperty.value().variant()) == devicePath) {
            return objectPath;
        }
    }

    return std::nullopt;
}

quint64 blockDeviceSizeFromFd(int fd) {
    quint64 bytes = 0;
    if (::ioctl(fd, BLKGETSIZE64, &bytes) != 0) {
        return 0;
    }
    return bytes;
}

ProtectedOpenResult duplicateDescriptorResult(const QDBusUnixFileDescriptor& descriptor) {
    if (!descriptor.isValid()) {
        return ProtectedOpenResult::failed(QStringLiteral("udisks2 returned an invalid descriptor"));
    }
    const int fd = ::dup(descriptor.fileDescriptor());
    if (fd < 0) {
        return ProtectedOpenResult::failed(QStringLiteral("Could not duplicate udisks2 descriptor"));
    }
    const quint64 fileSize = blockDeviceSizeFromFd(fd);
    if (fileSize == 0) {
        ::close(fd);
        return ProtectedOpenResult::failed(QStringLiteral("Could not determine block device size"));
    }
    return ProtectedOpenResult::opened(fd, fileSize);
}

}  // namespace

bool UDisks2DeviceOpener::isAvailable(const QString& devicePath) const {
    QDBusConnectionInterface* interface = QDBusConnection::systemBus().interface();
    if (interface == nullptr) {
        return false;
    }
    const QDBusReply<bool> registered =
        interface->isServiceRegistered(QString::fromLatin1(kUDisksService));
    return registered.isValid() && registered.value() && objectPathForDevice(devicePath).has_value();
}

ProtectedOpenResult UDisks2DeviceOpener::open(const QString& devicePath) const {
    const std::optional<QDBusObjectPath> objectPath = objectPathForDevice(devicePath);
    if (!objectPath.has_value()) {
        return ProtectedOpenResult::unavailable();
    }

    QDBusInterface block(QString::fromLatin1(kUDisksService), objectPath->path(),
                         QString::fromLatin1(kUDisksBlockInterface),
                         QDBusConnection::systemBus());

    QVariantMap options;
    options.insert(QStringLiteral("flags"), O_CLOEXEC);
    QDBusReply<QDBusUnixFileDescriptor> openDevice =
        block.call(QStringLiteral("OpenDevice"), QStringLiteral("r"), options);
    if (openDevice.isValid()) {
        return duplicateDescriptorResult(openDevice.value());
    }

    QDBusReply<QDBusUnixFileDescriptor> openForBackup =
        block.call(QStringLiteral("OpenForBackup"), QVariantMap{});
    if (openForBackup.isValid()) {
        return duplicateDescriptorResult(openForBackup.value());
    }

    return ProtectedOpenResult::failed(openDevice.error().message());
}

}  // namespace breco
