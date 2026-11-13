#include "core/ft/backend.h"

#include <algorithm>
#include <stdexcept>

#include "core/rfb_protocol.h"

namespace vnxfer::ft {

const char* to_string(Kind kind) {
    switch (kind) {
        case Kind::UltraVnc: return "ultravnc";
        case Kind::TightVnc2: return "tightvnc2";
        case Kind::TightVnc13: return "tightvnc1.3";
        case Kind::None: default: return "none";
    }
}

// Permission probe: type 7 / AbortFileTransfer / contentParam >= 1 -> Access reply.
ProbeResult probe_ultravnc(RfbClient& client) {
    ProbeResult result;
    result.kind = Kind::None;

    client.send_ft(rfb::kFtAbortFileTransfer, 3, 0, {});

    FtMessage msg;
    bool got = false;
    for (int i = 0; i < 6; ++i) {
        if (!client.read_ft_message(msg, 3000)) break;
        if (msg.ctype == rfb::kFtProtocolVersion || msg.ctype == rfb::kFtSessionStart ||
            msg.ctype == rfb::kFtSessionEnd) {
            continue;
        }
        got = true;
        break;
    }
    if (!got) {
        result.detail = "no reply to permission probe (server does not speak UltraVNC FT)";
        return result;
    }

    if (msg.ctype == rfb::kFtFileTransferAccess) {
        result.kind = Kind::UltraVnc;
        result.uvnc_permission = static_cast<int>(msg.size);
        if (msg.size == 0xFFFFFFFFu) {
            result.uvnc_permission = -1;
        }
        result.detail = msg.size == 1 ? "permission granted" : "permission denied";
    } else if (msg.ctype == rfb::kFtAbortFileTransfer) {
        // Old (<= RC18) server answers the probe with Abort + size -1.
        result.kind = Kind::UltraVnc;
        result.uvnc_permission = -1;
        result.detail = "old protocol (<=RC18): permission denied";
        return result;
    } else {
        result.detail = "unexpected reply to permission probe (ctype " +
                        std::to_string(msg.ctype) + ")";
        return result;
    }

    if (result.uvnc_permission != 1) return result;

    // Drives list
    client.send_ft(rfb::kFtDirContentRequest, rfb::kFtRDrivesList, 0, {});
    if (client.read_ft_message(msg, 3000) && msg.ctype == rfb::kFtDirPacket &&
        msg.cparam == rfb::kFtADrivesList) {
        result.uvnc_drives.assign(msg.payload.begin(), msg.payload.end());
        // Strip NULs for display.
        result.uvnc_drives.erase(
            std::remove(result.uvnc_drives.begin(), result.uvnc_drives.end(), '\0'),
            result.uvnc_drives.end());
    }
    return result;
}

}  // namespace vnxfer::ft
