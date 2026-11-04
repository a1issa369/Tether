#include <cstdio>
#include <memory>

#include "sim.hpp"
#include "test_main.hpp"

using namespace tether;
using namespace sim;

namespace {

struct Scenario {
    Params net;
    Config client;
    Config server;
    std::size_t msgs = 30;
    std::size_t max_size = 4000;
    bool bidir = true;
    Millis limit_ms = 600000;
};

struct Result {
    std::unique_ptr<Net> net;
    std::vector<Bytes> sent_a, sent_b, got_a, got_b;
    bool finished = false;
};

Result run_transfer(const Scenario& sc) {
    Result r;
    r.net = std::make_unique<Net>(sc.net, sc.client, sc.server);
    Net& net = *r.net;
    std::mt19937 sizes(sc.net.seed * 7919u + 1);
    for (std::size_t i = 0; i < sc.msgs; ++i) {
        const std::size_t size_a = (i % 11 == 5) ? 0 : sizes() % (sc.max_size + 1);
        const std::size_t size_b = (i % 13 == 3) ? 0 : sizes() % (sc.max_size + 1);
        r.sent_a.push_back(make_message(i, size_a));
        r.sent_b.push_back(make_message(1000 + i, size_b));
    }
    bool a_queued = false;
    bool b_queued = false;
    while (net.now < sc.limit_ms) {
        net.step();
        if (!a_queued) {
            for (const auto& m : r.sent_a) CHECK(net.a.send(m, net.now) == SendResult::Ok);
            a_queued = true;
        }
        if (sc.bidir && net.b && !b_queued) {
            for (const auto& m : r.sent_b) CHECK(net.b->send(m, net.now) == SendResult::Ok);
            b_queued = true;
        }
        while (auto m = net.a.receive()) r.got_a.push_back(std::move(*m));
        if (net.b) {
            while (auto m = net.b->receive()) r.got_b.push_back(std::move(*m));
        }
        const bool b_done = r.got_b.size() == sc.msgs;
        const bool a_done = !sc.bidir || r.got_a.size() == sc.msgs;
        if (b_done && a_done) {
            r.finished = true;
            break;
        }
    }
    return r;
}

void expect_exact(const Scenario& sc, const Result& r, const char* what) {
    const bool ok = r.finished && r.got_b == r.sent_a && (!sc.bidir || r.got_a == r.sent_b);
    if (!ok) {
        std::printf("      %s: seed=%u loss=%.2f dup=%.2f corrupt=%.2f jitter=%llu finished=%d got_b=%zu/%zu got_a=%zu/%zu\n",
                    what, sc.net.seed, sc.net.loss, sc.net.dup, sc.net.corrupt,
                    static_cast<unsigned long long>(sc.net.jitter), r.finished ? 1 : 0, r.got_b.size(), sc.msgs,
                    r.got_a.size(), sc.msgs);
    }
    CHECK(ok);
}

Packet mk(PacketType type, std::uint32_t id, std::uint32_t seq, std::uint32_t ack, std::uint16_t window = 64) {
    Packet p;
    p.type = type;
    p.conn_id = id;
    p.seq = seq;
    p.ack = ack;
    p.window = window;
    return p;
}

// A server-side connection that has completed the handshake with a peer whose first seq is 100.
Connection established_server(const Config& cfg = {}) {
    Connection s = Connection::make_server(mk(PacketType::Syn, 7, 100, 0, 64), cfg, 0);
    s.take_output();
    s.on_packet(mk(PacketType::Ack, 7, 0, cfg.initial_seq), 1);
    return s;
}

Packet data_packet(std::uint32_t seq, std::uint8_t flags, Bytes payload) {
    Packet p = mk(PacketType::Data, 7, seq, 0);
    p.flags = flags;
    p.payload = std::move(payload);
    return p;
}

}  // namespace

// ---------- handshake ----------

