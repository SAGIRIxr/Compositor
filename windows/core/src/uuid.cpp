#include "compositor/uuid.h"
#include <cctype>
#include <cstdio>
#include <random>

namespace comp {

std::string makeUUID() {
    static thread_local std::mt19937_64 engine{std::random_device{}() ^ (uint64_t(std::random_device{}()) << 32)};
    uint8_t b[16];
    for (int i = 0; i < 16; i += 8) {
        uint64_t v = engine();
        for (int k = 0; k < 8; ++k) b[i + k] = uint8_t(v >> (k * 8));
    }
    b[6] = uint8_t((b[6] & 0x0F) | 0x40);
    b[8] = uint8_t((b[8] & 0x3F) | 0x80);
    char text[37];
    std::snprintf(text, sizeof text, "%02X%02X%02X%02X-%02X%02X-%02X%02X-%02X%02X-%02X%02X%02X%02X%02X%02X",
                  b[0], b[1], b[2], b[3], b[4], b[5], b[6], b[7], b[8], b[9], b[10], b[11], b[12], b[13], b[14], b[15]);
    return text;
}

std::string normalizeUUID(const std::string& text) {
    if (text.size() != 36) return {};
    std::string out = text;
    for (size_t i = 0; i < out.size(); ++i) {
        if (i == 8 || i == 13 || i == 18 || i == 23) { if (out[i] != '-') return {}; continue; }
        if (!std::isxdigit(static_cast<unsigned char>(out[i]))) return {};
        out[i] = char(std::toupper(static_cast<unsigned char>(out[i])));
    }
    return out;
}

} // namespace comp
