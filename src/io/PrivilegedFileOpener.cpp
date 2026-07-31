#include "io/PrivilegedFileOpener.h"

#include <QCoreApplication>
#include <QFileInfo>
#include <QLocalServer>
#include <QLocalSocket>
#include <QProcess>
#include <QStandardPaths>
#include <QTemporaryDir>

#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cstring>

namespace breco {

namespace {

constexpr int kHelperTimeoutMs = 120000;

quint64 regularFileSizeFromFd(int fd) {
    struct stat st {};
    if (::fstat(fd, &st) != 0 || !S_ISREG(st.st_mode) || st.st_size <= 0) {
        return 0;
    }
    return static_cast<quint64>(st.st_size);
}

int receiveFd(int socketFd) {
    char payload = '\0';
    struct iovec iov {};
    iov.iov_base = &payload;
    iov.iov_len = sizeof(payload);

    char control[CMSG_SPACE(sizeof(int))] {};
    struct msghdr message {};
    message.msg_iov = &iov;
    message.msg_iovlen = 1;
    message.msg_control = control;
    message.msg_controllen = sizeof(control);

    const ssize_t nread = ::recvmsg(socketFd, &message, 0);
    if (nread <= 0 || payload != 'O') {
        return -1;
    }

    for (struct cmsghdr* cmsg = CMSG_FIRSTHDR(&message); cmsg != nullptr;
         cmsg = CMSG_NXTHDR(&message, cmsg)) {
        if (cmsg->cmsg_level == SOL_SOCKET && cmsg->cmsg_type == SCM_RIGHTS &&
            cmsg->cmsg_len == CMSG_LEN(sizeof(int))) {
            int fd = -1;
            std::memcpy(&fd, CMSG_DATA(cmsg), sizeof(fd));
            return fd;
        }
    }
    return -1;
}

}  // namespace

bool PrivilegedFileOpener::isAvailable(const QString& filePath) const {
    const QFileInfo info(filePath);
    return info.exists() && info.isFile() && QFileInfo(helperPath()).isExecutable() &&
           !QStandardPaths::findExecutable(QStringLiteral("pkexec")).isEmpty();
}

ProtectedOpenResult PrivilegedFileOpener::open(const QString& filePath) const {
    if (!isAvailable(filePath)) {
        return ProtectedOpenResult::unavailable();
    }

    QTemporaryDir socketDir;
    if (!socketDir.isValid()) {
        return ProtectedOpenResult::failed(QStringLiteral("Could not create helper socket directory"));
    }
    const QString socketPath = socketDir.filePath(QStringLiteral("fd.sock"));
    QLocalServer::removeServer(socketPath);

    QLocalServer server;
    if (!server.listen(socketPath)) {
        return ProtectedOpenResult::failed(server.errorString());
    }

    QProcess helper;
    helper.start(QStandardPaths::findExecutable(QStringLiteral("pkexec")),
                 {helperPath(), socketPath, QFileInfo(filePath).absoluteFilePath()});
    if (!helper.waitForStarted()) {
        return ProtectedOpenResult::failed(helper.errorString());
    }

    if (!server.waitForNewConnection(kHelperTimeoutMs)) {
        helper.kill();
        helper.waitForFinished(5000);
        return ProtectedOpenResult::failed(QStringLiteral("Timed out waiting for privileged helper"));
    }

    QLocalSocket* socket = server.nextPendingConnection();
    if (socket == nullptr) {
        helper.kill();
        helper.waitForFinished(5000);
        return ProtectedOpenResult::failed(QStringLiteral("Privileged helper did not connect"));
    }

    const int fd = receiveFd(static_cast<int>(socket->socketDescriptor()));
    socket->disconnectFromServer();
    socket->deleteLater();
    helper.waitForFinished(5000);

    if (fd < 0) {
        return ProtectedOpenResult::failed(QStringLiteral("Privileged helper did not return a file"));
    }
    const quint64 fileSize = regularFileSizeFromFd(fd);
    if (fileSize == 0) {
        ::close(fd);
        return ProtectedOpenResult::failed(QStringLiteral("Could not determine protected file size"));
    }
    return ProtectedOpenResult::opened(fd, fileSize);
}

QString PrivilegedFileOpener::helperPath() const {
    return QCoreApplication::applicationDirPath() + QStringLiteral("/breco_privileged_open_helper");
}

}  // namespace breco
