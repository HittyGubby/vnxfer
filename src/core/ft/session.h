#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

#include "core/rfb_client.h"

namespace vnxfer {

struct RemoteEntry {
    std::string name;
    bool is_dir = false;
    bool executable = false;
    uint64_t size = 0;
    int64_t mtime = 0;  // unix seconds, 0 if unknown
};

using ProgressFn = std::function<void(uint64_t done, uint64_t total)>;

class FtSession {
public:
    virtual ~FtSession() = default;

    virtual std::string describe() const = 0;
    virtual std::string default_root() const = 0;
    virtual std::vector<RemoteEntry> list(const std::string& path) = 0;
    virtual void download(const std::string& remote, const std::string& local,
                          const ProgressFn& progress = {}) = 0;
    virtual void upload(const std::string& local, const std::string& remote, bool overwrite,
                        const ProgressFn& progress = {}) = 0;
    virtual void mkdir(const std::string& path) = 0;
    virtual void remove(const std::string& path) = 0;
    virtual void rename(const std::string& from, const std::string& to) = 0;

    virtual bool supports_resume() const { return false; }
    // Cheap round-trip used as a keepalive; no-op where the protocol has none.
    virtual void ping() {}
    // Lowercase hex MD5 of a remote chunk; throws when unsupported.
    virtual std::string md5(const std::string&, uint64_t, uint64_t) {
        throw std::runtime_error("md5 is not supported by this protocol");
    }
    virtual std::string capabilities() const { return {}; }
    virtual void download_resume(const std::string& remote, const std::string& local,
                                 uint64_t offset, const ProgressFn& progress) {
        (void)offset;
        download(remote, local, progress);
    }
    virtual void upload_resume(const std::string& local, const std::string& remote,
                               bool overwrite, uint64_t offset, const ProgressFn& progress) {
        (void)offset;
        upload(local, remote, overwrite, progress);
    }
};

struct TransferStats {
    uint64_t files = 0;
    uint64_t bytes = 0;
};

// Deletes a remote file or a whole directory tree (depth-first).
void remove_tree(FtSession& session, const std::string& remote_path);
void download_tree(FtSession& session, const std::string& remote_dir,
                   const std::string& local_dir, const ProgressFn& progress,
                   TransferStats& stats);
void upload_tree(FtSession& session, const std::string& local_dir,
                 const std::string& remote_dir, bool overwrite, const ProgressFn& progress,
                 TransferStats& stats);

std::unique_ptr<FtSession> make_ultravnc_session(RfbClient&& client);
std::unique_ptr<FtSession> make_tightvnc2_session(RfbClient&& client);
std::unique_ptr<FtSession> make_tightvnc13_session(RfbClient&& client);

}  // namespace vnxfer
