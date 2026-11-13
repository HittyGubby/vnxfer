#include "core/ft/open.h"

#include <stdexcept>

#include "core/ft/backend.h"
#include "core/rfb_client.h"
#include "core/rfb_protocol.h"

namespace vnxfer {

std::unique_ptr<FtSession> open_session(const ConnectionOptions& options) {
    std::string tight_error;
    try {
        RfbClient client(options.host, options.port, options.password, options.timeout_ms);
        client.connect();
        client.handshake(8, true);
        if (client.caps().present && client.caps().hasClient(rfb::kTv2CompressionSupportRequest)) {
            return make_tightvnc2_session(std::move(client));
        }
        if (client.caps().present && client.caps().hasClient(rfb::kTv13FileListRequest)) {
            return make_tightvnc13_session(std::move(client));
        }
    } catch (const std::exception& e) {
        tight_error = e.what();
    }

    RfbClient client(options.host, options.port, options.password, options.timeout_ms);
    client.connect();
    client.handshake(8, false);
    auto probe = ft::probe_ultravnc(client);
    if (probe.kind == ft::Kind::UltraVnc && probe.uvnc_permission == 1) {
        return make_ultravnc_session(std::move(client));
    }
    if (!probe.detail.empty()) throw std::runtime_error(probe.detail);
    if (!tight_error.empty()) throw std::runtime_error("no file transfer support (" + tight_error + ")");
    throw std::runtime_error("no file transfer support");
}

}  // namespace vnxfer
