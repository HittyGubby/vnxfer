#include "core/socket.h"

#include <arpa/inet.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <stdexcept>

namespace vnxfer {

namespace {
[[noreturn]] void fail(const std::string& what) {
    throw std::runtime_error(what + ": " + std::strerror(errno));
}
}  // namespace

Socket::~Socket() { close(); }

Socket::Socket(Socket&& other) noexcept : fd_(other.fd_), timeout_ms_(other.timeout_ms_) {
    other.fd_ = -1;
}

Socket& Socket::operator=(Socket&& other) noexcept {
    if (this != &other) {
        close();
        fd_ = other.fd_;
        timeout_ms_ = other.timeout_ms_;
        other.fd_ = -1;
    }
    return *this;
}

void Socket::close() {
    if (fd_ >= 0) {
        ::close(fd_);
        fd_ = -1;
    }
}

void Socket::set_timeout(int ms) { timeout_ms_ = ms; }

Socket Socket::connect_tcp(const std::string& host, uint16_t port, int timeout_ms) {
    addrinfo hints{};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;

    addrinfo* res = nullptr;
    const std::string service = std::to_string(port);
    const int rc = ::getaddrinfo(host.c_str(), service.c_str(), &hints, &res);
    if (rc != 0) {
        throw std::runtime_error("getaddrinfo(" + host + "): " + gai_strerror(rc));
    }

    Socket self;
    self.timeout_ms_ = timeout_ms;
    std::string last_error = "no address";

    for (addrinfo* ai = res; ai != nullptr; ai = ai->ai_next) {
        int fd = ::socket(ai->ai_family, ai->ai_socktype | SOCK_CLOEXEC, ai->ai_protocol);
        if (fd < 0) {
            last_error = std::strerror(errno);
            continue;
        }

        // Non-blocking connect with deadline.
        int flags = ::fcntl(fd, F_GETFL, 0);
        ::fcntl(fd, F_SETFL, flags | O_NONBLOCK);
        int ok = ::connect(fd, ai->ai_addr, ai->ai_addrlen);
        if (ok != 0 && errno == EINPROGRESS) {
            pollfd pfd{fd, POLLOUT, 0};
            int pr = ::poll(&pfd, 1, timeout_ms);
            if (pr > 0) {
                int so_error = 0;
                socklen_t len = sizeof(so_error);
                ::getsockopt(fd, SOL_SOCKET, SO_ERROR, &so_error, &len);
                ok = so_error == 0 ? 0 : -1;
                if (ok != 0) last_error = std::strerror(so_error);
            } else if (pr == 0) {
                last_error = "connect timed out";
                ok = -1;
            } else {
                last_error = std::strerror(errno);
            }
        } else if (ok != 0) {
            last_error = std::strerror(errno);
        }

        if (ok == 0) {
            ::fcntl(fd, F_SETFL, flags);
            int one = 1;
            ::setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
            self.fd_ = fd;
            break;
        }
        ::close(fd);
    }
    ::freeaddrinfo(res);

    if (!self.valid()) {
        throw std::runtime_error("connect to " + host + ":" + service + " failed: " + last_error);
    }
    return self;
}

void Socket::read_exact(void* buf, std::size_t n) {
    if (!read_exact_timeout(buf, n, timeout_ms_)) {
        throw std::runtime_error("read timed out");
    }
}

bool Socket::read_exact_timeout(void* buf, std::size_t n, int timeout_ms) {
    auto* p = static_cast<uint8_t*>(buf);
    std::size_t done = 0;
    while (done < n) {
        pollfd pfd{fd_, POLLIN, 0};
        int pr = ::poll(&pfd, 1, timeout_ms);
        if (pr == 0) return false;
        if (pr < 0) {
            if (errno == EINTR) continue;
            fail("poll");
        }
        ssize_t got = ::recv(fd_, p + done, n - done, 0);
        if (got == 0) {
            throw std::runtime_error("connection closed by peer (" + std::to_string(done) + "/" +
                                     std::to_string(n) + " bytes read)");
        }
        if (got < 0) {
            if (errno == EINTR) continue;
            if (errno == EAGAIN || errno == EWOULDBLOCK) continue;
            fail("recv");
        }
        done += static_cast<std::size_t>(got);
    }
    return true;
}

void Socket::write_all(const void* buf, std::size_t n) {
    const auto* p = static_cast<const uint8_t*>(buf);
    if (std::getenv("VNXFER_TRACE") != nullptr) {
        std::fprintf(stderr, "[tx]");
        for (std::size_t i = 0; i < n; ++i) std::fprintf(stderr, " %02x", p[i]);
        std::fprintf(stderr, "\n");
    }
    std::size_t done = 0;
    while (done < n) {
        ssize_t sent = ::send(fd_, p + done, n - done, MSG_NOSIGNAL);
        if (sent < 0) {
            if (errno == EINTR) continue;
            fail("send");
        }
        done += static_cast<std::size_t>(sent);
    }
}

}  // namespace vnxfer
