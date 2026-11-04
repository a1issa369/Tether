#include "tether/endpoint.hpp"

#include <algorithm>
#include <random>

namespace tether {

SendResult Session::send(const Bytes& message) {
    const SendResult r = conn_.send(message, ep_->now());
    ep_->flush(*this);
    return r;
}

std::optional<Bytes> Session::receive() {
    auto m = conn_.receive();
    ep_->flush(*this);  // a reopened window should reach the sender promptly
    return m;
}

void Session::close() {
    conn_.close(ep_->now());
    ep_->flush(*this);
}

Endpoint::Endpoint(Config cfg, std::uint16_t port)
    : cfg_(cfg), socket_(port), start_(std::chrono::steady_clock::now()) {
    std::random_device rd;
    rng_state_ = (static_cast<std::uint64_t>(rd()) << 32) ^ rd() ^ 0x9E3779B97F4A7C15ULL;
    if (rng_state_ == 0) rng_state_ = 1;
}

Millis Endpoint::now() const {
    return static_cast<Millis>(
        std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start_).count());
}

Config Endpoint::session_config() {
    rng_state_ ^= rng_state_ >> 12;
    rng_state_ ^= rng_state_ << 25;
    rng_state_ ^= rng_state_ >> 27;
    Config c = cfg_;
    c.initial_seq = static_cast<std::uint32_t>((rng_state_ * 2685821657736338717ULL) >> 32);
    return c;
}

std::shared_ptr<Session> Endpoint::connect(const Address& peer) {
    std::uint32_t id = 0;
    do {
        rng_state_ ^= rng_state_ >> 12;
        rng_state_ ^= rng_state_ << 25;
        rng_state_ ^= rng_state_ >> 27;
        id = static_cast<std::uint32_t>((rng_state_ * 2685821657736338717ULL) >> 32);
    } while (id == 0 || sessions_.count(Key{peer, id}) != 0);
    auto session = std::shared_ptr<Session>(new Session(this, peer, Connection::make_client(id, session_config(), now())));
    sessions_[Key{peer, id}] = session;
    flush(*session);
    return session;
}

std::shared_ptr<Session> Endpoint::accept() {
    if (accept_queue_.empty()) return nullptr;
    auto s = accept_queue_.front();
    accept_queue_.pop_front();
    return s;
}

void Endpoint::flush(Session& s) {
    for (const Bytes& datagram : s.conn_.take_output()) socket_.send_to(s.peer_, datagram);
}

void Endpoint::reply_stateless(const Address& to, PacketType type, std::uint32_t conn_id) {
    Packet p;
    p.type = type;
    p.conn_id = conn_id;
    socket_.send_to(to, encode(p));
}

void Endpoint::dispatch(const Address& from, const Bytes& datagram) {
    Packet p;
    if (!decode(datagram.data(), datagram.size(), p)) {
        ++bad_datagrams_;
        return;
    }
    auto it = sessions_.find(Key{from, p.conn_id});
    if (it != sessions_.end()) {
        it->second->conn_.on_packet(p, now());
        return;
    }
    if (p.type == PacketType::Syn) {
        if (!cfg_.accept_connections || sessions_.size() >= cfg_.max_connections) return;
        auto session = std::shared_ptr<Session>(new Session(this, from, Connection::make_server(p, session_config(), now())));
        sessions_[Key{from, p.conn_id}] = session;
        accept_queue_.push_back(session);
        return;
    }
    // Unknown connection. A repeated Fin after we forgot the session still deserves its FinAck so the
    // peer can finish closing; anything else gets a Rst so the peer stops talking to a ghost.
    if (p.type == PacketType::Fin) {
        reply_stateless(from, PacketType::FinAck, p.conn_id);
    } else if (p.type == PacketType::Data || p.type == PacketType::Ack) {
        reply_stateless(from, PacketType::Rst, p.conn_id);
    }
}

void Endpoint::poll(int timeout_ms) {
    for (auto& kv : sessions_) flush(*kv.second);

    int wait = timeout_ms;
    const Millis t0 = now();
    for (auto& kv : sessions_) {
        if (auto dl = kv.second->conn_.next_deadline()) {
            const Millis until = *dl > t0 ? *dl - t0 : 0;
            wait = std::min<int>(wait, static_cast<int>(std::min<Millis>(until, 1000000)));
        }
    }
    socket_.wait_readable(std::max(wait, 0));

    for (int budget = 4096; budget > 0; --budget) {
        auto datagram = socket_.receive();
        if (!datagram) break;
        dispatch(datagram->first, datagram->second);
    }

    const Millis t = now();
    for (auto it = sessions_.begin(); it != sessions_.end();) {
        Session& s = *it->second;
        s.conn_.tick(t);
        flush(s);
        if (s.conn_.closed()) {
            if (!s.seen_closed_) {
                s.seen_closed_ = true;
                s.closed_at_ = t;
            } else if (t - s.closed_at_ >= cfg_.linger_ms) {
                it = sessions_.erase(it);
                continue;
            }
        }
        ++it;
    }
}

}  // namespace tether
