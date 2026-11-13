#include <algorithm>
#include <cstring>
#include <ctime>
#include <fstream>
#include <stdexcept>

#include <zlib.h>

#include "core/ft/session.h"
#include "core/rfb_protocol.h"

namespace vnxfer {

namespace {

uint32_t le32(const uint8_t* p) {
    return static_cast<uint32_t>(p[0]) | static_cast<uint32_t>(p[1]) << 8 |
           static_cast<uint32_t>(p[2]) << 16 | static_cast<uint32_t>(p[3]) << 24;
}

std::string time_suffix() {
    std::time_t now = std::time(nullptr);
    std::tm tm{};
    localtime_r(&now, &tm);
    char buf[32];
    std::strftime(buf, sizeof(buf), "%m/%d/%Y %H:%M", &tm);
    return buf;
}

std::vector<uint8_t> inflate_all(const std::vector<uint8_t>& in) {
    z_stream zs{};
    if (inflateInit(&zs) != Z_OK) throw std::runtime_error("zlib init failed");
    zs.next_in = const_cast<Bytef*>(in.data());
    zs.avail_in = static_cast<uInt>(in.size());

    std::vector<uint8_t> out;
    std::array<uint8_t, 16384> chunk{};
    int rc = Z_OK;
    while (rc == Z_OK) {
        zs.next_out = chunk.data();
        zs.avail_out = static_cast<uInt>(chunk.size());
        rc = inflate(&zs, Z_NO_FLUSH);
        std::size_t produced = chunk.size() - zs.avail_out;
        out.insert(out.end(), chunk.begin(), chunk.begin() + static_cast<std::ptrdiff_t>(produced));
    }
    inflateEnd(&zs);
    if (rc != Z_STREAM_END) throw std::runtime_error("corrupt compressed file packet");
    return out;
}

// libvncserver maps "C:/x" to "/x" on Unix and keeps drive letters on Windows.
std::string to_wire_path(const std::string& path) {
    if (path.rfind("C:", 0) == 0) return path;
    if (!path.empty() && path[0] == '/') return "C:" + path;
    return "C:/" + path;
}

class UltraVncSession final : public FtSession {
public:
    explicit UltraVncSession(RfbClient&& client) : client_(std::move(client)) {}

    std::string describe() const override { return "ultravnc"; }
    std::string default_root() const override { return "/"; }

    std::vector<RemoteEntry> list(const std::string& path) override {
        client_.send_ft(rfb::kFtDirContentRequest, rfb::kFtRDirContent, 0,
                        bytes(to_wire_path(path)));

        std::vector<RemoteEntry> entries;
        FtMessage msg;
        while (client_.read_ft_message(msg, timeout_ms_)) {
            if (msg.ctype != rfb::kFtDirPacket) continue;
            if (msg.cparam == 0 && msg.length == 0) break;
            if (msg.length < 44) continue;  // directory echo
            RemoteEntry entry;
            const uint8_t* p = msg.payload.data();
            uint32_t attrs = le32(p);
            entry.is_dir = (attrs & 0x10u) != 0;
            entry.mtime = static_cast<int64_t>(le32(p + 20));
            entry.size = static_cast<uint64_t>(le32(p + 32)) |
                         static_cast<uint64_t>(le32(p + 28)) << 32;
            const char* name = reinterpret_cast<const char*>(p + 44);
            std::size_t max_len = msg.length - 44;
            entry.name.assign(name, ::strnlen(name, max_len));
            entries.push_back(std::move(entry));
        }
        return entries;
    }

    void download(const std::string& remote, const std::string& local,
                  const ProgressFn& progress) override {
        const std::string wire = to_wire_path(remote);
        client_.send_ft(rfb::kFtFileTransferRequest, 0, 1, bytes(wire));  // 1 = offer compression

        FtMessage msg;
        if (!client_.read_ft_message(msg, timeout_ms_) || msg.ctype != rfb::kFtFileHeader) {
            throw std::runtime_error("download: no file header for " + remote);
        }
        const uint32_t size = msg.size;
        uint32_t size_h = client_.read_u32();
        if (size == 0xFFFFFFFFu) {
            throw std::runtime_error("download: server cannot open " + remote);
        }
        const uint64_t total = static_cast<uint64_t>(size) | static_cast<uint64_t>(size_h) << 32;

        // Tell the server we are ready; it only starts streaming after this.
        client_.send_ft(rfb::kFtFileHeader, 0, 0, {});

        std::ofstream out(local, std::ios::binary | std::ios::trunc);
        if (!out) throw std::runtime_error("cannot open local file " + local);

        uint64_t done = 0;
        while (true) {
            if (!client_.read_ft_message(msg, 60000)) {
                throw std::runtime_error("download: timed out waiting for data");
            }
            if (msg.ctype == rfb::kFtFilePacket) {
                std::vector<uint8_t> data = msg.payload;
                if (msg.size == 1) data = inflate_all(data);
                out.write(reinterpret_cast<const char*>(data.data()),
                          static_cast<std::streamsize>(data.size()));
                done += data.size();
                if (progress) progress(done, total);
            } else if (msg.ctype == rfb::kFtEndOfFile) {
                break;
            } else if (msg.ctype == rfb::kFtAbortFileTransfer) {
                throw std::runtime_error("download aborted by server");
            }
        }
        out.close();
        if (progress) progress(done, total ? total : done);
    }

