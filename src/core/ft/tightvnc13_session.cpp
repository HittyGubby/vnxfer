#include <algorithm>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <fstream>
#include <stdexcept>

#include "core/ft/session.h"
#include "core/rfb_protocol.h"

namespace vnxfer {

namespace {

// TightVNC 1.3 uses a ftproot prefix: the server concatenates ftproot + path.
// "/" therefore means "the ftproot directory" (usually $HOME).
class TightVnc13Session final : public FtSession {
public:
    explicit TightVnc13Session(RfbClient&& client) : client_(std::move(client)) {}

    std::string describe() const override { return "tightvnc1.3"; }
    std::string default_root() const override { return "/"; }

    std::vector<RemoteEntry> list(const std::string& path) override {
        const std::string wire = normalize(path);
        client_.write_u8(rfb::kTv13FileListRequest);
        client_.write_u8(1);
        client_.write_u16(static_cast<uint16_t>(wire.size()));
        client_.write_bytes(wire.data(), wire.size());

        uint8_t type = client_.read_u8(15000);
        if (type != rfb::kTv13FileListData) {
            throw std::runtime_error("tightvnc1.3: unexpected reply to list (" +
                                     std::to_string(type) + ")");
        }
        (void)client_.read_u8();
        uint16_t num_files = client_.read_u16();
        uint16_t data_size = client_.read_u16();
        (void)client_.read_u16();  // compressedSize

        auto sizes = client_.read_bytes(static_cast<std::size_t>(num_files) * 8u);
        auto names = client_.read_bytes(data_size);

        std::vector<RemoteEntry> entries;
        std::size_t name_offset = 0;
        for (uint16_t i = 0; i < num_files; ++i) {
            RemoteEntry entry;
            const uint8_t* p = sizes.data() + static_cast<std::size_t>(i) * 8;
            const uint32_t raw_size = be32(p);
            entry.mtime = static_cast<int64_t>(be32(p + 4));
            if (raw_size == 0xFFFFFFFFu) {  // the server encodes directories as -1
                entry.is_dir = true;
            } else {
                entry.size = raw_size;
            }
            if (name_offset >= names.size()) break;
            const char* s = reinterpret_cast<const char*>(names.data() + name_offset);
            std::size_t len = ::strnlen(s, names.size() - name_offset);
            entry.name.assign(s, len);
            name_offset += len + 1;
            entries.push_back(std::move(entry));
        }
        return entries;
    }

    void download(const std::string& remote, const std::string& local,
                  const ProgressFn& progress) override {
        const std::string wire = normalize(remote);
        client_.write_u8(rfb::kTv13FileDownloadRequest);
        client_.write_u8(0);  // no compression
        client_.write_u16(static_cast<uint16_t>(wire.size()));
        client_.write_u32(0);  // offset
        client_.write_bytes(wire.data(), wire.size());

        std::ofstream out(local, std::ios::binary | std::ios::trunc);
        if (!out) throw std::runtime_error("cannot open local file " + local);

        uint64_t done = 0;
        while (true) {
            uint8_t type = client_.read_u8(30000);
            if (type == rfb::kTv13FileDownloadFailed) {
                (void)client_.read_u8();
                uint16_t reason_len = client_.read_u16();
                auto reason = client_.read_bytes(reason_len);
                throw std::runtime_error("download failed: " +
                                         std::string(reason.begin(), reason.end()));
            }
            if (type != rfb::kTv13FileDownloadData) {
                throw std::runtime_error("tightvnc1.3: unexpected download reply " +
                                         std::to_string(type));
            }
            uint8_t level = client_.read_u8();
            uint16_t real_size = client_.read_u16();
            uint16_t compressed_size = client_.read_u16();
            if (real_size == 0 && compressed_size == 0) {
                (void)client_.read_u32();  // mtime
                break;
            }
            if (level != 0) throw std::runtime_error("tightvnc1.3: compressed blocks unsupported");
            auto data = client_.read_bytes(compressed_size);
            out.write(reinterpret_cast<const char*>(data.data()),
                      static_cast<std::streamsize>(data.size()));
            done += data.size();
            if (progress) progress(done, 0);
        }
        out.close();
        if (progress) progress(done, done);
    }

