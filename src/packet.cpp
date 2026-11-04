#include "tether/packet.hpp"

#include "tether/crc32.hpp"

namespace tether {
namespace {

void put16(Bytes& b, std::size_t at, std::uint16_t v) {
    b[at] = static_cast<std::uint8_t>(v >> 8);
    b[at + 1] = static_cast<std::uint8_t>(v);
}

void put32(Bytes& b, std::size_t at, std::uint32_t v) {
    b[at] = static_cast<std::uint8_t>(v >> 24);
    b[at + 1] = static_cast<std::uint8_t>(v >> 16);
    b[at + 2] = static_cast<std::uint8_t>(v >> 8);
    b[at + 3] = static_cast<std::uint8_t>(v);
}

std::uint16_t get16(const std::uint8_t* p) {
    return static_cast<std::uint16_t>((p[0] << 8) | p[1]);
}

std::uint32_t get32(const std::uint8_t* p) {
    return (static_cast<std::uint32_t>(p[0]) << 24) | (static_cast<std::uint32_t>(p[1]) << 16) |
           (static_cast<std::uint32_t>(p[2]) << 8) | static_cast<std::uint32_t>(p[3]);
}

// The checksum covers the whole datagram with the checksum field itself read as zero.
std::uint32_t checksum_of(const std::uint8_t* data, std::size_t size) {
    static const std::uint8_t zero[4] = {0, 0, 0, 0};
    std::uint32_t c = crc32(data, 24);
    c = crc32(zero, 4, c);
    return crc32(data + kHeaderSize, size - kHeaderSize, c);
}

bool fail(DecodeError* slot, DecodeError e) {
    if (slot != nullptr) *slot = e;
    return false;
}

}  // namespace

Bytes encode(const Packet& p) {
    Bytes out(kHeaderSize + p.payload.size(), 0);
    put16(out, 0, kMagic);
    out[2] = kVersion;
    out[3] = static_cast<std::uint8_t>(p.type);
    out[4] = p.flags;
    out[5] = 0;
    put16(out, 6, p.window);
    put32(out, 8, p.conn_id);
    put32(out, 12, p.seq);
    put32(out, 16, p.ack);
    put32(out, 20, p.sack);
    for (std::size_t i = 0; i < p.payload.size(); ++i) out[kHeaderSize + i] = p.payload[i];
    put32(out, 24, checksum_of(out.data(), out.size()));
    return out;
}

bool decode(const std::uint8_t* data, std::size_t size, Packet& out, DecodeError* error) {
    if (error != nullptr) *error = DecodeError::None;
    if (data == nullptr || size < kHeaderSize) return fail(error, DecodeError::TooShort);
    if (get16(data) != kMagic) return fail(error, DecodeError::BadMagic);
    if (data[2] != kVersion) return fail(error, DecodeError::BadVersion);
    if (size - kHeaderSize > kMaxPayload) return fail(error, DecodeError::PayloadTooLarge);
    if (get32(data + 24) != checksum_of(data, size)) return fail(error, DecodeError::BadChecksum);

    const std::uint8_t raw_type = data[3];
    if (raw_type < 1 || raw_type > 7) return fail(error, DecodeError::BadType);
    const auto type = static_cast<PacketType>(raw_type);
    const std::uint8_t flags = data[4];
    if (data[5] != 0) return fail(error, DecodeError::BadFlags);

    std::uint8_t allowed = 0;
    if (type == PacketType::Data) allowed = kFlagBegin | kFlagEnd;
    if (type == PacketType::Ack) allowed = kFlagProbe;
    if ((flags & ~allowed) != 0) return fail(error, DecodeError::BadFlags);
    if (type != PacketType::Data && size != kHeaderSize) return fail(error, DecodeError::UnexpectedPayload);

    out.type = type;
    out.flags = flags;
    out.window = get16(data + 6);
    out.conn_id = get32(data + 8);
    out.seq = get32(data + 12);
    out.ack = get32(data + 16);
    out.sack = get32(data + 20);
    out.payload.assign(data + kHeaderSize, data + size);
    return true;
}

}  // namespace tether
