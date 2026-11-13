#include "core/des.h"

#include <openssl/des.h>

#include <cstring>

namespace vnxfer {

namespace {
uint8_t reverse_bits(uint8_t v) {
    v = static_cast<uint8_t>((v & 0xF0) >> 4 | (v & 0x0F) << 4);
    v = static_cast<uint8_t>((v & 0xCC) >> 2 | (v & 0x33) << 2);
    v = static_cast<uint8_t>((v & 0xAA) >> 1 | (v & 0x55) << 1);
    return v;
}
}  // namespace

std::array<uint8_t, 16> vnc_auth_response(std::span<const uint8_t, 16> challenge,
                                          std::string_view password) {
    DES_cblock key{};
    for (std::size_t i = 0; i < 8 && i < password.size(); ++i) {
        key[i] = reverse_bits(static_cast<uint8_t>(password[i]));
    }

    DES_key_schedule schedule;
    DES_set_key_unchecked(&key, &schedule);

    std::array<uint8_t, 16> out{};
    DES_cblock in1{};
    DES_cblock in2{};
    DES_cblock out1{};
    DES_cblock out2{};
    std::memcpy(in1, challenge.data(), 8);
    std::memcpy(in2, challenge.data() + 8, 8);
    DES_ecb_encrypt(&in1, &out1, &schedule, DES_ENCRYPT);
    DES_ecb_encrypt(&in2, &out2, &schedule, DES_ENCRYPT);
    std::memcpy(out.data(), out1, 8);
    std::memcpy(out.data() + 8, out2, 8);
    return out;
}

}  // namespace vnxfer
