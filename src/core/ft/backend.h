#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "core/rfb_client.h"

namespace vnxfer::ft {

enum class Kind { None, UltraVnc, TightVnc2, TightVnc13 };

const char* to_string(Kind kind);

struct ProbeResult {
    Kind kind = Kind::None;
    std::string detail;
    bool compression = false;
    int block_size = 0;
    std::vector<std::string> roots;   // TightVNC 2.x drive list
    std::string uvnc_drives;          // UltraVNC raw drive list
    int uvnc_permission = 0;          // 1 allow, -1 deny, 0 unknown
};

ProbeResult probe_ultravnc(RfbClient& client);
ProbeResult probe_tightvnc2(RfbClient& client);
ProbeResult probe_tightvnc13(RfbClient& client);

}  // namespace vnxfer::ft
