#include "tether/connection.hpp"

#include <algorithm>
#include <cmath>

namespace tether {
namespace {

// Sequence numbers wrap at 2^32, so ordering is decided by the signed difference.
inline bool seq_lt(std::uint32_t a, std::uint32_t b) { return static_cast<std::int32_t>(a - b) < 0; }

constexpr int kFastRetransmitThreshold = 3;  // SACKed packets above a hole before we resend it

}  // namespace

Connection::Connection(std::uint32_t id, const Config& cfg, bool is_client)
    : cfg_(cfg), id_(id), is_client_(is_client) {
    initial_seq_ = cfg.initial_seq;
    send_base_ = next_seq_ = peer_ack_ = initial_seq_;
    rto_ = cfg.initial_rto_ms;
    last_adv_ = advertised_window();
}

Connection Connection::make_client(std::uint32_t conn_id, const Config& cfg, Millis now) {
    Connection c(conn_id, cfg, true);
    c.state_ = State::SynSent;
    c.last_now_ = now;
    Packet syn = c.base(PacketType::Syn);
    syn.seq = c.initial_seq_;
    syn.ack = 0;
    c.emit(syn);
    c.start_ctrl(Ctrl::Syn, now);
    return c;
}

Connection Connection::make_server(const Packet& syn, const Config& cfg, Millis now) {
    Connection c(syn.conn_id, cfg, false);
    c.state_ = State::SynReceived;
    c.last_now_ = now;
    c.recv_next_ = syn.seq;
    c.peer_window_ = syn.window;
    c.start_ctrl(Ctrl::SynAck, now);
    c.resend_ctrl();
    return c;
}

std::uint16_t Connection::advertised_window() const {
    return held_pkts_ >= cfg_.recv_window ? 0 : static_cast<std::uint16_t>(cfg_.recv_window - held_pkts_);
}

Packet Connection::base(PacketType type, std::uint8_t flags) const {
    Packet p;
    p.type = type;
    p.flags = flags;
    p.window = advertised_window();
    p.conn_id = id_;
    p.seq = next_seq_;
    p.ack = recv_next_;
    p.sack = sack_bits();
    return p;
}

void Connection::emit(const Packet& packet) {
    out_.push_back(encode(packet));
    ++stats_.packets_sent;
}

std::uint32_t Connection::sack_bits() const {
    std::uint32_t bits = 0;
    if (recv_buf_.empty()) return bits;
    for (std::uint32_t i = 0; i < 32; ++i) {
        if (recv_buf_.count(recv_next_ + 1 + i) != 0) bits |= (1U << i);
    }
    return bits;
}

void Connection::send_ack() {
    Packet p = base(PacketType::Ack);
    last_adv_ = p.window;
    ack_needed_ = false;
    ++stats_.acks_sent;
    emit(p);
}

void Connection::start_ctrl(Ctrl kind, Millis now) {
    ctrl_kind_ = kind;
    ctrl_tries_ = 0;
    ctrl_rto_ = cfg_.initial_rto_ms;
    ctrl_deadline_ = now + ctrl_rto_;
}

void Connection::resend_ctrl() {
    switch (ctrl_kind_) {
        case Ctrl::Syn: {
            Packet p = base(PacketType::Syn);
            p.seq = initial_seq_;
            p.ack = 0;
            emit(p);
            break;
        }
        case Ctrl::SynAck: {
            Packet p = base(PacketType::SynAck);
            p.seq = initial_seq_;
            p.ack = recv_next_;
            emit(p);
            break;
        }
        case Ctrl::Fin: {
            emit(base(PacketType::Fin));
            break;
        }
        case Ctrl::None:
            break;
    }
}

void Connection::transmit(Outstanding& o, Millis now) {
    Packet p = base(PacketType::Data, o.flags);
    p.seq = o.seq;
    p.payload = o.payload;
    if (o.tx_count > 0) ++stats_.retransmits;
    o.sent_at = now;
    ++o.tx_count;
    ++stats_.data_packets_sent;
    last_adv_ = p.window;
    ack_needed_ = false;  // every Data packet carries the current ack, sack and window
    emit(p);
}

void Connection::pump(Millis now) {
    if (state_ != State::Established) return;
    while (!pending_.empty() && inflight_.size() < cfg_.send_window &&
           static_cast<std::uint32_t>(next_seq_ - peer_ack_) < peer_window_) {
        Segment seg = std::move(pending_.front());
        pending_.pop_front();
        pending_bytes_ -= seg.payload.size();
        Outstanding o;
        o.seq = next_seq_++;
        o.flags = seg.flags;
        o.payload = std::move(seg.payload);
        inflight_.push_back(std::move(o));
        transmit(inflight_.back(), now);
    }
    maybe_send_fin(now);
}

void Connection::maybe_send_fin(Millis now) {
    if (close_requested_ && !fin_sent_ && state_ == State::Established && pending_.empty() && inflight_.empty()) {
        fin_sent_ = true;
        start_ctrl(Ctrl::Fin, now);
        resend_ctrl();
    }
}

SendResult Connection::send(const Bytes& message, Millis now) {
    if (state_ == State::Closed || close_requested_) return SendResult::Closed;
    if (message.size() > cfg_.max_message_size) return SendResult::TooLarge;
    if (pending_bytes_ + message.size() > cfg_.max_send_queue_bytes) return SendResult::QueueFull;

    const std::size_t chunk = cfg_.max_payload;
    std::size_t offset = 0;
    do {
        const std::size_t n = std::min(chunk, message.size() - offset);
        Segment seg;
        seg.payload.assign(message.begin() + static_cast<std::ptrdiff_t>(offset),
                           message.begin() + static_cast<std::ptrdiff_t>(offset + n));
        if (offset == 0) seg.flags |= kFlagBegin;
        offset += n;
        if (offset >= message.size()) seg.flags |= kFlagEnd;
        pending_bytes_ += n;
        pending_.push_back(std::move(seg));
    } while (offset < message.size());

    ++stats_.messages_sent;
    stats_.bytes_sent += message.size();
    last_now_ = now;
    pump(now);
    return SendResult::Ok;
}

std::optional<Bytes> Connection::receive() {
    if (inbox_.empty()) return std::nullopt;
    InboxMessage m = std::move(inbox_.front());
    inbox_.pop_front();
    held_pkts_ -= m.packets;
    // Tell the sender when a closed window reopens, or when it has grown by half.
    const std::uint16_t now_window = advertised_window();
    if ((last_adv_ == 0 && now_window > 0) || (now_window > last_adv_ && now_window - last_adv_ >= cfg_.recv_window / 2)) {
        ack_needed_ = true;
    }
    return std::move(m.data);
}

void Connection::close(Millis now) {
    if (state_ == State::Closed) return;
    close_requested_ = true;
    last_now_ = now;
    pump(now);
}

void Connection::fail(Error e, bool send_rst) {
    if (send_rst) emit(base(PacketType::Rst));
    error_ = e;
    state_ = State::Closed;
    inflight_.clear();
    pending_.clear();
    pending_bytes_ = 0;
    ctrl_kind_ = Ctrl::None;
    probing_ = false;
}

void Connection::on_datagram(const std::uint8_t* data, std::size_t size, Millis now) {
    Packet p;
    if (!decode(data, size, p)) {
        ++stats_.bad_packets;
        return;
    }
    on_packet(p, now);
}

void Connection::on_packet(const Packet& p, Millis now) {
    last_now_ = now;
    if (p.conn_id != id_) return;
    ++stats_.packets_received;

    if (state_ == State::Closed) {
        // Our Fin_Ack may have been lost: keep answering a repeated Fin.
        if (p.type == PacketType::Fin) emit(base(PacketType::FinAck));
        return;
    }
    if (p.type == PacketType::Rst) {
        fail(Error::Reset, false);
        return;
    }

    switch (state_) {
        case State::SynSent:
            if (p.type == PacketType::SynAck) {
                recv_next_ = p.seq;
                peer_window_ = p.window;
                state_ = State::Established;
                ctrl_kind_ = Ctrl::None;
                ack_needed_ = true;
                pump(now);
            }
            return;
        case State::SynReceived:
            if (p.type == PacketType::Syn) {
                resend_ctrl();  // our SynAck was lost
                return;
            }
            if (p.type == PacketType::Ack || p.type == PacketType::Data || p.type == PacketType::Fin ||
                p.type == PacketType::FinAck) {
                state_ = State::Established;
                ctrl_kind_ = Ctrl::None;
                break;  // handle it as an established packet
            }
            return;
        case State::Established:
            break;
        case State::Closed:
            return;
    }

    probe_tries_ = 0;  // any sign of life from the peer
    switch (p.type) {
        case PacketType::Syn:
            break;  // a late duplicate
        case PacketType::SynAck:
            if (is_client_) ack_needed_ = true;  // our handshake Ack was lost
            break;
        case PacketType::Data:
            on_ack_fields(p, now);
            on_data(p);
            break;
        case PacketType::Ack:
            on_ack_fields(p, now);
            if ((p.flags & kFlagProbe) != 0) ack_needed_ = true;
            break;
        case PacketType::Fin:
            on_fin();
            break;
        case PacketType::FinAck:
            if (fin_sent_) {
                fin_acked_ = true;
                if (ctrl_kind_ == Ctrl::Fin) ctrl_kind_ = Ctrl::None;
                check_closed();
            }
            break;
        case PacketType::Rst:
            break;
    }
    if (state_ == State::Established) pump(now);
}

void Connection::on_ack_fields(const Packet& p, Millis now) {
    const std::uint32_t a = p.ack;
    if (seq_lt(a, send_base_)) return;   // stale or reordered acknowledgement
    if (seq_lt(next_seq_, a)) return;    // acknowledges data we never sent
    peer_ack_ = a;
    peer_window_ = p.window;
    if (peer_window_ == 0) {
        if (!probing_) {
            probing_ = true;
            probe_interval_ = rto_;
            probe_deadline_ = now + probe_interval_;
            probe_tries_ = 0;
        }
    } else {
        probing_ = false;
    }

    bool have_sample = false;
    Millis sample_sent_at = 0;
    auto consider = [&](const Outstanding& o) {
        if (o.tx_count == 1 && (!have_sample || o.sent_at > sample_sent_at)) {
            have_sample = true;
            sample_sent_at = o.sent_at;
        }
    };

    while (!inflight_.empty() && seq_lt(inflight_.front().seq, a)) {
        if (!inflight_.front().sacked) consider(inflight_.front());
        inflight_.pop_front();
    }
    send_base_ = a;

    for (std::uint32_t i = 0; i < 32; ++i) {
        if ((p.sack & (1U << i)) == 0) continue;
        const std::uint32_t idx = i + 1;  // inflight_[0] has seq == a
        if (idx < inflight_.size() && !inflight_[idx].sacked) {
            inflight_[idx].sacked = true;
            consider(inflight_[idx]);
        }
    }
    if (have_sample && now >= sample_sent_at) update_rtt(static_cast<double>(now - sample_sent_at));

    // Fast retransmit: a packet with enough SACKed packets above it is presumed lost.
    int sacked_above = 0;
    for (std::size_t i = inflight_.size(); i-- > 0;) {
        Outstanding& o = inflight_[i];
        if (o.sacked) {
            ++sacked_above;
        } else if (sacked_above >= kFastRetransmitThreshold && !o.fast_retx) {
            o.fast_retx = true;
            ++stats_.fast_retransmits;
            transmit(o, now);
        }
    }
}

void Connection::update_rtt(double r) {
    if (!have_rtt_) {
        srtt_ = r;
        rttvar_ = r / 2.0;
        have_rtt_ = true;
    } else {
        rttvar_ = 0.75 * rttvar_ + 0.25 * std::fabs(srtt_ - r);
        srtt_ = 0.875 * srtt_ + 0.125 * r;
    }
    const double rto = srtt_ + std::max(1.0, 4.0 * rttvar_);
    const auto clamped = static_cast<Millis>(std::ceil(rto));
    rto_ = std::min(std::max(clamped, cfg_.min_rto_ms), cfg_.max_rto_ms);
}

bool Connection::deliver(std::uint8_t flags, const Bytes& payload) {
    if ((flags & kFlagBegin) != 0) {
        if (partial_active_) return false;  // a new message began before the last one ended
        partial_active_ = true;
        partial_.clear();
        partial_pkts_ = 0;
    } else if (!partial_active_) {
        return false;  // continuation with no beginning
    }
    partial_.insert(partial_.end(), payload.begin(), payload.end());
    ++partial_pkts_;
    if (partial_.size() > cfg_.max_message_size) return false;
    if ((flags & kFlagEnd) != 0) {
        InboxMessage m;
        m.packets = partial_pkts_;
        stats_.bytes_received += partial_.size();
        ++stats_.messages_received;
        m.data = std::move(partial_);
        held_pkts_ += m.packets;
        inbox_.push_back(std::move(m));
        partial_ = Bytes();
        partial_pkts_ = 0;
        partial_active_ = false;
    }
    return true;
}

void Connection::on_data(const Packet& p) {
    ack_needed_ = true;
    const std::int32_t d = static_cast<std::int32_t>(p.seq - recv_next_);
    if (d < 0) {
        ++stats_.duplicates_received;  // already delivered; the repeated ack tells the sender
        return;
    }
    if (static_cast<std::uint32_t>(d) >= advertised_window()) {
        ++stats_.dropped_out_of_window;
        return;
    }
    if (d > 0) {
        if (recv_buf_.count(p.seq) != 0) {
            ++stats_.duplicates_received;
        } else {
            ++stats_.out_of_order_received;
            recv_buf_[p.seq] = RecvEntry{p.flags, p.payload};
        }
        return;
    }
    if (!deliver(p.flags, p.payload)) {
        fail(Error::Protocol, true);
        return;
    }
    ++recv_next_;
    for (auto it = recv_buf_.find(recv_next_); it != recv_buf_.end(); it = recv_buf_.find(recv_next_)) {
        const bool ok = deliver(it->second.flags, it->second.payload);
        recv_buf_.erase(it);
        if (!ok) {
            fail(Error::Protocol, true);
            return;
        }
        ++recv_next_;
    }
}

void Connection::on_fin() {
    peer_fin_ = true;
    emit(base(PacketType::FinAck));
    check_closed();
}

void Connection::check_closed() {
    if (fin_sent_ && fin_acked_ && peer_fin_ && state_ == State::Established) {
        state_ = State::Closed;
        ctrl_kind_ = Ctrl::None;
        probing_ = false;
    }
}

void Connection::check_rto(Millis now) {
    if (inflight_.empty() || peer_window_ == 0) return;
    bool expired = false;
    for (const auto& o : inflight_) {
        if (!o.sacked && now >= o.sent_at + rto_) {
            expired = true;
            break;
        }
    }
    if (!expired) return;
    const Millis rto_at_expiry = rto_;
    for (auto& o : inflight_) {
        if (o.sacked || now < o.sent_at + rto_at_expiry) continue;
        if (o.tx_count > cfg_.max_retries) {
            fail(Error::Timeout, false);
            return;
        }
        transmit(o, now);
    }
    ++stats_.timeouts;
    rto_ = std::min(rto_ * 2, cfg_.max_rto_ms);  // exponential backoff until a fresh RTT sample
}

void Connection::check_probe(Millis now) {
    if (!probing_) return;
    if (pending_.empty() && inflight_.empty()) {
        probing_ = false;
        return;
    }
    if (now < probe_deadline_) return;
    if (probe_tries_ >= cfg_.max_retries) {
        fail(Error::Timeout, false);
        return;
    }
    emit(base(PacketType::Ack, kFlagProbe));
    ++stats_.acks_sent;
    ++probe_tries_;
    probe_interval_ = std::min(probe_interval_ * 2, cfg_.max_rto_ms);
    probe_deadline_ = now + probe_interval_;
}

void Connection::tick(Millis now) {
    last_now_ = now;
    if (state_ == State::Closed) return;

    if (ctrl_kind_ != Ctrl::None && now >= ctrl_deadline_) {
        if (ctrl_tries_ >= cfg_.max_retries) {
            fail(Error::Timeout, false);
            return;
        }
        resend_ctrl();
        ++ctrl_tries_;
        ctrl_rto_ = std::min(ctrl_rto_ * 2, cfg_.max_rto_ms);
        ctrl_deadline_ = now + ctrl_rto_;
    }
    if (state_ == State::Established) {
        check_rto(now);
        if (state_ == State::Closed) return;
        check_probe(now);
        if (state_ == State::Closed) return;
        pump(now);
    }
}

std::optional<Millis> Connection::next_deadline() const {
    if (state_ == State::Closed) return std::nullopt;
    std::optional<Millis> best;
    auto consider = [&](Millis t) {
        if (!best || t < *best) best = t;
    };
    if (ctrl_kind_ != Ctrl::None) consider(ctrl_deadline_);
    if (state_ == State::Established) {
        if (peer_window_ > 0) {
            for (const auto& o : inflight_) {
                if (!o.sacked) consider(o.sent_at + rto_);
            }
        }
        if (probing_) consider(probe_deadline_);
    }
    return best;
}

std::vector<Bytes> Connection::take_output() {
    if (ack_needed_ && state_ == State::Established) send_ack();
    std::vector<Bytes> result;
    result.swap(out_);
    return result;
}

}  // namespace tether