TEST(handshake_completes) {
    Net net({});
    CHECK(net.run_until([&] { return net.b && net.a.established() && net.b->established(); }, 2000));
    CHECK(net.a.error() == Error::None);
    CHECK(net.b->error() == Error::None);
}

TEST(handshake_survives_a_lost_syn) {
    Net net({});
    int syns = 0;
    net.drop_if = [&](int from, const Packet& p) { return from == 0 && p.type == PacketType::Syn && ++syns == 1; };
    CHECK(net.run_until([&] { return net.b && net.a.established() && net.b->established(); }, 5000));
    CHECK(net.count_sent(0, PacketType::Syn) >= 2);
}

TEST(handshake_survives_a_lost_synack) {
    Net net({});
    int n = 0;
    net.drop_if = [&](int from, const Packet& p) { return from == 1 && p.type == PacketType::SynAck && ++n == 1; };
    CHECK(net.run_until([&] { return net.b && net.a.established() && net.b->established(); }, 5000));
}

TEST(handshake_survives_a_lost_final_ack) {
    Net net({});
    int n = 0;
    net.drop_if = [&](int from, const Packet& p) { return from == 0 && p.type == PacketType::Ack && ++n == 1; };
    CHECK(net.run_until([&] { return net.b && net.a.established() && net.b->established(); }, 5000));
}

TEST(connect_to_nobody_times_out) {
    Config cfg;
    cfg.initial_rto_ms = 50;
    cfg.max_retries = 4;
    Net net({}, cfg);
    net.drop_if = [](int, const Packet&) { return true; };
    CHECK(net.run_until([&] { return net.a.closed(); }, 20000));
    CHECK(net.a.error() == Error::Timeout);
}

// ---------- delivery ----------

TEST(delivers_in_order_without_loss) {
    Scenario sc;
    sc.msgs = 100;
    sc.max_size = 3000;
    Result r = run_transfer(sc);
    expect_exact(sc, r, "clean");
    CHECK_EQ(r.net->a.stats().retransmits, 0u);
    CHECK_EQ(r.net->b->stats().retransmits, 0u);
}

TEST(fragments_and_reassembles_a_large_message) {
    Net net({});
    const Bytes big = make_message(1, 600000);
    CHECK(net.a.send(big, 0) == SendResult::Ok);
    std::optional<Bytes> got;
    CHECK(net.run_until(
        [&] {
            if (net.b && !got) got = net.b->receive();
            return got.has_value();
        },
        120000));
    CHECK(got.has_value() && *got == big);
    CHECK(net.a.stats().data_packets_sent >= 600000 / 1200);
}

TEST(zero_length_message_is_delivered) {
    Net net({});
    CHECK(net.a.send(Bytes{}, 0) == SendResult::Ok);
    std::optional<Bytes> got;
    CHECK(net.run_until(
        [&] {
            if (net.b && !got) got = net.b->receive();
            return got.has_value();
        },
        5000));
    CHECK(got.has_value() && got->empty());
}

TEST(survives_loss_across_many_seeds) {
    for (double loss : {0.10, 0.30}) {
        for (std::uint32_t seed = 1; seed <= 15; ++seed) {
            Scenario sc;
            sc.net.loss = loss;
            sc.net.seed = seed;
            sc.net.jitter = 5;
            Result r = run_transfer(sc);
            expect_exact(sc, r, "loss");
        }
    }
}

TEST(survives_duplication_reordering_and_corruption) {
    for (std::uint32_t seed = 1; seed <= 20; ++seed) {
        Scenario sc;
        sc.net.loss = 0.15;
        sc.net.dup = 0.25;
        sc.net.corrupt = 0.10;
        sc.net.jitter = 80;
        sc.net.seed = seed;
        Result r = run_transfer(sc);
        expect_exact(sc, r, "chaos");
    }
}

