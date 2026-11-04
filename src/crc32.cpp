#include "tether/crc32.hpp"

#include <array>

namespace tether {
namespace {

std::array<std::uint32_t, 256> make_table() {
    std::array<std::uint32_t, 256> table{};
    for (std::uint32_t i = 0; i < 256; ++i) {
        std::uint32_t c = i;
        for (int k = 0; k < 8; ++k) {
            c = (c & 1U) ? (0xEDB88320U ^ (c >> 1)) : (c >> 1);
        }
        table[i] = c;
    }
    return table;
}

}  // namespace

std::uint32_t crc32(const std::uint8_t* data, std::size_t size, std::uint32_t seed) {
    static const std::array<std::uint32_t, 256> table = make_table();
    std::uint32_t c = ~seed;
    for (std::size_t i = 0; i < size; ++i) {
        c = table[(c ^ data[i]) & 0xFFU] ^ (c >> 8);
    }
    return ~c;
}

}  // namespace tether
