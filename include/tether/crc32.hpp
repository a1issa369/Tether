#pragma once
#include <cstddef>
#include <cstdint>

namespace tether {

// CRC-32 (IEEE 802.3, reflected, polynomial 0xEDB88320).
// Chain calls by passing the previous result as `seed`.
std::uint32_t crc32(const std::uint8_t* data, std::size_t size, std::uint32_t seed = 0);

}  // namespace tether