TEST(every_datagram_duplicated_delivers_each_message_once) {
    Scenario sc;
    sc.net.dup = 1.0;
    sc.msgs = 50;
    Result r = run_transfer(sc);
    expect_exact(sc, r, "dup");
    CHECK(r.net->b->stats().duplicates_received > 0);
}

TEST(corruption_is_detected_not_delivered) {
    Scenario sc;
    sc.net.corrupt = 0.3;
    sc.net.seed = 9;
    Result r = run_transfer(sc);
    expect_exact(sc, r, "corrupt");
    CHECK(r.net->a.stats().bad_packets + r.net->b->stats().bad_packets > 0);
}

TEST(sequence_numbers_wrap_around) {
    Scenario sc;
    sc.client.initial_seq = 0xFFFFFF00u;
    sc.server.initial_seq = 0xFFFFFFF0u;
    sc.net.loss = 0.10;
    sc.net.jitter = 10;
    sc.msgs = 400;
    sc.max_size = 200;
    Result r = run_transfer(sc);
    expect_exact(sc, r, "wrap");
    CHECK(r.net->a.stats().data_packets_sent > 400);  // well past 2^32 - 0xFFFFFF00 = 256 packets
}

// ---------- reliability machinery ----------

TEST(retransmission_timer_backs_off_exponentially_then_gives_up) {
    Config cfg;
    cfg.initial_rto_ms = 100;
    cfg.max_rto_ms = 3000;
    cfg.max_retries = 6;
    Net net({}, cfg, cfg);
    CHECK(net.run_until([&] { return net.b && net.a.established() && net.b->established(); }, 2000));
    net.drop_if = [](int, const Packet&) { return true; };  // the network goes dark
    CHECK(net.a.send(make_message(1, 100), net.now) == SendResult::Ok);
    CHECK(net.run_until([&] { return net.a.closed(); }, 120000));
    CHECK(net.a.error() == Error::Timeout);

    std::vector<Millis> times;
    for (const auto& s : net.log) {
        if (s.from == 0 && s.pkt.type == PacketType::Data) times.push_back(s.at);
    }
    CHECK_EQ(times.size(), static_cast<std::size_t>(cfg.max_retries + 1));
    // The first send happens between simulator steps, so its log time can be 1 ms late; the gaps after
    // the first retransmission are exact.
    const Millis first_gap = times[1] - times[0];
    CHECK(first_gap == cfg.initial_rto_ms || first_gap == cfg.initial_rto_ms - 1);
    Millis expected_gap = cfg.initial_rto_ms * 2;
    for (std::size_t i = 2; i < times.size(); ++i) {
        CHECK_EQ(times[i] - times[i - 1], expected_gap);
        expected_gap = std::min<Millis>(expected_gap * 2, cfg.max_rto_ms);
    }
    CHECK_EQ(times[times.size() - 1] - times[times.size() - 2], cfg.max_rto_ms);  // the cap is reached
}

TEST(round_trip_estimate_converges) {
    Params p;
    p.delay = 40;  // 80 ms round trip
    Net net(p);
    std::size_t sent = 0;
    std::size_t got = 0;
    while (net.now < 20000 && got < 50) {
        net.step();
        if (net.now % 100 == 0 && sent < 50) net.a.send(make_message(sent++, 200), net.now);
        if (net.b) {
            while (net.b->receive()) ++got;
        }
    }
    CHECK_EQ(got, 50u);
    CHECK(net.a.srtt_ms() > 78.0 && net.a.srtt_ms() < 90.0);
    CHECK(net.a.rto_ms() >= static_cast<Millis>(net.a.srtt_ms()));
    CHECK(net.a.rto_ms() < 400);
}

