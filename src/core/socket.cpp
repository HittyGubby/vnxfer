#include "core/socket.h"

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <stdexcept>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#ifndef MSG_NOSIGNAL
#define MSG_NOSIGNAL 0
#endif
#ifndef SOCK_CLOEXEC
#define SOCK_CLOEXEC 0
#endif
#else
#include <arpa/inet.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

namespace vnxfer {

namespace {

#ifdef _WIN32
SOCKET as_socket(socket_handle h) { return static_cast<SOCKET>(h); }
using pollfd_t = WSAPOLLFD;
int poll_wait(pollfd_t* fds, int count, int timeout_ms) { return WSAPoll(fds, count, timeout_ms); }
void close_socket(socket_handle h) { ::closesocket(as_socket(h)); }
int last_error() { return WSAGetLastError(); }
void set_nonblocking(socket_handle h, bool on) {
    u_long mode = on ? 1 : 0;
    ::ioctlsocket(as_socket(h), FIONBIO, &mode);
}
struct WinsockInit {
    WinsockInit() {
        WSADATA data;
        WSAStartup(MAKEWORD(2, 2), &data);
    }
};
WinsockInit g_winsock;
#else
using pollfd_t = struct pollfd;
int as_socket(socket_handle h) { return h; }
int poll_wait(pollfd_t* fds, int count, int timeout_ms) { return ::poll(fds, count, timeout_ms); }
void close_socket(socket_handle h) { ::close(h); }
int last_error() { return errno; }
void set_nonblocking(socket_handle h, bool on) {
    int flags = ::fcntl(h, F_GETFL, 0);
    ::fcntl(h, F_SETFL, on ? (flags | O_NONBLOCK) : (flags & ~O_NONBLOCK));
}
#endif

}  // namespace

Socket::~Socket() { close(); }

Socket::Socket(Socket&& other) noexcept : fd_(other.fd_), timeout_ms_(other.timeout_ms_) {
    other.fd_ = kInvalid;
}

Socket& Socket::operator=(Socket&& other) noexcept {
    if (this != &other) {
        close();
        fd_ = other.fd_;
        timeout_ms_ = other.timeout_ms_;
        other.fd_ = kInvalid;
    }
    return *this;
}

void Socket::close() {
    if (valid()) {
        close_socket(fd_);
        fd_ = kInvalid;
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
        throw std::runtime_error("getaddrinfo(" + host + ") failed: " + std::to_string(rc));
    }

    Socket self;
    self.timeout_ms_ = timeout_ms;
    std::string last_error_text = "no address";

    for (addrinfo* ai = res; ai != nullptr; ai = ai->ai_next) {
        socket_handle handle = static_cast<socket_handle>(
            ::socket(static_cast<int>(ai->ai_family),
                     static_cast<int>(ai->ai_socktype | SOCK_CLOEXEC),
                     static_cast<int>(ai->ai_protocol)));
        if (!self.valid() && handle == kInvalid) {
            last_error_text = std::strerror(last_error());
            continue;
        }

        set_nonblocking(handle, true);
        int ok = ::connect(as_socket(handle), ai->ai_addr, static_cast<int>(ai->ai_addrlen));
#ifdef _WIN32
        const bool in_progress = ok != 0 && last_error() == WSAEWOULDBLOCK;
#else
        const bool in_progress = ok != 0 && last_error() == EINPROGRESS;
#endif
        if (in_progress) {
            pollfd_t pfd{};
            pfd.fd = as_socket(handle);
            pfd.events = POLLOUT;
            int pr = poll_wait(&pfd, 1, timeout_ms);
            if (pr > 0) {
                int so_error = 0;
#ifdef _WIN32
                int len = sizeof(so_error);
#else
                socklen_t len = sizeof(so_error);
#endif
                ::getsockopt(as_socket(handle), SOL_SOCKET, SO_ERROR,
                             reinterpret_cast<char*>(&so_error), &len);
                ok = so_error == 0 ? 0 : -1;
                if (ok != 0) last_error_text = "connect failed: " + std::to_string(so_error);
            } else if (pr == 0) {
                last_error_text = "connect timed out";
                ok = -1;
            } else {
                last_error_text = std::strerror(last_error());
            }
        } else if (ok != 0) {
            last_error_text = std::strerror(last_error());
        }

        if (ok == 0) {
            set_nonblocking(handle, false);
            int one = 1;
            ::setsockopt(as_socket(handle), IPPROTO_TCP, TCP_NODELAY,
                         reinterpret_cast<const char*>(&one), sizeof(one));
            self.fd_ = handle;
            break;
        }
        close_socket(handle);
    }
    ::freeaddrinfo(res);

    if (!self.valid()) {
        throw std::runtime_error("connect to " + host + ":" + service + " failed: " +
                                 last_error_text);
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
        pollfd_t pfd{};
        pfd.fd = as_socket(fd_);
        pfd.events = POLLIN;
        int pr = poll_wait(&pfd, 1, timeout_ms);
        if (pr == 0) return false;
        if (pr < 0) {
            if (last_error() == EINTR) continue;
            throw std::runtime_error("poll failed");
        }
        int got = ::recv(as_socket(fd_), reinterpret_cast<char*>(p + done),
                         static_cast<int>(n - done), 0);
        if (got == 0) {
            throw std::runtime_error("connection closed by peer (" + std::to_string(done) + "/" +
                                     std::to_string(n) + " bytes read)");
        }
        if (got < 0) {
            int err = last_error();
            if (err == EINTR || err == EAGAIN || err == EWOULDBLOCK) continue;
            throw std::runtime_error("recv failed: " + std::to_string(err));
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
        int sent = ::send(as_socket(fd_), reinterpret_cast<const char*>(p + done),
                          static_cast<int>(n - done), MSG_NOSIGNAL);
        if (sent < 0) {
            int err = last_error();
            if (err == EINTR) continue;
            throw std::runtime_error("send failed: " + std::to_string(err));
        }
        done += static_cast<std::size_t>(sent);
    }
}

}  // namespace vnxfer
