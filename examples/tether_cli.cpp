// tether_cli: small demos of the library.
//
//   tether_cli echo-server PORT [--once] [--loss P]
//   tether_cli echo-client HOST PORT MESSAGE... [--loss P]
//   tether_cli recv PORT OUTFILE [--loss P]
//   tether_cli send HOST PORT FILE [--loss P]
//
// --loss P drops each outgoing datagram with probability P (0..1) so you can watch the protocol cope.
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

#include "tether/endpoint.hpp"

using namespace tether;

namespace {

double seconds_since(std::chrono::steady_clock::time_point t) {
    return std::chrono::duration<double>(std::chrono::steady_clock::now() - t).count();
}

struct Options {
    std::vector<std::string> args;
    double loss = 0.0;
    bool once = false;
};

Options parse(int argc, char** argv) {
    Options o;
    for (int i = 2; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--loss" && i + 1 < argc) {
            o.loss = std::atof(argv[++i]);
        } else if (a == "--once") {
            o.once = true;
        } else {
            o.args.push_back(a);
        }
    }
    return o;
}

void print_stats(const char* who, const Session& s, double secs) {
    const Stats& st = s.stats();
    std::fprintf(stderr,
                 "[%s] %.2fs  sent %llu B in %llu data packets, %llu retransmits (%llu fast, %llu timer), "
                 "received %llu B, %llu duplicates, %llu out of order, srtt %.1f ms\n",
                 who, secs, static_cast<unsigned long long>(st.bytes_sent),
                 static_cast<unsigned long long>(st.data_packets_sent), static_cast<unsigned long long>(st.retransmits),
                 static_cast<unsigned long long>(st.fast_retransmits), static_cast<unsigned long long>(st.timeouts),
                 static_cast<unsigned long long>(st.bytes_received), static_cast<unsigned long long>(st.duplicates_received),
                 static_cast<unsigned long long>(st.out_of_order_received), s.srtt_ms());
}

bool wait_established(Endpoint& ep, Session& s, double timeout_s) {
    const auto t0 = std::chrono::steady_clock::now();
    while (!s.established() && !s.closed() && seconds_since(t0) < timeout_s) ep.poll(20);
    return s.established();
}

std::string text(const Bytes& b) { return std::string(b.begin(), b.end()); }
Bytes bytes(const std::string& s) { return Bytes(s.begin(), s.end()); }

int echo_server(const Options& o) {
    if (o.args.size() < 1) return 2;
    Config cfg;
    cfg.accept_connections = true;
    Endpoint ep(cfg, static_cast<std::uint16_t>(std::atoi(o.args[0].c_str())));
    ep.socket().set_impairment(o.loss, 0.0, 7);
    std::fprintf(stderr, "echo server listening on UDP port %u\n", ep.port());
    std::vector<std::shared_ptr<Session>> sessions;
    bool served_one = false;
    for (;;) {
        ep.poll(50);
        while (auto s = ep.accept()) {
            std::fprintf(stderr, "accepted %s\n", s->peer().str().c_str());
            sessions.push_back(s);
            served_one = true;
        }
        for (auto it = sessions.begin(); it != sessions.end();) {
            Session& s = **it;
            while (auto m = s.receive()) s.send(*m);
            if (s.peer_closed()) s.close();
            if (s.closed()) {
                it = sessions.erase(it);
            } else {
                ++it;
            }
        }
        if (o.once && served_one && sessions.empty()) {
            for (int i = 0; i < 20; ++i) ep.poll(50);  // let the closing handshake settle
            return 0;
        }
    }
}

int echo_client(const Options& o) {
    if (o.args.size() < 3) return 2;
    auto addr = Address::resolve(o.args[0], static_cast<std::uint16_t>(std::atoi(o.args[1].c_str())));
    if (!addr) {
        std::fprintf(stderr, "cannot resolve %s\n", o.args[0].c_str());
        return 1;
    }
    Endpoint ep;
    ep.socket().set_impairment(o.loss, 0.0, 11);
    auto s = ep.connect(*addr);
    if (!wait_established(ep, *s, 10)) {
        std::fprintf(stderr, "could not connect\n");
        return 1;
    }
    for (std::size_t i = 2; i < o.args.size(); ++i) {
        s->send(bytes(o.args[i]));
        const auto t0 = std::chrono::steady_clock::now();
        std::optional<Bytes> reply;
        while (!(reply = s->receive()) && seconds_since(t0) < 10 && !s->closed()) ep.poll(20);
        if (!reply) {
            std::fprintf(stderr, "no echo for message %zu\n", i - 1);
            return 1;
        }
        std::printf("echo: %s\n", text(*reply).c_str());
    }
    s->close();
    const auto t0 = std::chrono::steady_clock::now();
    while (!s->closed() && seconds_since(t0) < 10) ep.poll(20);
    return s->closed() && s->error() == Error::None ? 0 : 1;
}