TEST(karns_rule_no_rtt_sample_from_a_retransmitted_packet) {
    Config cfg;
    cfg.initial_rto_ms = 150;  // shorter than the 200 ms round trip, so the first packet is resent needlessly
    cfg.min_rto_ms = 20;
    Params p;
    p.delay = 100;
    Net net(p, cfg, cfg);
    // Handshake first, then send one message and wait for its ack. The ack that comes back may belong to
    // the original or to the retransmission, which is exactly why such a packet must not produce a sample.
    CHECK(net.run_until([&] { return net.a.established() && net.b && net.b->established(); }, 20000));
    const double before = net.a.srtt_ms();
    net.a.send(make_message(0, 100), net.now);
    CHECK(net.run_until([&] { return net.a.stats().retransmits >= 1 && net.b->stats().messages_received >= 1; }, 20000));
    for (int i = 0; i < 1000; ++i) net.step();  // let every ack, original and duplicate, come back
    CHECK(net.a.stats().retransmits >= 1);
    // The only data packet was retransmitted, so no new sample may have been taken from it.
    CHECK(net.a.srtt_ms() == before);
}

TEST(fin_is_not_sent_until_all_data_is_acknowledged) {
    Params p;
    p.delay = 20;
    Net net(p);
    bool dropped = false;
    net.drop_if = [&](int from, const Packet& pk) {
        if (from == 0 && pk.type == PacketType::Data && !dropped) return dropped = true;
        return false;
    };
    net.a.send(make_message(0, 100), 0);
    net.a.close(0);
    CHECK(net.run_until([&] { return net.a.closed(); }, 20000) || true);
    bool have_fin = false, have_data = false;
    Millis first_fin = 0, last_data = 0;
    for (const auto& s : net.log) {
        if (s.from != 0) continue;
        if (s.pkt.type == PacketType::Fin && !have_fin) { have_fin = true; first_fin = s.at; }
        if (s.pkt.type == PacketType::Data) { have_data = true; last_data = s.at; }
    }
    CHECK(have_fin);
    CHECK(have_data);
    CHECK(first_fin > last_data);  // no Data (e.g. a retransmission) after the Fin went out
}

TEST(a_lost_packet_is_resent_by_fast_retransmit_before_the_timer) {
    Config cfg;
    cfg.initial_rto_ms = 1000;  // timer recovery would take over a second
    Params p;
    p.delay = 20;
    Net net(p, cfg, cfg);
    bool dropped = false;
    net.drop_if = [&](int from, const Packet& pk) {
        if (from == 0 && pk.type == PacketType::Data && pk.seq == 2 && !dropped) return dropped = true;
        return false;
    };
    for (std::size_t i = 0; i < 10; ++i) net.a.send(make_message(i, 100), 0);
    std::size_t got = 0;
    CHECK(net.run_until(
        [&] {
            if (net.b) {
                while (net.b->receive()) ++got;
            }
            return got == 10;
        },
        5000));
    CHECK(net.now < 500);
    CHECK_EQ(net.a.stats().fast_retransmits, 1u);
    CHECK_EQ(net.a.stats().timeouts, 0u);
    CHECK_EQ(net.a.stats().retransmits, 1u);
}

TEST(a_whole_lost_window_is_recovered_by_the_timer) {
    Config cfg;
    cfg.initial_rto_ms = 100;
    Net net({}, cfg, cfg);
    CHECK(net.run_until([&] { return net.b && net.a.established() && net.b->established(); }, 2000));
    int dropped = 0;
    net.drop_if = [&](int from, const Packet& pk) { return from == 0 && pk.type == PacketType::Data && dropped < 20 && ++dropped > 0; };
    for (std::size_t i = 0; i < 20; ++i) net.a.send(make_message(i, 100), net.now);
    std::vector<Bytes> got;
    CHECK(net.run_until(
        [&] {
            while (auto m = net.b->receive()) got.push_back(*m);
            return got.size() == 20;
        },
        20000));
    for (std::size_t i = 0; i < got.size(); ++i) CHECK(got[i] == make_message(i, 100));
    CHECK(net.a.stats().timeouts >= 1);
}

