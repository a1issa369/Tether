#pragma once
// A deterministic simulated network between two Connections: loss, duplication, corruption and
// jittered (hence reordered) delivery, all driven by a seeded RNG and a virtual millisecond clock.
#include <functional>
#include <optional>
#include <queue>
#include <random>
#include <vector>

#include "tether/connection.hpp"

namespace sim {

using namespace tether;

struct Params {
    double loss = 0.0;
    double dup = 0.0;
    double corrupt = 0.0;
    Millis delay = 10;   // one-way base delay
    Millis jitter = 0;   // extra random delay in [0, jitter]; jitter is what reorders packets
    std::uint32_t seed = 1;
};

struct Sent {
    Millis at;
    int from;  // 0 = client, 1 = server
    Packet pkt;
    bool dropped;
};

class Net {
public:
    Net(Params p, Config client_cfg = {}, Config server_cfg = {}, std::uint32_t conn_id = 0xABCD)
        : params(p),
          a(Connection::make_client(conn_id, client_cfg, 0)),
          rng_(p.seed),
          server_cfg_(server_cfg) {}

    Params params;
    Connection a;
    std::optional<Connection> b;
    Millis now = 0;
    // Return true to drop a packet (evaluated before random loss). `from` is 0 for client, 1 for server.
    std::function<bool(int from, const Packet&)> drop_if;
    std::vector<Sent> log;

    void step() {
        ++now;
        while (!queue_.empty() && queue_.top().at <= now) {
            InFlight f = queue_.top();
            queue_.pop();
            deliver(f);
        }
        a.tick(now);
        if (b) b->tick(now);
        collect(0, a);
        if (b) collect(1, *b);
    }

    template <typename Pred>
    bool run_until(Pred done, Millis limit_ms) {
        const Millis end = now + limit_ms;
        while (now < end) {
            step();
            if (done()) return true;
        }
        return done();
    }

    std::size_t count_sent(int from, PacketType type) const {
        std::size_t n = 0;
        for (const auto& s : log) {
            if (s.from == from && s.pkt.type == type) ++n;
        }
        return n;
    }

private:
    struct InFlight {
        Millis at;
        std::uint64_t order;
        int to;
        Bytes data;
        bool operator<(const InFlight& o) const { return at != o.at ? at > o.at : order > o.order; }
    };

    double uniform() { return std::uniform_real_distribution<double>(0.0, 1.0)(rng_); }

    void deliver(const InFlight& f) {
        if (f.to == 0) {
            a.on_datagram(f.data.data(), f.data.size(), now);
            return;
        }
        if (!b) {
            Packet p;
            if (decode(f.data.data(), f.data.size(), p) && p.type == PacketType::Syn) {
                b = Connection::make_server(p, server_cfg_, now);
            }
            return;
        }
        b->on_datagram(f.data.data(), f.data.size(), now);
    }

    void collect(int from, Connection& c) {
        for (Bytes& dg : c.take_output()) {
            Packet p;
            if (!decode(dg.data(), dg.size(), p)) continue;
            const bool dropped = (drop_if && drop_if(from, p)) || uniform() < params.loss;
            log.push_back({now, from, p, dropped});
            if (dropped) continue;
            const int copies = uniform() < params.dup ? 2 : 1;
            for (int i = 0; i < copies; ++i) {
                Bytes data = dg;
                if (uniform() < params.corrupt) {
                    const auto at = std::uniform_int_distribution<std::size_t>(0, data.size() - 1)(rng_);
                    data[at] ^= static_cast<std::uint8_t>(1u << std::uniform_int_distribution<int>(0, 7)(rng_));
                }
                const Millis jitter = params.jitter == 0 ? 0 : std::uniform_int_distribution<Millis>(0, params.jitter)(rng_);
                queue_.push({now + params.delay + jitter, order_++, 1 - from, std::move(data)});
            }
        }
    }

    std::mt19937_64 rng_;
    Config server_cfg_;
    std::priority_queue<InFlight> queue_;
    std::uint64_t order_ = 0;
};

// Deterministic pseudo-random message whose content depends on (index, size).
inline Bytes make_message(std::size_t index, std::size_t size) {
    Bytes m(size);
    std::uint32_t x = static_cast<std::uint32_t>(index) * 2654435761u + 12345u;
    for (std::size_t i = 0; i < size; ++i) {
        x = x * 1664525u + 1013904223u;
        m[i] = static_cast<std::uint8_t>(x >> 24);
    }
    return m;
}

}  // namespace sim