    void upload(const std::string& local, const std::string& remote, bool overwrite,
                const ProgressFn& progress) override {
        (void)overwrite;  // 1.3 always truncates
        std::ifstream in(local, std::ios::binary);
        if (!in) throw std::runtime_error("cannot open local file " + local);
        in.seekg(0, std::ios::end);
        const uint64_t total = static_cast<uint64_t>(in.tellg());
        in.seekg(0, std::ios::beg);

        const std::string wire = normalize(remote);
        client_.write_u8(rfb::kTv13FileUploadRequest);
        client_.write_u8(0);
        client_.write_u16(static_cast<uint16_t>(wire.size()));
        client_.write_u32(0);
        client_.write_bytes(wire.data(), wire.size());

        std::array<char, 32768> buf{};
        uint64_t done = 0;
        while (in) {
            in.read(buf.data(), static_cast<std::streamsize>(buf.size()));
            std::streamsize got = in.gcount();
            if (got <= 0) break;
            client_.write_u8(rfb::kTv13FileUploadData);
            client_.write_u8(0);
            client_.write_u16(static_cast<uint16_t>(got));
            client_.write_u16(static_cast<uint16_t>(got));
            client_.write_bytes(buf.data(), static_cast<std::size_t>(got));
            done += static_cast<uint64_t>(got);
            if (progress) progress(done, total);
        }
        client_.write_u8(rfb::kTv13FileUploadData);
        client_.write_u8(0);
        client_.write_u16(0);
        client_.write_u16(0);
        client_.write_u32(static_cast<uint32_t>(std::time(nullptr)));
        sync();
        if (progress) progress(done, done);
    }

    void mkdir(const std::string& path) override {
        const std::string wire = normalize(path);
        client_.write_u8(rfb::kTv13FileCreateDirRequest);
        client_.write_u8(0);
        client_.write_u16(static_cast<uint16_t>(wire.size()));
        client_.write_bytes(wire.data(), wire.size());
        sync();
    }

    void remove(const std::string&) override {
        throw std::runtime_error("tightvnc1.3 has no delete message");
    }
    void rename(const std::string&, const std::string&) override {
        throw std::runtime_error("tightvnc1.3 has no rename message");
    }

private:
    static uint32_t be32(const uint8_t* p) {
        return static_cast<uint32_t>(p[0]) << 24 | static_cast<uint32_t>(p[1]) << 16 |
               static_cast<uint32_t>(p[2]) << 8 | p[3];
    }

    static std::string normalize(const std::string& path) {
        if (path.empty()) return "/";
        if (path[0] == '/') return path;
        return "/" + path;
    }

    // The server processes messages in order but has no upload/ mkdir ack, so
    // round-trip a list request and drain it before closing the socket.
    void sync() {
        client_.write_u8(rfb::kTv13FileListRequest);
        client_.write_u8(1);
        client_.write_u16(1);
        client_.write_bytes("/", 1);

        uint8_t type = client_.read_u8(15000);
        if (type == rfb::kTv13FileDownloadFailed || type == rfb::kTv13FileUploadFailed) {
            (void)client_.read_u8();
            uint16_t len = client_.read_u16();
            auto reason = client_.read_bytes(len);
            throw std::runtime_error(std::string(reason.begin(), reason.end()));
        }
        if (type != rfb::kTv13FileListData) return;
        (void)client_.read_u8();
        uint16_t num_files = client_.read_u16();
        uint16_t data_size = client_.read_u16();
        (void)client_.read_u16();
        client_.read_bytes(static_cast<std::size_t>(num_files) * 8u);
        client_.read_bytes(data_size);
    }

    RfbClient client_;
};

}  // namespace

std::unique_ptr<FtSession> make_tightvnc13_session(RfbClient&& client) {
    return std::make_unique<TightVnc13Session>(std::move(client));
}

}  // namespace vnxfer