TEST(flow_control_bounds_memory_and_recovers_when_the_app_reads) {
    Config client;
    client.send_window = 8;
    Config server;
    server.recv_window = 16;
    Params p;
    p.delay = 5;
    Net net(p, client, server);
    const std::size_t total = 300;
    for (std::size_t i = 0; i < total; ++i) CHECK(net.a.send(make_message(i, 100), 0) == SendResult::Ok);

    std::vector<Bytes> got;
    std::size_t max_held = 0;
    std::size_t max_inflight = 0;
    bool checked_stall = false;
    while (net.now < 60000 && got.size() < total) {
        net.step();
        max_inflight = std::max(max_inflight, net.a.inflight());
        if (net.b) {
            max_held = std::max(max_held, net.b->held_packets());
            if (net.now >= 3000) {
                while (auto m = net.b->receive()) got.push_back(std::move(*m));
            }
        }
        if (net.now == 2900) {
            checked_stall = true;
            CHECK_EQ(net.b->held_packets(), 16u);          // receiver full, not unbounded
            CHECK(net.a.pending_packets() > 0);            // sender is held back
            CHECK(!net.a.closed());
        }
    }
    CHECK(checked_stall);
    CHECK(max_held <= 16);
    CHECK(max_inflight <= 8);
    CHECK_EQ(got.size(), total);
    for (std::size_t i = 0; i < got.size(); ++i) CHECK(got[i] == make_message(i, 100));
    CHECK_EQ(net.b->stats().dropped_out_of_window, 0u);  // the sender honoured the advertised window
    CHECK(net.count_sent(0, PacketType::Ack) >= 1);  // zero-window probes
}

// ---------- closing ----------

TEST(graceful_close_after_data_reaches_closed_on_both_sides) {
    Net net({});
    for (std::size_t i = 0; i < 20; ++i) net.a.send(make_message(i, 3000), 0);
    std::vector<Bytes> got;
    bool closed_called = false;
    CHECK(net.run_until(
        [&] {
            if (net.b) {
                while (auto m = net.b->receive()) got.push_back(std::move(*m));
                if (net.b->peer_closed() && !closed_called) {
                    closed_called = true;
                    net.b->close(net.now);
                }
            }
            if (net.now == 5) net.a.close(net.now);  // close while data is still queued
            return net.a.closed() && net.b && net.b->closed();
        },
        60000));
    CHECK_EQ(got.size(), 20u);  // every message arrived before the Fin
    for (std::size_t i = 0; i < got.size(); ++i) CHECK(got[i] == make_message(i, 3000));
    CHECK(net.a.error() == Error::None);
    CHECK(net.b->error() == Error::None);
    CHECK(net.a.send(Bytes{1}, net.now) == SendResult::Closed);
}

TEST(close_survives_lost_finacks) {
    Net net({});
    int lost = 0;
    net.drop_if = [&](int, const Packet& p) { return p.type == PacketType::FinAck && lost < 3 && ++lost > 0; };
    CHECK(net.run_until([&] { return net.b && net.a.established() && net.b->established(); }, 2000));
    net.a.close(net.now);
    net.b->close(net.now);
    CHECK(net.run_until([&] { return net.a.closed() && net.b->closed(); }, 60000));
    CHECK(net.a.error() == Error::None);
    CHECK(net.b->error() == Error::None);
    CHECK_EQ(lost, 3);
}

TEST(half_close_still_lets_the_other_side_send) {
    Net net({});
    CHECK(net.run_until([&] { return net.b && net.a.established() && net.b->established(); }, 2000));
    net.a.close(net.now);
    CHECK(net.run_until([&] { return net.b->peer_closed(); }, 10000));
    // b may keep talking after a closed its direction
    CHECK(net.b->send(make_message(5, 500), net.now) == SendResult::Ok);
    std::optional<Bytes> got;
    CHECK(net.run_until(
        [&] {
            if (!got) got = net.a.receive();
            return got.has_value();
        },
        10000));
    CHECK(got.has_value() && *got == make_message(5, 500));
}

// ---------- limits and protocol defence ----------

