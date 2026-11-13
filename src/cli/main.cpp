#include <chrono>
#include <cstdio>
#include <filesystem>
#include <optional>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

#include "core/ft/backend.h"
#include "core/ft/open.h"
#include "core/ft/retry.h"
#include "core/ft/session.h"
#include "core/rfb_client.h"
#include "core/rfb_protocol.h"

namespace {

using namespace vnxfer;

struct Options {
    std::string host = "127.0.0.1";
    uint16_t port = 5900;
    std::string password;
    int timeout_ms = 5000;
    bool prefer_tight = true;
    std::string path;
    std::string remote;
    std::string local;
    std::string from;
    std::string to;
    bool overwrite = true;
    bool recursive = false;
    bool resume = false;
    int retries = 3;
    int retry_delay_ms = 500;
};

void usage() {
    std::fprintf(stderr,
                 "vnxfer - VNC file transfer client\n"
                 "\n"
                 "  vnxfer probe HOST[:PORT] [--password PW]\n"
                 "  vnxfer ls    HOST[:PORT] [--path PATH] [--password PW]\n"
                 "  vnxfer get   HOST[:PORT] --remote PATH --local FILE [--recursive] [--resume] [--password PW]\n"
                 "  vnxfer put   HOST[:PORT] --local FILE --remote PATH [--recursive] [--resume] [--no-overwrite] [--password PW]\n"
                 "  vnxfer mkdir HOST[:PORT] --path PATH\n"
                 "  vnxfer rm    HOST[:PORT] --path PATH\n"
                 "  vnxfer mv    HOST[:PORT] --from A --to B\n"
                 "\n"
                 "Password can also be set via VNXFER_PASSWORD.\n");
}

std::string hex_list(const std::vector<uint8_t>& values) {
    std::string out;
    char buf[8];
    for (std::size_t i = 0; i < values.size(); ++i) {
        std::snprintf(buf, sizeof(buf), "0x%02x", values[i]);
        if (i) out += ' ';
        out += buf;
    }
    return out;
}

std::string join(const std::vector<std::string>& values, const char* sep) {
    std::string out;
    for (std::size_t i = 0; i < values.size(); ++i) {
        if (i) out += sep;
        out += values[i];
    }
    return out;
}

std::string human_size(uint64_t n) {
    const char* units[] = {"B", "K", "M", "G", "T"};
    double v = static_cast<double>(n);
    int u = 0;
    while (v >= 1024.0 && u < 4) {
        v /= 1024.0;
        ++u;
    }
    char buf[32];
    std::snprintf(buf, sizeof(buf), u == 0 ? "%.0f%s" : "%.1f%s", v, units[u]);
    return buf;
}

std::string fmt_time(int64_t t) {
    if (t <= 0) return "-";
    std::time_t tt = static_cast<std::time_t>(t);
    std::tm tm{};
    localtime_r(&tt, &tm);
    char buf[32];
    std::strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M", &tm);
    return buf;
}

void print_basic(const RfbClient& client) {
    const auto& info = client.server_info();
    std::printf("banner:   %s", client.server_version().c_str());
    std::printf("security: %s -> 0x%02x (%s)\n", hex_list(client.security_types()).c_str(),
                client.chosen_security_type(),
                client.tight_selected() ? "Tight"
                                        : (client.chosen_security_type() == 2 ? "VNC" : "None"));
    std::printf("server:   %dx%d \"%s\"\n", info.width, info.height, info.name.c_str());
}

void print_caps(const Capabilities& caps) {
    if (!caps.present) {
        std::printf("caps:     not advertised\n");
        return;
    }
    std::printf("caps:     %zu server / %zu client / %zu encoding\n", caps.server.size(),
                caps.client.size(), caps.encodings.size());
    std::vector<std::string> ft;
    for (const auto& c : caps.client) {
        if ((c.code & 0xFFFFFF00u) == 0xFC000100u || (c.code >= 130 && c.code <= 136)) {
            char buf[32];
            std::snprintf(buf, sizeof(buf), "0x%08x", c.code);
            ft.emplace_back(buf);
        }
    }
    if (!ft.empty()) std::printf("client ft: %s\n", join(ft, " ").c_str());
}

void print_probe(const ft::ProbeResult& r) {
    std::printf("file xfer: %s", ft::to_string(r.kind));
    if (!r.detail.empty()) std::printf("  (%s)", r.detail.c_str());
    std::printf("\n");
    if (r.kind == ft::Kind::UltraVnc) {
        std::printf("uvnc:      permission=%d", r.uvnc_permission);
        if (!r.uvnc_drives.empty()) std::printf(" drives=%s", r.uvnc_drives.c_str());
        std::printf("\n");
    }
    if (r.kind == ft::Kind::TightVnc2 && !r.roots.empty()) {
        std::printf("roots:     %s\n", join(r.roots, " ").c_str());
    }
    if (r.kind == ft::Kind::TightVnc13 && !r.roots.empty()) {
        std::printf("entries:   %s\n", join(r.roots, " ").c_str());
    }
}

bool parse_endpoint(const std::string& text, Options& opts) {
    auto pos = text.rfind(':');
    if (pos == std::string::npos) {
        opts.host = text;
        return true;
    }
    opts.host = text.substr(0, pos);
    long port = std::strtol(text.c_str() + pos + 1, nullptr, 10);
    if (port <= 0 || port > 65535) return false;
    opts.port = static_cast<uint16_t>(port);
    return true;
}

std::unique_ptr<FtSession> open_client_session(const Options& opts) {
    return open_session(ConnectionOptions{opts.host, opts.port, opts.password, opts.timeout_ms});
}

int run_probe(Options opts) {
    bool found = false;
    std::string tight_error;
    try {
        RfbClient client(opts.host, opts.port, opts.password, opts.timeout_ms);
        client.connect();
        client.handshake(8, opts.prefer_tight);
        print_basic(client);
        print_caps(client.caps());

        if (client.caps().present) {
            if (client.caps().hasClient(rfb::kTv2CompressionSupportRequest)) {
                print_probe(ft::probe_tightvnc2(client));
                found = true;
            } else if (client.caps().hasClient(rfb::kTv13FileListRequest)) {
                print_probe(ft::probe_tightvnc13(client));
                found = true;
            } else {
                std::printf("file xfer: server advertises no file-transfer messages\n");
            }
        }
    } catch (const std::exception& e) {
        tight_error = e.what();
        std::printf("phase1:   %s\n", e.what());
    }

    if (found) return 0;

    try {
        RfbClient client(opts.host, opts.port, opts.password, opts.timeout_ms);
        client.connect();
        client.handshake(8, false);
        if (client.chosen_security_type() == rfb::kSecTight) {
            std::printf("file xfer: none detected (server only offered Tight; no FT capability)\n");
            return 1;
        }
        print_probe(ft::probe_ultravnc(client));
        return 0;
    } catch (const std::exception& e) {
        std::printf("file xfer: none (%s)\n", e.what());
        return 1;
    }
}

void progress_printer(uint64_t done, uint64_t total) {
    static auto last = std::chrono::steady_clock::now();
    auto now = std::chrono::steady_clock::now();
    if (total == 0) {
        if (now - last < std::chrono::milliseconds(200)) return;
        last = now;
        std::fprintf(stderr, "\r%llu bytes", static_cast<unsigned long long>(done));
        std::fflush(stderr);
        return;
    }
    if (now - last < std::chrono::milliseconds(200) && done != total) return;
    last = now;
    double pct = total ? 100.0 * static_cast<double>(done) / static_cast<double>(total) : 0.0;
    std::fprintf(stderr, "\r%5.1f%%  %s / %s", pct, human_size(done).c_str(),
                 human_size(total).c_str());
    std::fflush(stderr);
    if (done == total) std::fprintf(stderr, "\n");
}

int run_ls_once(const Options& opts) {
    auto session = open_client_session(opts);
    const std::string path = opts.path.empty() ? session->default_root() : opts.path;
    auto entries = session->list(path);
    std::printf("%s %10s  %-16s  %s\n", "d", "size", "modified", "name");
    for (const auto& e : entries) {
        std::printf("%s %10s  %-16s  %s%s\n", e.is_dir ? "d" : "-",
                    e.is_dir ? "-" : human_size(e.size).c_str(), fmt_time(e.mtime).c_str(),
                    e.name.c_str(), e.is_dir ? "/" : "");
    }
    return 0;
}

int run_ls(const Options& opts) {
    RetryPolicy policy{opts.retries, opts.retry_delay_ms, true};
    int rc = 0;
    with_retry(policy, [&](int attempt) {
        if (attempt > 0) {
            std::fprintf(stderr, "retry %d/%d\n", attempt + 1, policy.attempts);
        }
        rc = run_ls_once(opts);
    });
    return rc;
}

std::optional<uint64_t> query_remote_size(FtSession& session, const std::string& path) {
    auto pos = path.find_last_of('/');
    const std::string dir = pos == std::string::npos ? "/" : path.substr(0, pos);
    const std::string name = pos == std::string::npos ? path : path.substr(pos + 1);
    try {
        for (const auto& entry : session.list(dir.empty() ? "/" : dir)) {
            if (entry.name == name && !entry.is_dir) return entry.size;
        }
    } catch (const std::exception&) {
    }
    return std::nullopt;
}

uint64_t local_size(const std::string& path) {
    std::error_code ec;
    const auto size = std::filesystem::file_size(path, ec);
    return ec ? 0 : size;
}

int run_get_once(const Options& opts) {
    auto session = open_client_session(opts);

    if (opts.recursive) {
        TransferStats stats;
        download_tree(*session, opts.remote, opts.local, progress_printer, stats);
        std::printf("downloaded %llu files (%s) into %s\n",
                    static_cast<unsigned long long>(stats.files), human_size(stats.bytes).c_str(),
                    opts.local.c_str());
        return 0;
    }

    if (opts.resume && session->supports_resume()) {
        auto remote_size = query_remote_size(*session, opts.remote);
        const uint64_t have = local_size(opts.local);
        if (remote_size && have > 0 && have < *remote_size) {
            std::printf("resuming at %s / %s\n", human_size(have).c_str(),
                        human_size(*remote_size).c_str());
            session->download_resume(opts.remote, opts.local, have, progress_printer);
            std::printf("downloaded %s -> %s\n", opts.remote.c_str(), opts.local.c_str());
            return 0;
        }
    }

    session->download(opts.remote, opts.local, progress_printer);
    std::printf("downloaded %s -> %s\n", opts.remote.c_str(), opts.local.c_str());
    return 0;
}

int run_get(const Options& opts) {
    RetryPolicy policy{opts.retries, opts.retry_delay_ms, true};
    int rc = 0;
    with_retry(policy, [&](int attempt) {
        if (attempt > 0) {
            std::fprintf(stderr, "retry %d/%d\n", attempt + 1, policy.attempts);
        }
        rc = run_get_once(opts);
    });
    return rc;
}

int run_put_once(const Options& opts) {
    auto session = open_client_session(opts);

    if (opts.recursive) {
        TransferStats stats;
        upload_tree(*session, opts.local, opts.remote, opts.overwrite, progress_printer, stats);
        std::printf("uploaded %llu files (%s) into %s\n",
                    static_cast<unsigned long long>(stats.files), human_size(stats.bytes).c_str(),
                    opts.remote.c_str());
        return 0;
    }

    if (opts.resume && session->supports_resume()) {
        auto remote_size = query_remote_size(*session, opts.remote);
        const uint64_t have = local_size(opts.local);
        if (remote_size && *remote_size > 0 && *remote_size < have) {
            std::printf("resuming at %s / %s\n", human_size(*remote_size).c_str(),
                        human_size(have).c_str());
            session->upload_resume(opts.local, opts.remote, opts.overwrite, *remote_size,
                                   progress_printer);
            std::printf("uploaded %s -> %s\n", opts.local.c_str(), opts.remote.c_str());
            return 0;
        }
    }

    session->upload(opts.local, opts.remote, opts.overwrite, progress_printer);
    std::printf("uploaded %s -> %s\n", opts.local.c_str(), opts.remote.c_str());
    return 0;
}

int run_put(const Options& opts) {
    RetryPolicy policy{opts.retries, opts.retry_delay_ms, true};
    int rc = 0;
    with_retry(policy, [&](int attempt) {
        if (attempt > 0) {
            std::fprintf(stderr, "retry %d/%d\n", attempt + 1, policy.attempts);
        }
        rc = run_put_once(opts);
    });
    return rc;
}

int run_action_once(const std::string& cmd, const Options& opts) {
    auto session = open_client_session(opts);
    if (cmd == "mkdir") {
        session->mkdir(opts.path);
    } else if (cmd == "rm") {
        if (opts.recursive) {
            remove_tree(*session, opts.path);
        } else {
            session->remove(opts.path);
        }
    } else if (cmd == "mv") {
        session->rename(opts.from, opts.to);
    }
    std::printf("%s: ok\n", cmd.c_str());
    return 0;
}

int run_action(const std::string& cmd, const Options& opts) {
    RetryPolicy policy{opts.retries, opts.retry_delay_ms, true};
    int rc = 0;
    with_retry(policy, [&](int attempt) {
        if (attempt > 0) {
            std::fprintf(stderr, "retry %d/%d\n", attempt + 1, policy.attempts);
        }
        rc = run_action_once(cmd, opts);
    });
    return rc;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 3 || argv[2][0] == '-') {
        usage();
        return 2;
    }

