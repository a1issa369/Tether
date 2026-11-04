#include "tether/udp.hpp"

#include <arpa/inet.h>
#include <fcntl.h>
#include <netdb.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>
#include <stdexcept>

namespace tether {

std::optional<Address> Address::resolve(const std::string& host, std::uint16_t port) {
    addrinfo hints{};
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_DGRAM;
    addrinfo* result = nullptr;
    if (getaddrinfo(host.c_str(), nullptr, &hints, &result) != 0 || result == nullptr) return std::nullopt;
    Address a;
    std::memcpy(&a.sa_, result->ai_addr, sizeof(sockaddr_in));
    a.sa_.sin_port = htons(port);
    freeaddrinfo(result);
    return a;
}

Address Address::from_sockaddr(const sockaddr_in& sa) {
    Address a;
    a.sa_ = sa;
    return a;
}

std::uint16_t Address::port() const { return ntohs(sa_.sin_port); }

std::string Address::str() const {
    char buf[INET_ADDRSTRLEN] = {0};
    inet_ntop(AF_INET, &sa_.sin_addr, buf, sizeof(buf));
    return std::string(buf) + ":" + std::to_string(port());
}

bool Address::operator==(const Address& o) const {
    return sa_.sin_addr.s_addr == o.sa_.sin_addr.s_addr && sa_.sin_port == o.sa_.sin_port;
}

std::size_t AddressHash::operator()(const Address& a) const {
    return std::hash<std::uint64_t>()((static_cast<std::uint64_t>(a.raw().sin_addr.s_addr) << 16) ^ a.raw().sin_port);
}

UdpSocket::UdpSocket(std::uint16_t port) {
    fd_ = ::socket(AF_INET, SOCK_DGRAM, 0);
    if (fd_ < 0) throw std::runtime_error(std::string("socket: ") + std::strerror(errno));
    sockaddr_in sa{};
    sa.sin_family = AF_INET;
    sa.sin_addr.s_addr = htonl(INADDR_ANY);
    sa.sin_port = htons(port);
    if (::bind(fd_, reinterpret_cast<sockaddr*>(&sa), sizeof(sa)) != 0) {
        const std::string msg = std::string("bind: ") + std::strerror(errno);
        close_fd();
        throw std::runtime_error(msg);
    }
    socklen_t len = sizeof(sa);
    ::getsockname(fd_, reinterpret_cast<sockaddr*>(&sa), &len);
    port_ = ntohs(sa.sin_port);
    const int flags = ::fcntl(fd_, F_GETFL, 0);
    ::fcntl(fd_, F_SETFL, flags | O_NONBLOCK);
    int buf = 4 << 20;  // best effort: a bigger buffer absorbs bursts between polls
    ::setsockopt(fd_, SOL_SOCKET, SO_RCVBUF, &buf, sizeof(buf));
    ::setsockopt(fd_, SOL_SOCKET, SO_SNDBUF, &buf, sizeof(buf));
}

UdpSocket::~UdpSocket() { close_fd(); }

UdpSocket::UdpSocket(UdpSocket&& o) noexcept
    : fd_(o.fd_), port_(o.port_), loss_(o.loss_), dup_(o.dup_), rng_state_(o.rng_state_), sent_(o.sent_), dropped_(o.dropped_) {
    o.fd_ = -1;
}

UdpSocket& UdpSocket::operator=(UdpSocket&& o) noexcept {
    if (this != &o) {
        close_fd();
        fd_ = o.fd_;
        port_ = o.port_;
        loss_ = o.loss_;
        dup_ = o.dup_;
        rng_state_ = o.rng_state_;
        sent_ = o.sent_;
        dropped_ = o.dropped_;
        o.fd_ = -1;
    }
    return *this;
}

void UdpSocket::close_fd() {
    if (fd_ >= 0) ::close(fd_);
    fd_ = -1;
}

double UdpSocket::next_uniform() {
    // xorshift64*: small, deterministic, good enough for loss simulation
    rng_state_ ^= rng_state_ >> 12;
    rng_state_ ^= rng_state_ << 25;
    rng_state_ ^= rng_state_ >> 27;
    const std::uint64_t r = rng_state_ * 2685821657736338717ULL;
    return static_cast<double>(r >> 11) / 9007199254740992.0;
}

void UdpSocket::set_impairment(double loss, double dup, std::uint32_t seed) {
    loss_ = loss;
    dup_ = dup;
    rng_state_ = (static_cast<std::uint64_t>(seed) << 1) | 1ULL;
}

bool UdpSocket::send_to(const Address& to, const Bytes& data) {
    if (loss_ > 0.0 && next_uniform() < loss_) {
        ++dropped_;
        return true;  // pretend it left; the network "lost" it
    }
    const int copies = (dup_ > 0.0 && next_uniform() < dup_) ? 2 : 1;
    bool ok = true;
    for (int i = 0; i < copies; ++i) {
        const ssize_t n = ::sendto(fd_, data.data(), data.size(), 0, reinterpret_cast<const sockaddr*>(&to.raw()),
                                   sizeof(sockaddr_in));
        ++sent_;
        if (n < 0) ok = false;  // e.g. buffer full: UDP gives no guarantee, the protocol will retransmit
    }
    return ok;
}

std::optional<std::pair<Address, Bytes>> UdpSocket::receive() {
    static thread_local Bytes buf(65536);
    sockaddr_in from{};
    socklen_t len = sizeof(from);
    const ssize_t n = ::recvfrom(fd_, buf.data(), buf.size(), 0, reinterpret_cast<sockaddr*>(&from), &len);
    if (n < 0) return std::nullopt;
    return std::make_pair(Address::from_sockaddr(from), Bytes(buf.begin(), buf.begin() + n));
}

bool UdpSocket::wait_readable(int timeout_ms) {
    pollfd p{};
    p.fd = fd_;
    p.events = POLLIN;
    const int r = ::poll(&p, 1, timeout_ms < 0 ? 0 : timeout_ms);
    return r > 0 && (p.revents & POLLIN) != 0;
}

}  // namespace tether
