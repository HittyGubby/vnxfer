#include "core/rfb_client.h"

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstring>
#include <stdexcept>

#include "core/des.h"
#include "core/rfb_protocol.h"

namespace vnxfer {

namespace {
std::string to_string_bytes(const std::vector<uint8_t>& v) {
    return std::string(v.begin(), v.end());
}
}  // namespace

bool Capabilities::hasClient(uint32_t code) const {
    return std::any_of(client.begin(), client.end(),
                       [code](const Capability& c) { return c.code == code; });
}

bool Capabilities::hasServer(uint32_t code) const {
    return std::any_of(server.begin(), server.end(),
                       [code](const Capability& c) { return c.code == code; });
}

RfbClient::RfbClient(std::string host, uint16_t port, std::string password, int io_timeout_ms)
    : host_(std::move(host)), port_(port), password_(std::move(password)), timeout_ms_(io_timeout_ms) {}

void RfbClient::connect() {
    socket_ = Socket::connect_tcp(host_, port_, timeout_ms_);
    socket_.set_timeout(timeout_ms_);
}

uint8_t RfbClient::read_u8(int timeout_ms) {
    uint8_t v = 0;
    if (timeout_ms < 0) {
        socket_.read_exact(&v, 1);
    } else if (!socket_.read_exact_timeout(&v, 1, timeout_ms)) {
        throw std::runtime_error("read timed out");
    }
    return v;
}

uint16_t RfbClient::read_u16(int timeout_ms) {
    std::array<uint8_t, 2> b{};
    if (timeout_ms < 0) {
        socket_.read_exact(b.data(), b.size());
    } else if (!socket_.read_exact_timeout(b.data(), b.size(), timeout_ms)) {
        throw std::runtime_error("read timed out");
    }
    return static_cast<uint16_t>(b[0] << 8 | b[1]);
}

uint32_t RfbClient::read_u32(int timeout_ms) {
    std::array<uint8_t, 4> b{};
    if (timeout_ms < 0) {
        socket_.read_exact(b.data(), b.size());
    } else if (!socket_.read_exact_timeout(b.data(), b.size(), timeout_ms)) {
        throw std::runtime_error("read timed out");
    }
    return static_cast<uint32_t>(b[0]) << 24 | static_cast<uint32_t>(b[1]) << 16 |
           static_cast<uint32_t>(b[2]) << 8 | static_cast<uint32_t>(b[3]);
}

uint64_t RfbClient::read_u64(int timeout_ms) {
    uint64_t hi = read_u32(timeout_ms);
    uint64_t lo = read_u32(timeout_ms);
    return (hi << 32) | lo;
}

std::vector<uint8_t> RfbClient::read_bytes(std::size_t n, int timeout_ms) {
    std::vector<uint8_t> out(n);
    if (n == 0) return out;
    if (timeout_ms < 0) {
        socket_.read_exact(out.data(), n);
    } else if (!socket_.read_exact_timeout(out.data(), n, timeout_ms)) {
        throw std::runtime_error("read timed out");
    }
    return out;
}

bool RfbClient::try_read_byte(uint8_t& value, int timeout_ms) {
    return socket_.read_exact_timeout(&value, 1, timeout_ms);
}

void RfbClient::write_u8(uint8_t v) { socket_.write_all(&v, 1); }

void RfbClient::write_u16(uint16_t v) {
    std::array<uint8_t, 2> b{static_cast<uint8_t>(v >> 8), static_cast<uint8_t>(v)};
    socket_.write_all(b.data(), b.size());
}

void RfbClient::write_u32(uint32_t v) {
    std::array<uint8_t, 4> b{static_cast<uint8_t>(v >> 24), static_cast<uint8_t>(v >> 16),
                             static_cast<uint8_t>(v >> 8), static_cast<uint8_t>(v)};
    socket_.write_all(b.data(), b.size());
}

void RfbClient::write_u64(uint64_t v) {
    write_u32(static_cast<uint32_t>(v >> 32));
    write_u32(static_cast<uint32_t>(v));
}

void RfbClient::write_bytes(const void* data, std::size_t n) { socket_.write_all(data, n); }

void RfbClient::write_bytes(const std::vector<uint8_t>& data) {
    if (!data.empty()) socket_.write_all(data.data(), data.size());
}

void RfbClient::handshake(int preferred_minor, bool prefer_tight) {
    negotiate_version(preferred_minor);
    authenticate(prefer_tight);
    read_client_init_reply();
    if (tight_selected()) {
        read_interaction_caps(2000);
    }
}

void RfbClient::negotiate_version(int preferred_minor) {
    std::array<char, 12> buf{};
    socket_.read_exact(buf.data(), buf.size());
    server_version_.assign(buf.data(), 12);

    int major = 0;
    int minor = 0;
    if (std::sscanf(server_version_.c_str(), "RFB %3d.%3d", &major, &minor) != 2) {
        throw std::runtime_error("bad RFB version banner: " + server_version_);
    }
    server_info_.protocol_minor = minor;

    int client_minor = std::min(preferred_minor, minor);
    // 3.6 is a private libvncserver/UltraVNC variant with the 4-byte security-type layout.
    if (client_minor > 3 && client_minor < 7) client_minor = minor;

    char out[16];
    std::snprintf(out, sizeof(out), "RFB %03d.%03d\n", 3, client_minor);
    socket_.write_all(out, 12);

    if (client_minor >= 7) {
        uint8_t n = read_u8();
        if (n == 0) {
            uint32_t reason_len = read_u32();
            throw std::runtime_error("server refused connection: " +
                                     to_string_bytes(read_bytes(reason_len)));
        }
        security_types_ = read_bytes(n);
    } else {
        uint32_t t = read_u32();
        if (t == 0) {
            uint32_t reason_len = read_u32();
            throw std::runtime_error("server refused connection: " +
                                     to_string_bytes(read_bytes(reason_len)));
        }
        security_types_.push_back(static_cast<uint8_t>(t));
    }
}

void RfbClient::authenticate(bool prefer_tight) {
    const auto& types = security_types_;
    auto contains = [&types](uint8_t t) {
        return std::find(types.begin(), types.end(), t) != types.end();
    };

    bool tight = prefer_tight && contains(rfb::kSecTight);
    uint8_t chosen = 0;
    if (tight) {
        chosen = rfb::kSecTight;
    } else if (contains(rfb::kSecVncAuth)) {
        chosen = rfb::kSecVncAuth;
    } else if (contains(rfb::kSecNone)) {
        chosen = rfb::kSecNone;
    } else {
        throw std::runtime_error("no supported security type (server offered " +
                                 std::to_string(types.size()) + ")");
    }
    chosen_security_type_ = chosen;
    // RFB 3.3/3.6: the server dictates the type, the client sends nothing.
    // RFB 3.7+: the client replies with one byte.
    if (server_info_.protocol_minor >= 7) write_u8(chosen);

    if (chosen == rfb::kSecTight) {
        uint32_t n_tunnel = read_u32();
        if (n_tunnel > 0) {
            std::vector<Capability> tunnels;
            tunnels.reserve(n_tunnel);
            for (uint32_t i = 0; i < n_tunnel; ++i) {
                Capability c;
                c.code = read_u32();
                auto vendor = read_bytes(4);
                auto name = read_bytes(8);
                c.vendor.assign(vendor.begin(), vendor.end());
                c.name.assign(name.begin(), name.end());
                tunnels.push_back(std::move(c));
            }
            const Capability* pick = &tunnels.front();
            for (const auto& c : tunnels) {
                if (c.name.rfind("NOTUNNEL", 0) == 0) {
                    pick = &c;
                    break;
                }
            }
            write_u32(pick->code);
        }

        uint32_t n_auth = read_u32();
        std::vector<Capability> auths;
        auths.reserve(n_auth);
        for (uint32_t i = 0; i < n_auth; ++i) {
            Capability c;
            c.code = read_u32();
            auto vendor = read_bytes(4);
            auto name = read_bytes(8);
            c.vendor.assign(vendor.begin(), vendor.end());
            c.name.assign(name.begin(), name.end());
            auths.push_back(std::move(c));
        }
        const Capability* pick = nullptr;
        for (const auto& c : auths) {
            if (c.name.rfind("VNCAUTH", 0) == 0) {
                pick = &c;
                break;
            }
        }
        if (pick == nullptr) {
            for (const auto& c : auths) {
                if (c.name.rfind("NONE", 0) == 0) {
                    pick = &c;
                    break;
                }
            }
        }
        // n_auth == 0 means "no auth required" (libvncserver sends no caps).
        if (pick != nullptr) write_u32(pick->code);

        if (pick != nullptr && pick->name.rfind("VNCAUTH", 0) == 0) {
            if (password_.empty()) throw std::runtime_error("server requires a VNC password");
            std::array<uint8_t, 16> challenge{};
            socket_.read_exact(challenge.data(), challenge.size());
            auto resp = vnc_auth_response(challenge, password_);
            socket_.write_all(resp.data(), resp.size());
        }
        uint32_t result = read_u32();
        if (result != 0) throw std::runtime_error("tight authentication failed (code " +
                                                  std::to_string(result) + ")");
        return;
    }

    if (chosen == rfb::kSecVncAuth) {
        if (password_.empty()) throw std::runtime_error("server requires a VNC password");
        std::array<uint8_t, 16> challenge{};
        socket_.read_exact(challenge.data(), challenge.size());
        auto resp = vnc_auth_response(challenge, password_);
        socket_.write_all(resp.data(), resp.size());
    }

    if (server_info_.protocol_minor >= 8) {
        uint32_t result = read_u32();
        if (result != 0) throw std::runtime_error("authentication failed (code " +
                                                  std::to_string(result) + ")");
    }
}

void RfbClient::read_client_init_reply() {
    write_u8(1);  // shared session

    std::array<uint8_t, 24> si{};
    socket_.read_exact(si.data(), si.size());
    server_info_.width = static_cast<int>(si[0] << 8 | si[1]);
    server_info_.height = static_cast<int>(si[2] << 8 | si[3]);
    uint32_t name_len = static_cast<uint32_t>(si[20]) << 24 |
                        static_cast<uint32_t>(si[21]) << 16 |
                        static_cast<uint32_t>(si[22]) << 8 | si[23];
    if (name_len > 65536) throw std::runtime_error("server name too long");
    server_info_.name = to_string_bytes(read_bytes(name_len));

    // No encodings: we only care about file transfer right now.
    write_u8(rfb::kMsgSetEncodings);
    write_u8(0);
    write_u16(0);
}

void RfbClient::read_interaction_caps(int timeout_ms) {
    std::array<uint8_t, 8> head{};
    if (!socket_.read_exact_timeout(head.data(), head.size(), timeout_ms)) {
        caps_.present = false;
        return;
    }
    uint16_t n_server = static_cast<uint16_t>(head[0] << 8 | head[1]);
    uint16_t n_client = static_cast<uint16_t>(head[2] << 8 | head[3]);
    uint16_t n_enc = static_cast<uint16_t>(head[4] << 8 | head[5]);
    uint32_t total = static_cast<uint32_t>(n_server) + n_client + n_enc;
    if (total > 1024) throw std::runtime_error("interaction caps look bogus");

    auto body = read_bytes(total * 16u);
    auto parse = [&body](std::size_t offset, uint16_t count, std::vector<Capability>& out) {
        out.reserve(count);
        for (uint16_t i = 0; i < count; ++i) {
            std::size_t p = offset + static_cast<std::size_t>(i) * 16;
            Capability c;
            c.code = static_cast<uint32_t>(body[p]) << 24 | static_cast<uint32_t>(body[p + 1]) << 16 |
                     static_cast<uint32_t>(body[p + 2]) << 8 | body[p + 3];
            c.vendor.assign(reinterpret_cast<const char*>(&body[p + 4]), 4);
            c.name.assign(reinterpret_cast<const char*>(&body[p + 8]), 8);
            out.push_back(std::move(c));
        }
    };
    parse(0, n_server, caps_.server);
    parse(static_cast<std::size_t>(n_server) * 16, n_client, caps_.client);
    parse(static_cast<std::size_t>(n_server + n_client) * 16, n_enc, caps_.encodings);
    caps_.present = true;
}

bool RfbClient::read_message_type(uint8_t& type, int timeout_ms) {
    return socket_.read_exact_timeout(&type, 1, timeout_ms);
}

void RfbClient::skip_server_message(uint8_t type) {
    switch (type) {
        case rfb::kMsgBell:
            return;
        case rfb::kMsgServerCutText: {
            read_bytes(3);
            uint32_t len = read_u32();
            if (len > (1u << 20)) throw std::runtime_error("cut text too large");
            read_bytes(len);
            return;
        }
        case rfb::kMsgSetColourMapEntries: {
            read_bytes(3);
            uint16_t n = read_u16();
            read_bytes(static_cast<std::size_t>(n) * 6);
            return;
        }
        default:
            throw std::runtime_error("unexpected server message type " + std::to_string(type));
    }
}

bool RfbClient::read_ft_message(FtMessage& msg, int timeout_ms) {
    for (;;) {
        uint8_t type = 0;
        if (!read_message_type(type, timeout_ms)) return false;
        if (type != rfb::kMsgFileTransfer) {
            skip_server_message(type);
            continue;
        }
        msg.type = type;
        msg.ctype = read_u8();
        msg.cparam = read_u8();
        read_u8();  // pad
        msg.size = read_u32();
        msg.length = read_u32();
        if (msg.length > (1u << 26)) throw std::runtime_error("file transfer payload too large");
        msg.payload = read_bytes(msg.length);
        return true;
    }
}

void RfbClient::send_ft(uint8_t ctype, uint8_t cparam, uint32_t size,
                        const std::vector<uint8_t>& payload) {
    write_u8(rfb::kMsgFileTransfer);
    write_u8(ctype);
    write_u8(cparam);
    write_u8(0);
    write_u32(size);
    write_u32(static_cast<uint32_t>(payload.size()));
    write_bytes(payload);
}

}  // namespace vnxfer