    const std::string cmd = argv[1];
    Options opts;
    if (const char* env = std::getenv("VNXFER_PASSWORD")) opts.password = env;
    if (!parse_endpoint(argv[2], opts)) {
        std::fprintf(stderr, "bad endpoint: %s\n", argv[2]);
        return 2;
    }

    for (int i = 3; i < argc; ++i) {
        std::string arg = argv[i];
        auto need = [&](const char* name) -> std::string {
            if (i + 1 >= argc) throw std::runtime_error(std::string("missing value for ") + name);
            return argv[++i];
        };
        try {
            if (arg == "--password") {
                opts.password = need("--password");
            } else if (arg == "--timeout") {
                opts.timeout_ms = std::atoi(need("--timeout").c_str());
            } else if (arg == "--path") {
                opts.path = need("--path");
            } else if (arg == "--remote") {
                opts.remote = need("--remote");
            } else if (arg == "--local") {
                opts.local = need("--local");
            } else if (arg == "--from") {
                opts.from = need("--from");
            } else if (arg == "--to") {
                opts.to = need("--to");
            } else if (arg == "--overwrite") {
                opts.overwrite = true;
            } else if (arg == "--no-overwrite") {
                opts.overwrite = false;
            } else if (arg == "--retries") {
                opts.retries = std::atoi(need("--retries").c_str());
            } else if (arg == "--retry-delay") {
                opts.retry_delay_ms = std::atoi(need("--retry-delay").c_str());
            } else if (arg == "--recursive") {
                opts.recursive = true;
            } else if (arg == "--resume") {
                opts.resume = true;
            } else if (arg == "--no-tight") {
                opts.prefer_tight = false;
            } else {
                std::fprintf(stderr, "unknown option: %s\n", arg.c_str());
                usage();
                return 2;
            }
        } catch (const std::exception& e) {
            std::fprintf(stderr, "%s\n", e.what());
            return 2;
        }
    }

    try {
        if (cmd == "probe") {
            std::printf("endpoint: %s:%u\n", opts.host.c_str(), opts.port);
            return run_probe(opts);
        }
        if (cmd == "ls") return run_ls(opts);
        if (cmd == "get") {
            if (opts.remote.empty() || opts.local.empty()) {
                std::fprintf(stderr, "get needs --remote and --local\n");
                return 2;
            }
            return run_get(opts);
        }
        if (cmd == "put") {
            if (opts.remote.empty() || opts.local.empty()) {
                std::fprintf(stderr, "put needs --local and --remote\n");
                return 2;
            }
            return run_put(opts);
        }
        if (cmd == "mkdir" || cmd == "rm") {
            if (opts.path.empty()) {
                std::fprintf(stderr, "%s needs --path\n", cmd.c_str());
                return 2;
            }
            return run_action(cmd, opts);
        }
        if (cmd == "mv") {
            if (opts.from.empty() || opts.to.empty()) {
                std::fprintf(stderr, "mv needs --from and --to\n");
                return 2;
            }
            return run_action(cmd, opts);
        }
        usage();
        return 2;
    } catch (const std::exception& e) {
        std::fprintf(stderr, "error: %s\n", e.what());
        return 1;
    }
}
