#include "core/ft/backend.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <stdexcept>

#include "core/rfb_protocol.h"

#include <zlib.h>

namespace vnxfer::ft {

namespace {
std::vector<uint8_t> read_block(RfbClient& client, uint8_t level, uint32_t compressed_size,
                                uint32_t uncompressed_size) {
    auto data = client.read_bytes(compressed_size);
    if (level == 0 || compressed_size == uncompressed_size) return data;

    std::vector<uint8_t> out(uncompressed_size);
    // The server uses deflate(Z_SYNC_FLUSH) without finishing the stream, so
    // there is no Adler-32 trailer: accept a full output buffer, not Z_STREAM_END.
    {
        z_stream zs{};
        for (int window_bits : {15, -15}) {
            std::memset(&zs, 0, sizeof(zs));
            if (inflateInit2(&zs, window_bits) != Z_OK) continue;
            zs.next_in = data.data();
            zs.avail_in = compressed_size;
            zs.next_out = out.data();
            zs.avail_out = uncompressed_size;
            int rc = inflate(&zs, Z_SYNC_FLUSH);
            int guard = 0;
            while ((rc == Z_OK || rc == Z_BUF_ERROR) && zs.total_out < uncompressed_size &&
                   guard++ < 64) {
                rc = inflate(&zs, Z_SYNC_FLUSH);
            }
            const bool complete = zs.total_out == uncompressed_size;
            inflateEnd(&zs);
            if (complete) return out;
        }
    }
    char diag[160];
    std::snprintf(diag, sizeof(diag),
                  "failed to inflate TightVNC 2.x block (level=%u csize=%u usize=%u first=%02x%02x%02x%02x)",
                  level, compressed_size, uncompressed_size,
                  compressed_size > 0 ? data[0] : 0, compressed_size > 1 ? data[1] : 0,
                  compressed_size > 2 ? data[2] : 0, compressed_size > 3 ? data[3] : 0);
    throw std::runtime_error(diag);
}

std::string read_utf8(const std::vector<uint8_t>& buf, std::size_t& offset) {
    if (offset + 4 > buf.size()) throw std::runtime_error("truncated UTF8 length");
    uint32_t len = static_cast<uint32_t>(buf[offset]) << 24 |
                   static_cast<uint32_t>(buf[offset + 1]) << 16 |
                   static_cast<uint32_t>(buf[offset + 2]) << 8 | buf[offset + 3];
    offset += 4;
    if (offset + len > buf.size()) throw std::runtime_error("truncated UTF8 body");
    std::string s(reinterpret_cast<const char*>(&buf[offset]), len);
    offset += len;
    return s;
}
}  // namespace

ProbeResult probe_tightvnc2(RfbClient& client) {
    ProbeResult result;
    result.kind = Kind::None;

    client.write_u32(rfb::kTv2CompressionSupportRequest);
    uint32_t id = client.read_u32(3000);
    if (id != rfb::kTv2CompressionSupportReply) {
        result.detail = "unexpected reply id to compression probe";
        return result;
    }
    uint8_t compression = client.read_u8();
    result.compression = compression == 1;

    // List roots ("/" gives drive letters on Windows, a root listing on Unix).
    std::vector<uint8_t> payload;
    payload.push_back(compression ? 1 : 0);
    const std::string path = "/";
    payload.push_back(static_cast<uint8_t>(path.size() >> 24));
    payload.push_back(static_cast<uint8_t>(path.size() >> 16));
    payload.push_back(static_cast<uint8_t>(path.size() >> 8));
    payload.push_back(static_cast<uint8_t>(path.size()));
    payload.insert(payload.end(), path.begin(), path.end());

    client.write_u32(rfb::kTv2FileListRequest);
    client.write_bytes(payload);

    id = client.read_u32(3000);
    if (id != rfb::kTv2FileListReply) {
        result.detail = "file list request was not answered";
        return result;
    }
    uint8_t level = client.read_u8();
    uint32_t csize = client.read_u32();
    uint32_t usize = client.read_u32();
    auto block = read_block(client, level, csize, usize);

    result.kind = Kind::TightVnc2;
    result.detail = "compression " + std::string(result.compression ? "on" : "off");
    if (block.size() >= 4) {
        std::size_t offset = 0;
        uint32_t count = static_cast<uint32_t>(block[0]) << 24 |
                         static_cast<uint32_t>(block[1]) << 16 |
                         static_cast<uint32_t>(block[2]) << 8 | block[3];
        offset = 4;
        for (uint32_t i = 0; i < count && offset + 18 <= block.size(); ++i) {
            offset += 16;  // size + mtime
            uint16_t flags = static_cast<uint16_t>(block[offset] << 8 | block[offset + 1]);
            offset += 2;
            std::string name = read_utf8(block, offset);
            (void)flags;
            name.erase(std::remove(name.begin(), name.end(), '\0'), name.end());
            result.roots.push_back(std::move(name));
        }
    }
    return result;
}

}  // namespace vnxfer::ft
