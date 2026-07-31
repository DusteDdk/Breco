#include <fcntl.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include <cstring>
#include <iostream>

namespace {

bool sendStatus(int socketFd, char status, int fd = -1) {
    struct iovec iov {};
    iov.iov_base = &status;
    iov.iov_len = sizeof(status);

    char control[CMSG_SPACE(sizeof(int))] {};
    struct msghdr message {};
    message.msg_iov = &iov;
    message.msg_iovlen = 1;

    if (fd >= 0) {
        message.msg_control = control;
        message.msg_controllen = sizeof(control);
        struct cmsghdr* cmsg = CMSG_FIRSTHDR(&message);
        cmsg->cmsg_level = SOL_SOCKET;
        cmsg->cmsg_type = SCM_RIGHTS;
        cmsg->cmsg_len = CMSG_LEN(sizeof(int));
        std::memcpy(CMSG_DATA(cmsg), &fd, sizeof(fd));
        message.msg_controllen = cmsg->cmsg_len;
    }

    return ::sendmsg(socketFd, &message, 0) == static_cast<ssize_t>(sizeof(status));
}

int connectToSocket(const char* socketPath) {
    const int socketFd = ::socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (socketFd < 0) {
        return -1;
    }

    struct sockaddr_un address {};
    address.sun_family = AF_UNIX;
    if (std::strlen(socketPath) >= sizeof(address.sun_path)) {
        ::close(socketFd);
        return -1;
    }
    std::strncpy(address.sun_path, socketPath, sizeof(address.sun_path) - 1);

    if (::connect(socketFd, reinterpret_cast<struct sockaddr*>(&address), sizeof(address)) != 0) {
        ::close(socketFd);
        return -1;
    }
    return socketFd;
}

}  // namespace

int main(int argc, char* argv[]) {
    if (argc != 3) {
        std::cerr << "usage: breco_privileged_open_helper <socket-path> <file-path>\n";
        return 2;
    }

    const int socketFd = connectToSocket(argv[1]);
    if (socketFd < 0) {
        return 3;
    }

    const int fd = ::open(argv[2], O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
        sendStatus(socketFd, 'E');
        ::close(socketFd);
        return 4;
    }

    const bool sent = sendStatus(socketFd, 'O', fd);
    ::close(fd);
    ::close(socketFd);
    return sent ? 0 : 5;
}
