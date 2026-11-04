#include <random>

#include "tether/crc32.hpp"
#include "tether/packet.hpp"
#include "test_main.hpp"

using namespace tether;

namespace {

Packet sample(PacketType t) {
    Packet p;
    p.type = t;
    p.window = 0xBEEF;
    p.conn_id = 0xDEADBEEF;
    p.seq = 0xFFFFFFFF;
    p.ack = 0x01020304;
    p.sack = 0x80000001;
    if (t == PacketType::Data) {
        p.flags = kFlagBegin | kFlagEnd;
        p.payload = {1, 2, 3, 4, 5, 250, 251, 252};
    }
    return p;
}

bool same(const Packet& a, const Packet& b) {
    return a.type == b.type && a.flags == b.flags && a.window == b.window && a.conn_id == b.conn_id &&
           a.seq == b.seq && a.ack == b.ack && a.sack == b.sack && a.payload == b.payload;
}

}  // namespace

TEST(crc32_matches_known_vector) {
    const char* s = "123456789";
    CHECK_EQ(crc32(reinterpret_cast<const std::uint8_t*>(s), 9), 0xCBF43926u);
    // chaining equals one pass
    const auto* p = reinterpret_cast<const std::uint8_t*>(s);
    CHECK_EQ(crc32(p + 4, 5, crc32(p, 4)), 0xCBF43926u);
}

TEST(packet_round_trips_every_type) {
    for (int t = 1; t <= 7; ++t) {
        Packet in = sample(static_cast<PacketType>(t));
        Bytes wire = encode(in);
        CHECK_EQ(wire.size(), kHeaderSize + in.payload.size());
        Packet out;
        DecodeError err = DecodeError::BadMagic;
        CHECK(decode(wire.data(), wire.size(), out, &err));
        CHECK(err == DecodeError::None);
        CHECK(same(in, out));
    }
}

TEST(packet_probe_flag_round_trips_on_ack) {
    Packet in = sample(PacketType::Ack);
    in.flags = kFlagProbe;
    Bytes wire = encode(in);
    Packet out;
    CHECK(decode(wire.data(), wire.size(), out));
    CHECK_EQ(out.flags, kFlagProbe);
}

TEST(packet_empty_data_payload_is_valid) {
    Packet in = sample(PacketType::Data);
    in.payload.clear();
    Bytes wire = encode(in);
    Packet out;
    CHECK(decode(wire.data(), wire.size(), out));
    CHECK(out.payload.empty());
}

TEST(any_single_byte_change_is_rejected) {
    const Bytes wire = encode(sample(PacketType::Data));
    for (std::size_t i = 0; i < wire.size(); ++i) {
        for (int bit = 0; bit < 8; ++bit) {
            Bytes bad = wire;
            bad[i] ^= static_cast<std::uint8_t>(1u << bit);
            Packet out;
            CHECK(!decode(bad.data(), bad.size(), out));
        }
    }
}

TEST(every_truncation_is_rejected) {
    const Bytes wire = encode(sample(PacketType::Data));
    for (std::size_t n = 0; n < wire.size(); ++n) {
        Packet out;
        DecodeError err = DecodeError::None;
        CHECK(!decode(wire.data(), n, out, &err));
        CHECK(err != DecodeError::None);
    }
}

TEST(decoder_reports_specific_errors) {
    Packet out;
    DecodeError err = DecodeError::None;

    Bytes wire = encode(sample(PacketType::Data));
    wire[0] ^= 0xFF;
    CHECK(!decode(wire.data(), wire.size(), out, &err));
    CHECK(err == DecodeError::BadMagic);

    wire = encode(sample(PacketType::Data));
    wire[2] = 9;
    CHECK(!decode(wire.data(), wire.size(), out, &err));
    CHECK(err == DecodeError::BadVersion);

    wire = encode(sample(PacketType::Data));
    wire[30] ^= 1;
    CHECK(!decode(wire.data(), wire.size(), out, &err));
    CHECK(err == DecodeError::BadChecksum);

    // Well-formed checksum but semantically invalid packets (made by encoding them directly).
    Packet p = sample(PacketType::Ack);
    p.payload = {1};
    wire = encode(p);
    CHECK(!decode(wire.data(), wire.size(), out, &err));
    CHECK(err == DecodeError::UnexpectedPayload);

    p = sample(PacketType::Ack);
    p.flags = kFlagBegin;  // Begin is a Data flag
    wire = encode(p);
    CHECK(!decode(wire.data(), wire.size(), out, &err));
    CHECK(err == DecodeError::BadFlags);

    p = sample(PacketType::Data);
    p.flags = kFlagProbe;  // Probe is an Ack flag
    wire = encode(p);
    CHECK(!decode(wire.data(), wire.size(), out, &err));
    CHECK(err == DecodeError::BadFlags);

    p = sample(PacketType::Data);
    p.type = static_cast<PacketType>(42);
    wire = encode(p);
    CHECK(!decode(wire.data(), wire.size(), out, &err));
    CHECK(err == DecodeError::BadType);
}

TEST(decoder_survives_random_garbage) {
    std::mt19937 rng(12345);
    int accepted = 0;
    for (int i = 0; i < 200000; ++i) {
        Bytes junk(rng() % 80);
        for (auto& b : junk) b = static_cast<std::uint8_t>(rng());
        // sometimes start from a valid header so the checks past the magic number get exercised too
        if (junk.size() >= kHeaderSize && (rng() % 2) == 0) {
            junk[0] = 0x54;
            junk[1] = 0x45;
            junk[2] = 1;
        }
        Packet out;
        if (decode(junk.data(), junk.size(), out)) ++accepted;
    }
    CHECK_EQ(accepted, 0);  // a 32-bit checksum makes accidental acceptance astronomically unlikely
}
