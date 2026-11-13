#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

namespace vnxfer {

// Minimal blocking TCP socket with absolute read/write deadlines.
class Socket {
public:
    Socket() = default;
    ~Socket();
    Socket(const Socket&) = delete;
    Socket& operator=(const Socket&) = delete;
    Socket(Socket&& other) noexcept;
    Socket& operator=(Socket&& other) noexcept;

    static Socket connect_tcp(const std::string& host, uint16_t port, int timeout_ms);

    void read_exact(void* buf, std::size_t n);
    // Returns false on timeout, throws on error/EOF.
    bool read_exact_timeout(void* buf, std::size_t n, int timeout_ms);
    void write_all(const void* buf, std::size_t n);
    void set_timeout(int ms);
    void close();

    bool valid() const { return fd_ >= 0; }
    int fd() const { return fd_; }

private:
    int fd_ = -1;
    int timeout_ms_ = 5000;
};

}  // namespace vnxfer
