#include <algorithm>
#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <fstream>
#include <stdexcept>

#include <zlib.h>

#include "core/ft/session.h"
#include "core/rfb_protocol.h"

namespace vnxfer {

namespace {

class TightVnc2Session final : public FtSession {
public:
    explicit TightVnc2Session(RfbClient&& client) : client_(std::move(client)) {
        // The server keeps one deflate stream per connection and uses
        // Z_SYNC_FLUSH without finishing it, so the client must keep a single
        // inflate stream alive for the whole session.
        std::memset(&inflater_, 0, sizeof(inflater_));
        inflater_ok_ = inflateInit2(&inflater_, 15) == Z_OK;
    }

    ~TightVnc2Session() override {
        if (inflater_ok_) inflateEnd(&inflater_);
    }

    std::string describe() const override { return "tightvnc2"; }
    std::string default_root() const override { return "/"; }

    std::vector<RemoteEntry> list(const std::string& path) override {
        client_.write_u32(rfb::kTv2FileListRequest);
        client_.write_u8(compression_ ? 1 : 0);
        write_utf8(path);

        expect(rfb::kTv2FileListReply);
        uint8_t level = client_.read_u8();
        uint32_t csize = client_.read_u32();
        uint32_t usize = client_.read_u32();
        auto data = read_block(level, csize, usize);

        std::vector<RemoteEntry> entries;
        if (data.size() < 4) return entries;
        uint32_t count = be32(data.data());
        std::size_t offset = 4;
        for (uint32_t i = 0; i < count && offset + 22 <= data.size(); ++i) {
            RemoteEntry entry;
            entry.size = be64(data.data() + offset);
            offset += 8;
            entry.mtime = static_cast<int64_t>(be64(data.data() + offset) / 1000);
            offset += 8;
            uint16_t flags = static_cast<uint16_t>(data[offset] << 8 | data[offset + 1]);
            offset += 2;
            entry.is_dir = (flags & 0x1) != 0;
            entry.executable = (flags & 0x2) != 0;
            entry.name = read_utf8(data, offset);
            // The Windows server appends a NUL inside the UTF8 string; leaving it
            // in makes path concatenation produce embedded NULs.
            entry.name.erase(std::remove(entry.name.begin(), entry.name.end(), '\0'),
                             entry.name.end());
            entries.push_back(std::move(entry));
        }
        return entries;
    }

    bool supports_resume() const override { return true; }

    void ping() override {
        // TightVNC 4.0+ advertises Echo; 2.x does not (sending it would drop the
        // connection), so fall back to a harmless root listing.
        if (client_.caps().hasClient(0xFC000300)) {
            client_.write_u32(0xFC000300);
            const uint32_t reply = client_.read_u32(5000);
            if (reply != 0xFC000300) throw std::runtime_error("keepalive failed");
            return;
        }
        client_.write_u32(rfb::kTv2FileListRequest);
        client_.write_u8(compression_ ? 1 : 0);
        write_utf8("/");
        expect(rfb::kTv2FileListReply);
        uint8_t level = client_.read_u8();
        uint32_t csize = client_.read_u32();
        uint32_t usize = client_.read_u32();
        (void)read_block(level, csize, usize);
    }

    std::string md5(const std::string& path, uint64_t offset, uint64_t size) override {
        client_.write_u32(rfb::kTv2Md5Request);
        write_utf8(path);
        client_.write_u64(offset);
        client_.write_u64(size);
        expect(rfb::kTv2Md5Reply);
        auto digest = client_.read_bytes(16);
        static const char* hex = "0123456789abcdef";
        std::string out;
        for (uint8_t b : digest) {
            out.push_back(hex[b >> 4]);
            out.push_back(hex[b & 0xF]);
        }
        return out;
    }

    std::string capabilities() const override {
        return "TightVNC 2.x (0xFC0001xx): list, download, upload, mkdir, rm, rename, "
               "dirsize, md5, resume, compression";
    }

    void download(const std::string& remote, const std::string& local,
                  const ProgressFn& progress) override {
        download_impl(remote, local, 0, progress);
    }