int send_file(const Options& o) {
    if (o.args.size() < 3) return 2;
    std::ifstream in(o.args[2], std::ios::binary);
    if (!in) {
        std::fprintf(stderr, "cannot open %s\n", o.args[2].c_str());
        return 1;
    }
    in.seekg(0, std::ios::end);
    const std::size_t size = static_cast<std::size_t>(in.tellg());
    in.seekg(0);
    auto addr = Address::resolve(o.args[0], static_cast<std::uint16_t>(std::atoi(o.args[1].c_str())));
    if (!addr) return 1;
    Endpoint ep;
    ep.socket().set_impairment(o.loss, 0.0, 3);
    auto s = ep.connect(*addr);
    if (!wait_established(ep, *s, 10)) {
        std::fprintf(stderr, "could not connect\n");
        return 1;
    }
    const auto t0 = std::chrono::steady_clock::now();
    auto push = [&](const Bytes& m) {
        while (s->send(m) == SendResult::QueueFull) ep.poll(5);
    };
    push(bytes("TETHER-FILE " + std::to_string(size)));
    Bytes chunk(16 * 1024);
    while (in) {
        in.read(reinterpret_cast<char*>(chunk.data()), static_cast<std::streamsize>(chunk.size()));
        const auto n = static_cast<std::size_t>(in.gcount());
        if (n == 0) break;
        push(Bytes(chunk.begin(), chunk.begin() + static_cast<std::ptrdiff_t>(n)));
    }
    s->close();
    while (!s->closed() && seconds_since(t0) < 120) ep.poll(10);
    const double secs = seconds_since(t0);
    print_stats("send", *s, secs);
    std::fprintf(stderr, "[send] %.2f MB/s, %llu datagrams dropped by the loss simulator\n",
                 static_cast<double>(size) / 1e6 / std::max(secs, 1e-9),
                 static_cast<unsigned long long>(ep.socket().datagrams_dropped()));
    return s->closed() && s->error() == Error::None ? 0 : 1;
}

int recv_file(const Options& o) {
    if (o.args.size() < 2) return 2;
    Config cfg;
    cfg.accept_connections = true;
    Endpoint ep(cfg, static_cast<std::uint16_t>(std::atoi(o.args[0].c_str())));
    ep.socket().set_impairment(o.loss, 0.0, 5);
    std::fprintf(stderr, "receiver listening on UDP port %u\n", ep.port());
    std::shared_ptr<Session> s;
    while (!(s = ep.accept())) ep.poll(50);
    const auto t0 = std::chrono::steady_clock::now();
    std::ofstream out(o.args[1], std::ios::binary | std::ios::trunc);
    std::size_t expected = 0;
    std::size_t got = 0;
    bool header = false;
    while (!s->closed() && seconds_since(t0) < 300) {
        ep.poll(10);
        while (auto m = s->receive()) {
            if (!header) {
                header = true;
                const std::string h = text(*m);
                if (h.rfind("TETHER-FILE ", 0) != 0) {
                    std::fprintf(stderr, "bad header\n");
                    return 1;
                }
                expected = static_cast<std::size_t>(std::strtoull(h.c_str() + 12, nullptr, 10));
            } else {
                out.write(reinterpret_cast<const char*>(m->data()), static_cast<std::streamsize>(m->size()));
                got += m->size();
            }
        }
        if (s->peer_closed()) s->close();
    }
    for (int i = 0; i < 10; ++i) ep.poll(20);
    out.close();
    print_stats("recv", *s, seconds_since(t0));
    if (got != expected) {
        std::fprintf(stderr, "size mismatch: expected %zu bytes, got %zu\n", expected, got);
        return 1;
    }
    std::fprintf(stderr, "[recv] received %zu bytes, size matches\n", got);
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        std::fprintf(stderr, "usage: tether_cli echo-server|echo-client|send|recv ... (see source header)\n");
        return 2;
    }
    const std::string cmd = argv[1];
    const Options o = parse(argc, argv);
    int rc = 2;
    try {
        if (cmd == "echo-server") rc = echo_server(o);
        else if (cmd == "echo-client") rc = echo_client(o);
        else if (cmd == "send") rc = send_file(o);
        else if (cmd == "recv") rc = recv_file(o);
    } catch (const std::exception& e) {
        std::fprintf(stderr, "error: %s\n", e.what());
        return 1;
    }
    if (rc == 2) std::fprintf(stderr, "bad arguments, see the header of examples/tether_cli.cpp\n");
    return rc;
}
