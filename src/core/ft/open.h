#pragma once

#include <cstdint>
#include <memory>
#include <string>

#include "core/ft/session.h"

namespace vnxfer {

struct ConnectionOptions {
    std::string host = "127.0.0.1";
    uint16_t port = 5900;
    std::string password;
    int timeout_ms = 5000;
};

// Connects, negotiates and returns the matching file-transfer backend.
// Throws std::runtime_error when the server offers no usable file transfer.
std::unique_ptr<FtSession> open_session(const ConnectionOptions& options);

}  // namespace vnxfer
