#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "core/socket.h"

namespace vnxfer {

struct ServerInfo {
    int width = 0;
    int height = 0;
    std::string name;
    int protocol_minor = 0;
};

struct Capability {
    uint32_t code = 0;
    std::string vendor;
    std::string name;
};

struct Capabilities {
    bool present = false;
    std::vector<Capability> server;
    std::vector<Capability> client;
    std::vector<Capability> encodings;

    bool hasClient(uint32_t code) const;
    bool hasServer(uint32_t code) const;
};

struct FtMessage {
    uint8_t type = 0;
    uint8_t ctype = 0;
    uint8_t cparam = 0;
    uint32_t size = 0;
    uint32_t length = 0;
    std::vector<uint8_t> payload;
};

class RfbClient {
public:
    RfbClient(std::string host, uint16_t port, std::string password, int io_timeout_ms);

    void connect();
    // Performs version negotiation, authentication, ClientInit/ServerInit and
    // (when Tight auth is selected) reads the interaction capabilities.
    void handshake(int preferred_minor, bool prefer_tight);

    // Byte-level helpers (big-endian).
    uint8_t read_u8(int timeout_ms = -1);
    uint16_t read_u16(int timeout_ms = -1);
    uint32_t read_u32(int timeout_ms = -1);
    uint64_t read_u64(int timeout_ms = -1);
    std::vector<uint8_t> read_bytes(std::size_t n, int timeout_ms = -1);
    bool try_read_byte(uint8_t& value, int timeout_ms);

    void write_u8(uint8_t v);
    void write_u16(uint16_t v);
    void write_u32(uint32_t v);
    void write_u64(uint64_t v);
    void write_bytes(const void* data, std::size_t n);
    void write_bytes(const std::vector<uint8_t>& data);

    // Reads one server message type byte, skipping nothing. Returns false on timeout.
    bool read_message_type(uint8_t& type, int timeout_ms);
    // Consumes a non-FT server message (bell / cut text / colour map).
    void skip_server_message(uint8_t type);
    // Reads a type-7 message (payload included). Non-FT messages are skipped.
    bool read_ft_message(FtMessage& msg, int timeout_ms);
    void send_ft(uint8_t ctype, uint8_t cparam, uint32_t size,
                 const std::vector<uint8_t>& payload = {});

    Socket& socket() { return socket_; }
    const std::string& server_version() const { return server_version_; }
    const std::vector<uint8_t>& security_types() const { return security_types_; }
    uint8_t chosen_security_type() const { return chosen_security_type_; }
    bool tight_selected() const { return chosen_security_type_ == 16; }
    const ServerInfo& server_info() const { return server_info_; }
    const Capabilities& caps() const { return caps_; }
    const std::string& password() const { return password_; }

private:
    void negotiate_version(int preferred_minor);
    void authenticate(bool prefer_tight);
    void read_client_init_reply();
    void read_interaction_caps(int timeout_ms);

    std::string host_;
    uint16_t port_;
    std::string password_;
    int timeout_ms_;

    Socket socket_;
    std::string server_version_;
    std::vector<uint8_t> security_types_;
    uint8_t chosen_security_type_ = 0;
    ServerInfo server_info_;
    Capabilities caps_;
};

}  // namespace vnxfer
