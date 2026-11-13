#include "core/ft/backend.h"

#include <cstring>
#include <stdexcept>

#include "core/rfb_protocol.h"

namespace vnxfer::ft {

// TightVNC 1.3 uses the 3.7t extension message space: 130..136, u16 length fields.
// A list request with an empty path means "the ftproot directory" on the server.
ProbeResult probe_tightvnc13(RfbClient& client) {
    ProbeResult result;
    result.kind = Kind::None;

    // The server prepends its ftproot verbatim, and rejects an empty path, so "/"
    // means "list the ftproot directory".
    const std::string path = "/";
    client.write_u8(rfb::kTv13FileListRequest);
    client.write_u8(1);  // flags
    client.write_u16(static_cast<uint16_t>(path.size()));
    client.write_bytes(path.data(), path.size());

    uint8_t type = client.read_u8(3000);
    if (type != rfb::kTv13FileListData) {
        result.detail = "unexpected reply type " + std::to_string(type);
        return result;
    }
    (void)client.read_u8();               // flags
    uint16_t num_files = client.read_u16();
    uint16_t data_size = client.read_u16();
    uint16_t compressed_size = client.read_u16();

    auto size_data = client.read_bytes(static_cast<std::size_t>(num_files) * 8u);
    auto names = client.read_bytes(data_size);

    result.kind = Kind::TightVnc13;
    result.detail = std::to_string(num_files) + " entries, dataSize=" +
                    std::to_string(data_size) + " compressedSize=" +
                    std::to_string(compressed_size);

    std::size_t offset = 0;
    for (uint16_t i = 0; i < num_files && offset < names.size(); ++i) {
        const char* s = reinterpret_cast<const char*>(names.data() + offset);
        std::size_t len = ::strnlen(s, names.size() - offset);
        result.roots.emplace_back(s, len);
        offset += len + 1;
    }
    return result;
}

}  // namespace vnxfer::ft
