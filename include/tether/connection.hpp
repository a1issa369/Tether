#pragma once
#include <cstddef>
#include <cstdint>
#include <deque>
#include <optional>
#include <unordered_map>
#include <vector>

#include "tether/packet.hpp"

namespace tether {

using Millis = std::uint64_t;

struct Config {
    std::size_t max_payload = 1200;        // bytes of message data per Data packet
    std::uint16_t send_window = 64;        // max unacknowledged Data packets in flight
    std::uint16_t recv_window = 128;       // packets of completed messages we will hold for the app
    Millis initial_rto_ms = 250;           // retransmission timeout before any RTT sample
    Millis min_rto_ms = 20;
    Millis max_rto_ms = 3000;
    unsigned max_retries = 10;             // retransmissions of one packet before the connection fails
    std::size_t max_message_size = 4u << 20;
    std::size_t max_send_queue_bytes = 8u << 20;
    std::uint32_t initial_seq = 0;         // first sequence number we send (random in real use)
    // Endpoint-level settings (ignored by Connection itself)
    bool accept_connections = false;
    std::size_t max_connections = 1024;
    Millis linger_ms = 2000;               // keep a closed session around to answer a repeated Fin
};

enum class State { SynSent, SynReceived, Established, Closed };
enum class Error { None, Timeout, Reset, Protocol };
enum class SendResult { Ok, Closed, TooLarge, QueueFull };

struct Stats {
    std::uint64_t packets_sent = 0;
    std::uint64_t data_packets_sent = 0;
    std::uint64_t retransmits = 0;       // every repeated Data packet, fast or timer driven
    std::uint64_t fast_retransmits = 0;  // retransmits triggered by SACK gaps, not by the timer
    std::uint64_t timeouts = 0;          // retransmission timer expiries
    std::uint64_t acks_sent = 0;
    std::uint64_t packets_received = 0;
    std::uint64_t duplicates_received = 0;
    std::uint64_t out_of_order_received = 0;
    std::uint64_t dropped_out_of_window = 0;
    std::uint64_t bad_packets = 0;
    std::uint64_t messages_sent = 0;
    std::uint64_t messages_received = 0;
    std::uint64_t bytes_sent = 0;        // message bytes accepted from the app
    std::uint64_t bytes_received = 0;    // message bytes delivered to the app
};

// One end of a tether connection. It is a pure state machine: it never touches a socket or a
// clock. Feed it datagrams and the current time, and collect the datagrams it wants sent. That
// is what lets the tests run it against a simulated network with loss, reordering and corruption.
class Connection {
public:
    static Connection make_client(std::uint32_t conn_id, const Config& cfg, Millis now);
    static Connection make_server(const Packet& syn, const Config& cfg, Millis now);

    void on_datagram(const std::uint8_t* data, std::size_t size, Millis now);
    void on_packet(const Packet& packet, Millis now);
    void tick(Millis now);
    // Earliest time tick() has work to do, if any.
    std::optional<Millis> next_deadline() const;
    // Datagrams ready to transmit. Also emits a pending acknowledgement, so call it after
    // on_packet/tick/receive.
    std::vector<Bytes> take_output();

    SendResult send(const Bytes& message, Millis now);
    std::optional<Bytes> receive();
    // Finish sending queued data, then close our direction. Receiving continues until the peer closes.
    void close(Millis now);

    State state() const { return state_; }
    Error error() const { return error_; }
    std::uint32_t id() const { return id_; }
    bool established() const { return state_ == State::Established; }
    bool closed() const { return state_ == State::Closed; }
    bool peer_closed() const { return peer_fin_ && inbox_.empty(); }
    const Stats& stats() const { return stats_; }
    double srtt_ms() const { return have_rtt_ ? srtt_ : 0.0; }
    Millis rto_ms() const { return rto_; }
    std::size_t inflight() const { return inflight_.size(); }
    std::size_t pending_packets() const { return pending_.size(); }
    std::size_t held_packets() const { return held_pkts_; }
    std::uint16_t advertised_window() const;
    std::uint16_t peer_window() const { return peer_window_; }

private:
    Connection(std::uint32_t id, const Config& cfg, bool is_client);

    struct Outstanding {
        std::uint32_t seq = 0;
        std::uint8_t flags = 0;
        Bytes payload;
        Millis sent_at = 0;
        unsigned tx_count = 0;
        bool sacked = false;
        bool fast_retx = false;
    };
    struct Segment {
        std::uint8_t flags = 0;
        Bytes payload;
    };
    struct RecvEntry {
        std::uint8_t flags = 0;
        Bytes payload;
    };
    struct InboxMessage {
        Bytes data;
        std::size_t packets = 0;
    };
    enum class Ctrl { None, Syn, SynAck, Fin };

    Packet base(PacketType type, std::uint8_t flags = 0) const;
    void emit(const Packet& packet);
    std::uint32_t sack_bits() const;
    void send_ack();
    void transmit(Outstanding& o, Millis now);
    void pump(Millis now);
    void maybe_send_fin(Millis now);
    void start_ctrl(Ctrl kind, Millis now);
    void resend_ctrl();
    void on_ack_fields(const Packet& p, Millis now);
    void on_data(const Packet& p);
    bool deliver(std::uint8_t flags, const Bytes& payload);
    void on_fin();
    void check_closed();
    void check_rto(Millis now);
    void check_probe(Millis now);
    void update_rtt(double sample_ms);
    void fail(Error e, bool send_rst);

    Config cfg_;
    std::uint32_t id_;
    bool is_client_;
    State state_ = State::SynSent;
    Error error_ = Error::None;
    Millis last_now_ = 0;

    // sender
    std::uint32_t initial_seq_ = 0;
    std::uint32_t send_base_ = 0;  // oldest unacknowledged sequence number
    std::uint32_t next_seq_ = 0;
    std::deque<Outstanding> inflight_;
    std::deque<Segment> pending_;
    std::size_t pending_bytes_ = 0;
    std::uint32_t peer_ack_ = 0;
    std::uint16_t peer_window_ = 0;
    bool close_requested_ = false;
    bool fin_sent_ = false;
    bool fin_acked_ = false;
    bool peer_fin_ = false;

    // round trip estimate (RFC 6298 style)
    bool have_rtt_ = false;
    double srtt_ = 0;
    double rttvar_ = 0;
    Millis rto_ = 0;

    // receiver
    std::uint32_t recv_next_ = 0;
    std::unordered_map<std::uint32_t, RecvEntry> recv_buf_;
    Bytes partial_;
    std::size_t partial_pkts_ = 0;
    bool partial_active_ = false;
    std::deque<InboxMessage> inbox_;
    std::size_t held_pkts_ = 0;
    bool ack_needed_ = false;
    std::uint16_t last_adv_ = 0;

    // handshake and close retransmission
    Ctrl ctrl_kind_ = Ctrl::None;
    Millis ctrl_deadline_ = 0;
    Millis ctrl_rto_ = 0;
    unsigned ctrl_tries_ = 0;

    // zero-window probing
    bool probing_ = false;
    Millis probe_deadline_ = 0;
    Millis probe_interval_ = 0;
    unsigned probe_tries_ = 0;

    std::vector<Bytes> out_;
    Stats stats_;
};

}  // namespace tether
