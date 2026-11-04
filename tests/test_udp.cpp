#include <chrono>
#include <memory>

#include "sim.hpp"
#include "tether/endpoint.hpp"
#include "test_main.hpp"

using namespace tether;

namespace {

Address loopback(std::uint16_t port) { return *Address::resolve("127.0.0.1", port); }

double seconds_since(std::chrono::steady_clock::time_point t) {
    return std::chrono::duration<double>(std::chrono::steady_clock::now() - t).count();
}

template <typename Pred>
bool spin(Endpoint& a, Endpoint& b, Pred done, double limit_s) {
    const auto t0 = std::chrono::steady_clock::now();
    while (seconds_since(t0) < limit_s) {
        a.poll(2);
        b.poll(2);
        if (done()) return true;
    }
    return done();
}

Config server_config() {
    Config c;
    c.accept_connections = true;
    return c;
}

}  // namespace

TEST(udp_echo_over_loopback) {
    Endpoint server(server_config());
    Endpoint client;
    auto cs = client.connect(loopback(server.port()));
    std::shared_ptr<Session> ss;
    CHECK(spin(client, server, [&] {
        if (!ss) ss = server.accept();
        return cs->established() && ss && ss->established();
    }, 5));
    for (int i = 0; i < 5; ++i) {
        const Bytes msg = sim::make_message(i, 100 + i * 50);
        CHECK(cs->send(msg) == SendResult::Ok);
        std::optional<Bytes> at_server;
        CHECK(spin(client, server, [&] {
            if (!at_server) at_server = ss->receive();
            return at_server.has_value();
        }, 5));
        CHECK(at_server.has_value() && *at_server == msg);
        ss->send(*at_server);
        std::optional<Bytes> echoed;
        CHECK(spin(client, server, [&] {
            if (!echoed) echoed = cs->receive();
            return echoed.has_value();
        }, 5));
        CHECK(echoed.has_value() && *echoed == msg);
    }
    cs->close();
    CHECK(spin(client, server, [&] {
        if (ss->peer_closed() && !ss->closed()) ss->close();
        return cs->closed() && ss->closed();
    }, 5));
    CHECK(cs->error() == Error::None);
    CHECK(ss->error() == Error::None);
}

TEST(udp_transfer_with_twenty_percent_loss_each_way) {
    Endpoint server(server_config());
    Endpoint client;
    server.socket().set_impairment(0.20, 0.05, 1);
    client.socket().set_impairment(0.20, 0.05, 2);
    auto cs = client.connect(loopback(server.port()));
    std::shared_ptr<Session> ss;

    const std::size_t message_count = 200;
    std::vector<Bytes> sent;
    for (std::size_t i = 0; i < message_count; ++i) sent.push_back(sim::make_message(i, 4096));

    std::vector<Bytes> received;
    std::size_t next = 0;
    const bool ok = spin(client, server, [&] {
        if (!ss) ss = server.accept();
        while (next < sent.size() && cs->send(sent[next]) == SendResult::Ok) ++next;
        if (ss) {
            while (auto m = ss->receive()) received.push_back(std::move(*m));
        }
        return received.size() == message_count;
    }, 60);
    CHECK(ok);
    CHECK(received == sent);  // 800 KB, byte for byte and in order
    CHECK(cs->stats().retransmits > 0);
    CHECK(client.socket().datagrams_dropped() > 0);
}

TEST(udp_unknown_connection_gets_a_reset) {
    Endpoint server(server_config());
    UdpSocket raw;
    Packet p;
    p.type = PacketType::Data;
    p.flags = kFlagBegin | kFlagEnd;
    p.conn_id = 12345;
    p.payload = {1, 2, 3};
    CHECK(raw.send_to(loopback(server.port()), encode(p)));
    server.poll(50);
    std::optional<Packet> reply;
    for (int i = 0; i < 100 && !reply; ++i) {
        raw.wait_readable(10);
        if (auto d = raw.receive()) {
            Packet r;
            if (decode(d->second.data(), d->second.size(), r)) reply = r;
        }
    }
    CHECK(reply.has_value());
    CHECK(reply && reply->type == PacketType::Rst);
    CHECK(reply && reply->conn_id == 12345u);
}

TEST(udp_garbage_is_counted_and_ignored) {
    Endpoint server(server_config());
    UdpSocket raw;
    CHECK(raw.send_to(loopback(server.port()), Bytes{1, 2, 3}));
    CHECK(raw.send_to(loopback(server.port()), Bytes(100, 0xAB)));
    for (int i = 0; i < 5; ++i) server.poll(10);
    CHECK_EQ(server.bad_datagrams(), 2u);
    CHECK_EQ(server.session_count(), 0u);
}

TEST(udp_server_that_does_not_listen_ignores_connections) {
    Config server_cfg;  // accept_connections defaults to false
    Endpoint server(server_cfg);
    Config client_cfg;
    client_cfg.initial_rto_ms = 20;
    client_cfg.max_retries = 3;
    Endpoint client(client_cfg);
    auto cs = client.connect(loopback(server.port()));
    CHECK(spin(client, server, [&] { return cs->closed(); }, 10));
    CHECK(cs->error() == Error::Timeout);
    CHECK(!server.accept());
}

TEST(udp_reset_stops_a_client_talking_to_a_forgotten_session) {
    auto server = std::make_unique<Endpoint>(server_config());
    const std::uint16_t port = server->port();
    Endpoint client;
    auto cs = client.connect(loopback(port));
    std::shared_ptr<Session> ss;
    CHECK(spin(client, *server, [&] {
        if (!ss) ss = server->accept();
        return cs->established() && ss && ss->established();
    }, 5));
    // The server process "restarts": a new endpoint on the same port knows nothing about the session.
    ss.reset();
    server.reset();
    server = std::make_unique<Endpoint>(server_config(), port);
    CHECK(cs->send(Bytes{1, 2, 3}) == SendResult::Ok);
    CHECK(spin(client, *server, [&] { return cs->closed(); }, 5));
    CHECK(cs->error() == Error::Reset);
}
