#pragma once
#include <cstddef>
#include <cstdint>
#include <vector>

namespace tether {

using Bytes = std::vector<std::uint8_t>;

enum class PacketType : std::uint8_t {
    Syn = 1,     // client -> server: open a connection, carries the client's initial sequence number
    SynAck = 2,  // server -> client: accept, carries the server's initial sequence number
    Data = 3,    // one fragment of a message; consumes one sequence number
    Ack = 4,     // acknowledgement only; consumes no sequence number
    Fin = 5,     // sender is done sending; sent only after all its data is acknowledged
    FinAck = 6,  // acknowledges a Fin
    Rst = 7,     // abort: the connection id is unknown or the peer broke the protocol
};

// Data packet flags: a message is a run of Data packets from one with Begin to one with End.
constexpr std::uint8_t kFlagBegin = 0x01;
constexpr std::uint8_t kFlagEnd = 0x02;
// Ack packet flag: ask the peer to answer with an Ack (used to learn a reopened window).
constexpr std::uint8_t kFlagProbe = 0x04;

constexpr std::uint16_t kMagic = 0x5445;  // "TE"
constexpr std::uint8_t kVersion = 1;
constexpr std::size_t kHeaderSize = 28;
constexpr std::size_t kMaxPayload = 65000;  // hard decoder limit, far above any sane MTU

struct Packet {
    PacketType type = PacketType::Ack;
    std::uint8_t flags = 0;
    std::uint16_t window = 0;    // receiver's remaining capacity, in packets, beyond `ack`
    std::uint32_t conn_id = 0;
    std::uint32_t seq = 0;       // Data: sequence number. Syn/SynAck: initial sequence number. Fin: next seq.
    std::uint32_t ack = 0;       // next sequence number the sender of this packet expects
    std::uint32_t sack = 0;      // bit i set: packet ack+1+i has been received out of order
    Bytes payload;               // Data only
};

enum class DecodeError {
    None,
    TooShort,
    BadMagic,
    BadVersion,
    BadType,
    BadFlags,
    BadChecksum,
    UnexpectedPayload,
    PayloadTooLarge,
};

Bytes encode(const Packet& packet);

// Returns true and fills `out` on success. On failure `out` is untouched and `error` (if given) says why.
bool decode(const std::uint8_t* data, std::size_t size, Packet& out, DecodeError* error = nullptr);

}  // namespace tether