    void download_resume(const std::string& remote, const std::string& local, uint64_t offset,
                         const ProgressFn& progress) override {
        download_impl(remote, local, offset, progress);
    }

    void upload(const std::string& local, const std::string& remote, bool overwrite,
                const ProgressFn& progress) override {
        upload_impl(local, remote, overwrite, 0, progress);
    }

    void upload_resume(const std::string& local, const std::string& remote, bool overwrite,
                       uint64_t offset, const ProgressFn& progress) override {
        upload_impl(local, remote, offset > 0 ? false : overwrite, offset, progress);
    }

    void mkdir(const std::string& path) override {
        client_.write_u32(rfb::kTv2MkdirRequest);
        write_utf8(path);
        expect(rfb::kTv2MkdirReply);
    }

    void remove(const std::string& path) override {
        client_.write_u32(rfb::kTv2RemoveRequest);
        write_utf8(path);
        expect(rfb::kTv2RemoveReply);
    }

    void rename(const std::string& from, const std::string& to) override {
        client_.write_u32(rfb::kTv2RenameRequest);
        write_utf8(from);
        write_utf8(to);
        expect(rfb::kTv2RenameReply);
    }

private:
    void download_impl(const std::string& remote, const std::string& local, uint64_t offset,
                       const ProgressFn& progress) {
        client_.write_u32(rfb::kTv2DownloadStartRequest);
        write_utf8(remote);
        client_.write_u64(offset);
        expect(rfb::kTv2DownloadStartReply);

        std::ofstream out(local, std::ios::binary |
                                   (offset > 0 ? std::ios::app : std::ios::trunc));
        if (!out) throw std::runtime_error("cannot open local file " + local);

        constexpr uint32_t kChunk = 65536;
        uint64_t done = 0;
        while (true) {
            client_.write_u32(rfb::kTv2DownloadDataRequest);
            client_.write_u8(compression_ ? 1 : 0);
            client_.write_u32(kChunk);

            uint32_t id = expect_any({rfb::kTv2DownloadDataReply, rfb::kTv2DownloadEndReply});
            if (id == rfb::kTv2DownloadEndReply) {
                (void)client_.read_u8();
                (void)client_.read_u64();
                break;
            }
            uint8_t level = client_.read_u8();
            uint32_t csize = client_.read_u32();
            uint32_t usize = client_.read_u32();
            auto data = read_block(level, csize, usize);
            out.write(reinterpret_cast<const char*>(data.data()),
                      static_cast<std::streamsize>(data.size()));
            done += data.size();
            if (progress) progress(offset + done, 0);
        }
        out.close();
        if (progress) progress(offset + done, offset + done);
    }

    void upload_impl(const std::string& local, const std::string& remote, bool overwrite,
                     uint64_t offset, const ProgressFn& progress) {
        std::ifstream in(local, std::ios::binary);
        if (!in) throw std::runtime_error("cannot open local file " + local);
        in.seekg(static_cast<std::streamoff>(offset), std::ios::beg);

        client_.write_u32(rfb::kTv2UploadStartRequest);
        write_utf8(remote);
        client_.write_u8(overwrite ? 1 : 0);
        client_.write_u64(offset);
        expect(rfb::kTv2UploadStartReply);

        std::array<char, 65536> buf{};
        uint64_t done = 0;
        while (in) {
            in.read(buf.data(), static_cast<std::streamsize>(buf.size()));
            std::streamsize got = in.gcount();
            if (got <= 0) break;

            client_.write_u32(rfb::kTv2UploadDataRequest);
            client_.write_u8(0);  // no compression
            client_.write_u32(static_cast<uint32_t>(got));
            client_.write_u32(static_cast<uint32_t>(got));
            client_.write_bytes(buf.data(), static_cast<std::size_t>(got));
            expect(rfb::kTv2UploadDataReply);
            done += static_cast<uint64_t>(got);
            if (progress) progress(offset + done, 0);
        }

        client_.write_u32(rfb::kTv2UploadEndRequest);
        client_.write_u16(0);
        client_.write_u64(static_cast<uint64_t>(std::time(nullptr)) * 1000);
        expect(rfb::kTv2UploadEndReply);
        if (progress) progress(offset + done, offset + done);
    }