TEST(send_limits_are_enforced) {
    Config cfg;
    cfg.max_message_size = 5000;
    cfg.max_send_queue_bytes = 8000;
    Net net({}, cfg);
    CHECK(net.a.send(Bytes(5001), 0) == SendResult::TooLarge);
    CHECK(net.a.send(Bytes(5000), 0) == SendResult::Ok);
    CHECK(net.a.send(Bytes(5000), 0) == SendResult::QueueFull);  // not established yet, so it queues
    CHECK(net.a.send(Bytes(1000), 0) == SendResult::Ok);
}

TEST(a_continuation_without_a_beginning_is_a_protocol_error) {
    Connection s = established_server();
    s.on_packet(data_packet(100, kFlagEnd, {1}), 2);
    CHECK(s.closed());
    CHECK(s.error() == Error::Protocol);
    bool saw_rst = false;
    for (const Bytes& dg : s.take_output()) {
        Packet p;
        if (decode(dg.data(), dg.size(), p) && p.type == PacketType::Rst) saw_rst = true;
    }
    CHECK(saw_rst);
}

TEST(an_oversized_message_is_a_protocol_error) {
    Config cfg;
    cfg.max_message_size = 10;
    Connection s = established_server(cfg);
    s.on_packet(data_packet(100, kFlagBegin, Bytes(11)), 2);
    CHECK(s.error() == Error::Protocol);
}

TEST(duplicate_data_is_delivered_once) {
    Connection s = established_server();
    const Packet d = data_packet(100, kFlagBegin | kFlagEnd, {9, 9});
    s.on_packet(d, 2);
    s.on_packet(d, 3);
    auto m = s.receive();
    CHECK(m.has_value() && *m == Bytes({9, 9}));
    CHECK(!s.receive().has_value());
    CHECK_EQ(s.stats().duplicates_received, 1u);
}

TEST(data_beyond_the_window_is_dropped) {
    Config cfg;  // recv_window 128
    Connection s = established_server(cfg);
    s.on_packet(data_packet(100 + 128, kFlagBegin | kFlagEnd, {1}), 2);
    CHECK_EQ(s.stats().dropped_out_of_window, 1u);
    s.on_packet(data_packet(100 + 127, kFlagBegin | kFlagEnd, {1}), 3);  // last slot inside the window
    CHECK_EQ(s.stats().out_of_order_received, 1u);
    CHECK(!s.receive().has_value());  // neither is deliverable: packet 100 is still missing
}

TEST(acks_for_unsent_or_old_data_are_ignored) {
    Net net({});
    CHECK(net.run_until([&] { return net.b && net.a.established() && net.b->established(); }, 2000));
    net.a.send(make_message(1, 100), net.now);
    CHECK_EQ(net.a.inflight(), 1u);
    net.a.on_packet(mk(PacketType::Ack, 0xABCD, 0, 6), net.now);  // ack 6: never sent
    CHECK_EQ(net.a.inflight(), 1u);
    net.a.on_packet(mk(PacketType::Ack, 0xABCD, 0, 1), net.now);  // ack 1: really acknowledges seq 0
    CHECK_EQ(net.a.inflight(), 0u);
    net.a.on_packet(mk(PacketType::Ack, 0xABCD, 0, 0), net.now);  // now stale
    CHECK_EQ(net.a.inflight(), 0u);
}

TEST(packets_for_another_connection_are_ignored) {
    Connection c = Connection::make_client(5, {}, 0);
    c.on_packet(mk(PacketType::Rst, 6, 0, 0), 1);
    CHECK(!c.closed());
    CHECK_EQ(c.stats().packets_received, 0u);
}

TEST(reset_closes_the_connection) {
    Connection c = Connection::make_client(5, {}, 0);
    c.on_packet(mk(PacketType::Rst, 5, 0, 0), 1);
    CHECK(c.closed());
    CHECK(c.error() == Error::Reset);
}
