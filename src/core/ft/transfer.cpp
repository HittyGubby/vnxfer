#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <stdexcept>

#include "core/ft/session.h"

namespace vnxfer {

namespace {

std::string join_remote(const std::string& dir, const std::string& name) {
    if (dir.empty()) return "/" + name;
    if (dir.back() == '/') return dir + name;
    return dir + "/" + name;
}

struct RemoteFile {
    std::string path;
    uint64_t size = 0;
};

void collect_remote(FtSession& session, const std::string& dir, std::vector<RemoteFile>& out,
                    uint64_t& total) {
    for (const auto& entry : session.list(dir)) {
        if (entry.name == "." || entry.name == "..") continue;
        const std::string path = join_remote(dir, entry.name);
        if (entry.is_dir) {
            collect_remote(session, path, out, total);
        } else {
            out.push_back({path, entry.size});
            total += entry.size;
        }
    }
}

class Ticker {
public:
    Ticker(const ProgressFn& progress, uint64_t total) : progress_(progress), total_(total) {}

    void begin_file() { base_ = done_; }
    void at(uint64_t file_done) {
        done_ = base_ + file_done;
        if (progress_) progress_(done_, total_);
    }
    void finish() {
        if (progress_ && total_ == 0) progress_(done_, done_);
    }

private:
    const ProgressFn& progress_;
    uint64_t total_;
    uint64_t base_ = 0;
    uint64_t done_ = 0;
};

}  // namespace

void download_tree(FtSession& session, const std::string& remote_dir,
                   const std::string& local_dir, const ProgressFn& progress,
                   TransferStats& stats) {
    std::vector<RemoteFile> files;
    uint64_t total = 0;
    collect_remote(session, remote_dir, files, total);

    std::filesystem::create_directories(local_dir);
    Ticker ticker(progress, total);
    const std::size_t prefix = remote_dir.size() + (remote_dir.back() == '/' ? 0 : 1);

    for (const auto& file : files) {
        auto local_path = std::filesystem::path(local_dir) / file.path.substr(prefix);
        std::filesystem::create_directories(local_path.parent_path());
        ticker.begin_file();
        session.download(file.path, local_path.string(),
                         [&ticker](uint64_t done, uint64_t) { ticker.at(done); });
        stats.files += 1;
        stats.bytes += file.size;
    }
    ticker.finish();
}

void upload_tree(FtSession& session, const std::string& local_dir,
                 const std::string& remote_dir, bool overwrite, const ProgressFn& progress,
                 TransferStats& stats) {
    namespace fs = std::filesystem;
    if (!fs::is_directory(local_dir)) throw std::runtime_error("not a directory: " + local_dir);

    uint64_t total = 0;
    for (const auto& entry : fs::recursive_directory_iterator(local_dir)) {
        if (entry.is_regular_file()) total += entry.file_size();
    }
    Ticker ticker(progress, total);

    // Create the remote tree first so files always have a destination.
    try {
        session.mkdir(remote_dir);
    } catch (const std::exception&) {
    }
    for (const auto& entry : fs::recursive_directory_iterator(local_dir)) {
        if (!entry.is_directory()) continue;
        fs::path rel = fs::relative(entry.path(), local_dir);
        // Existing directories answer with an error on every backend; ignore it.
        try {
            session.mkdir(join_remote(remote_dir, rel.generic_string()));
        } catch (const std::exception&) {
        }
    }

    for (const auto& entry : fs::recursive_directory_iterator(local_dir)) {
        if (!entry.is_regular_file()) continue;
        fs::path rel = fs::relative(entry.path(), local_dir);
        const std::string remote = join_remote(remote_dir, rel.generic_string());
        ticker.begin_file();
        session.upload(entry.path().string(), remote, overwrite,
                       [&ticker](uint64_t done, uint64_t) { ticker.at(done); });
        stats.files += 1;
        stats.bytes += entry.file_size();
    }
    ticker.finish();
}

}  // namespace vnxfer