    static uint32_t be32(const uint8_t* p) {
        return static_cast<uint32_t>(p[0]) << 24 | static_cast<uint32_t>(p[1]) << 16 |
               static_cast<uint32_t>(p[2]) << 8 | p[3];
    }
    static uint64_t be64(const uint8_t* p) {
        return static_cast<uint64_t>(be32(p)) << 32 | be32(p + 4);
    }

    void write_utf8(const std::string& s) {
        client_.write_u32(static_cast<uint32_t>(s.size()));
        client_.write_bytes(s.data(), s.size());
    }

    std::string read_utf8(const std::vector<uint8_t>& buf, std::size_t& offset) {
        if (offset + 4 > buf.size()) throw std::runtime_error("truncated utf8 length");
        uint32_t len = be32(buf.data() + offset);
        offset += 4;
        if (offset + len > buf.size()) throw std::runtime_error("truncated utf8 body");
        std::string s(reinterpret_cast<const char*>(buf.data() + offset), len);
        offset += len;
        return s;
    }

    std::vector<uint8_t> read_block(uint8_t level, uint32_t csize, uint32_t usize) {
        auto data = client_.read_bytes(csize);
        if (level == 0 || csize == 0) return data;

        // Leave headroom so a single pass consumes the whole sync-flush frame;
        // unconsumed input would desynchronise the shared stream for the next block.
        std::vector<uint8_t> out(usize + 64);
        if (!inflater_ok_) throw std::runtime_error("tightvnc2: zlib init failed");
        inflater_.next_in = data.data();
        inflater_.avail_in = csize;
        inflater_.next_out = out.data();
        inflater_.avail_out = static_cast<uInt>(out.size());
        const uLong start = inflater_.total_out;
        int rc = Z_OK;
        int guard = 0;
        while (rc == Z_OK && inflater_.avail_in > 0 && guard++ < 64) {
            rc = inflate(&inflater_, Z_SYNC_FLUSH);
        }
        if (rc == Z_BUF_ERROR) rc = Z_OK;
        if (inflater_.total_out - start != usize) {
            char buf[128];
            std::snprintf(buf, sizeof(buf),
                          "tightvnc2: inflate mismatch (level=%u csize=%u usize=%u got=%lu rc=%d)",
                          level, csize, usize, static_cast<unsigned long>(inflater_.total_out - start),
                          rc);
            throw std::runtime_error(buf);
        }
        out.resize(usize);
        return out;
    }

    void expect(uint32_t expected) {
        uint32_t id = client_.read_u32(30000);
        if (id == rfb::kTv2LastRequestFailedReply) throw server_error();
        if (id != expected) {
            char buf[64];
            std::snprintf(buf, sizeof(buf), "unexpected reply 0x%08x (want 0x%08x)", id, expected);
            throw std::runtime_error(buf);
        }
    }

    uint32_t expect_any(std::initializer_list<uint32_t> ok) {
        uint32_t id = client_.read_u32(60000);
        if (id == rfb::kTv2LastRequestFailedReply) throw server_error();
        for (uint32_t v : ok) {
            if (id == v) return id;
        }
        char buf[64];
        std::snprintf(buf, sizeof(buf), "unexpected reply 0x%08x", id);
        throw std::runtime_error(buf);
    }

    std::runtime_error server_error() {
        uint32_t len = client_.read_u32();
        auto data = client_.read_bytes(len);
        return std::runtime_error("server error: " +
                                  std::string(reinterpret_cast<const char*>(data.data()),
                                              data.size()));
    }

    RfbClient client_;
    bool compression_ = true;
    z_stream inflater_{};
    bool inflater_ok_ = false;
};

}  // namespace

std::unique_ptr<FtSession> make_tightvnc2_session(RfbClient&& client) {
    return std::make_unique<TightVnc2Session>(std::move(client));
}

}  // namespace vnxfer