    void upload(const std::string& local, const std::string& remote, bool overwrite,
                const ProgressFn& progress) override {
        std::ifstream in(local, std::ios::binary);
        if (!in) throw std::runtime_error("cannot open local file " + local);
        in.seekg(0, std::ios::end);
        const uint64_t total = static_cast<uint64_t>(in.tellg());
        in.seekg(0, std::ios::beg);
        if (total > 0xFFFFFFFFull) throw std::runtime_error("ultravnc: files > 4 GiB unsupported");

        const std::string wire = to_wire_path(remote);
        client_.send_ft(rfb::kFtFileTransferOffer, 0, static_cast<uint32_t>(total),
                        bytes(wire + "," + time_suffix()));
        client_.write_u32(0);  // sizeH

        FtMessage msg;
        if (!client_.read_ft_message(msg, 30000) || msg.ctype != rfb::kFtFileAcceptHeader) {
            throw std::runtime_error("upload: no accept header");
        }
        if (msg.size == 0xFFFFFFFFu) {
            throw std::runtime_error("upload: server refused " + remote);
        }

        std::array<char, 8192> buf{};
        uint64_t done = 0;
        while (in) {
            in.read(buf.data(), static_cast<std::streamsize>(buf.size()));
            std::streamsize got = in.gcount();
            if (got <= 0) break;
            client_.send_ft(rfb::kFtFilePacket, 0, 0,
                            std::vector<uint8_t>(buf.begin(), buf.begin() + got));
            done += static_cast<uint64_t>(got);
            if (progress) progress(done, total);
        }
        client_.send_ft(rfb::kFtEndOfFile, 0, 0, {});

        // There is no explicit end-of-upload acknowledgement: round-trip a cheap
        // request so we know the server has consumed the packets and closed the file.
        client_.send_ft(rfb::kFtDirContentRequest, rfb::kFtRDrivesList, 0, {});
        FtMessage ack;
        (void)client_.read_ft_message(ack, 10000);
    }

    void mkdir(const std::string& path) override {
        command(1, path, 4, "mkdir");
    }
    void remove(const std::string& path) override { command(4, path, 7, "remove"); }

    void rename(const std::string& from, const std::string& to) override {
        // libvncserver splits the payload on the last '*'.
        const std::string wire = to_wire_path(from) + "*" + to_wire_path(to);
        client_.send_ft(rfb::kFtCommand, 5, 0, bytes(wire));
        FtMessage msg;
        if (!client_.read_ft_message(msg, 15000) || msg.ctype != rfb::kFtCommandReturn ||
            msg.cparam != 8 || msg.size != 0) {
            throw std::runtime_error("rename failed");
        }
    }

private:
    static std::vector<uint8_t> bytes(const std::string& s) {
        return std::vector<uint8_t>(s.begin(), s.end());
    }

    void command(uint8_t param, const std::string& payload, uint8_t expected, const char* what) {
        const std::string wire = to_wire_path(payload);
        client_.send_ft(rfb::kFtCommand, param, 0, bytes(wire));
        FtMessage msg;
        if (!client_.read_ft_message(msg, 15000) || msg.ctype != rfb::kFtCommandReturn) {
            throw std::runtime_error(std::string(what) + ": no command reply");
        }
        if (msg.cparam != expected || msg.size != 0) {
            throw std::runtime_error(std::string(what) + ": server returned " +
                                     std::to_string(msg.size));
        }
    }

    RfbClient client_;
    int timeout_ms_ = 15000;
};

}  // namespace

std::unique_ptr<FtSession> make_ultravnc_session(RfbClient&& client) {
    return std::make_unique<UltraVncSession>(std::move(client));
}

}  // namespace vnxfer
