#pragma once

#include <array>
#include <cstdint>
#include <span>
#include <string_view>

namespace vnxfer {

// VNC authentication: DES-ECB over the 16-byte challenge with a key derived
// from the password by reversing the bits of each byte.
std::array<uint8_t, 16> vnc_auth_response(std::span<const uint8_t, 16> challenge,
                                          std::string_view password);

}  // namespace vnxfer
