#pragma once
#include <netinet/in.h>

#include <cstdint>
#include <optional>
#include <string>
#include <utility>

#include "tether/packet.hpp"

namespace tether {

// IPv4 address and port.
class Address {
public:
    Address() = default;
    static std::optional<Address> resolve(const std::string& host, std::uint16_t port);
    static Address from_sockaddr(const sockaddr_in& sa);

    const sockaddr_in& raw() const { return sa_; }
    std::uint16_t port() const;
    std::string str() const;
    bool operator==(const Address& o) const;
    bool operator!=(const Address& o) const { return !(*this == o); }

private:
    sockaddr_in sa_{};
};

struct AddressHash {
    std::size_t operator()(const Address& a) const;
};

// Non-blocking UDP socket. For demos and tests it can drop or duplicate outgoing datagrams.
class UdpSocket {
public:
    explicit UdpSocket(std::uint16_t port = 0);  // binds 0.0.0.0:port, 0 picks a free port
    ~UdpSocket();
    UdpSocket(const UdpSocket&) = delete;
    UdpSocket& operator=(const UdpSocket&) = delete;
    UdpSocket(UdpSocket&& other) noexcept;
    UdpSocket& operator=(UdpSocket&& other) noexcept;

    std::uint16_t port() const { return port_; }
    bool send_to(const Address& to, const Bytes& data);
    std::optional<std::pair<Address, Bytes>> receive();  // nullopt when nothing is waiting
    bool wait_readable(int timeout_ms);

    // Each outgoing datagram is dropped with probability `loss` and, if sent, duplicated with `dup`.
    void set_impairment(double loss, double dup, std::uint32_t seed);
    std::uint64_t datagrams_sent() const { return sent_; }
    std::uint64_t datagrams_dropped() const { return dropped_; }

private:
    void close_fd();
    int fd_ = -1;
    std::uint16_t port_ = 0;
    double loss_ = 0.0;
    double dup_ = 0.0;
    std::uint64_t rng_state_ = 1;
    std::uint64_t sent_ = 0;
    std::uint64_t dropped_ = 0;
    double next_uniform();
};

}  // namespace tether
