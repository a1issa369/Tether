#pragma once
#include <chrono>
#include <deque>
#include <memory>
#include <optional>
#include <unordered_map>

#include "tether/connection.hpp"
#include "tether/udp.hpp"

namespace tether {

class Endpoint;

// One connection to one peer. Sessions are created by Endpoint::connect and Endpoint::accept and must
// not outlive their Endpoint.
class Session {
public:
    SendResult send(const Bytes& message);
    std::optional<Bytes> receive();
    void close();

    State state() const { return conn_.state(); }
    Error error() const { return conn_.error(); }
    bool established() const { return conn_.established(); }
    bool closed() const { return conn_.closed(); }
    bool peer_closed() const { return conn_.peer_closed(); }
    const Address& peer() const { return peer_; }
    const Stats& stats() const { return conn_.stats(); }
    double srtt_ms() const { return conn_.srtt_ms(); }
    Millis rto_ms() const { return conn_.rto_ms(); }

private:
    friend class Endpoint;
    Session(Endpoint* ep, const Address& peer, Connection conn) : ep_(ep), peer_(peer), conn_(std::move(conn)) {}
    Endpoint* ep_;
    Address peer_;
    Connection conn_;
    Millis closed_at_ = 0;
    bool seen_closed_ = false;
};

// A UDP socket plus every session on it. Single threaded: call poll() in a loop.
class Endpoint {
public:
    explicit Endpoint(Config cfg = {}, std::uint16_t port = 0);

    std::uint16_t port() const { return socket_.port(); }
    UdpSocket& socket() { return socket_; }
    std::shared_ptr<Session> connect(const Address& peer);
    std::shared_ptr<Session> accept();  // a newly accepted session, or nullptr
    // Wait up to timeout_ms for packets or timers, process everything that is ready, send what is due.
    void poll(int timeout_ms);
    std::size_t session_count() const { return sessions_.size(); }
    std::uint64_t bad_datagrams() const { return bad_datagrams_; }
    Millis now() const;

private:
    friend class Session;
    struct Key {
        Address addr;
        std::uint32_t id;
        bool operator==(const Key& o) const { return id == o.id && addr == o.addr; }
    };
    struct KeyHash {
        std::size_t operator()(const Key& k) const { return AddressHash()(k.addr) * 31u + k.id; }
    };

    void flush(Session& s);
    void dispatch(const Address& from, const Bytes& datagram);
    void reply_stateless(const Address& to, PacketType type, std::uint32_t conn_id);
    Config session_config();

    Config cfg_;
    UdpSocket socket_;
    std::chrono::steady_clock::time_point start_;
    std::unordered_map<Key, std::shared_ptr<Session>, KeyHash> sessions_;
    std::deque<std::shared_ptr<Session>> accept_queue_;
    std::uint64_t rng_state_;
    std::uint64_t bad_datagrams_ = 0;
};

}  // namespace tether
