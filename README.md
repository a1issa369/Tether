# tether

Reliable, ordered, message-oriented transport over UDP, written in C++17 with no dependencies.

It gives you connections, message framing, retransmission, selective acks, flow control and graceful close on top of plain UDP datagrams. I built it to learn how TCP-like protocols work by implementing one and testing it against a hostile network.

## Design

The protocol logic is a pure state machine (`Connection`). It never touches a socket or a clock: you feed it datagrams and the current time, and ask it for datagrams to send and the next deadline. A thin layer (`Endpoint`, `Session`, `UdpSocket`) runs it over a real non-blocking UDP socket with a single-threaded `poll()` loop.

Keeping the clock out of the core is what makes the tests deterministic: they run the same code against a simulated network with a virtual clock, seeded loss, duplication, corruption and reordering.

Wire format and behavior are in [docs/PROTOCOL.md](docs/PROTOCOL.md).

## Build and test

    cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
    cmake --build build -j
    ./build/tether_tests

Sanitizers: `cmake -S . -B build-san -DCMAKE_BUILD_TYPE=Debug -DTETHER_SANITIZE=ON`. Warnings are errors by default (`TETHER_WERROR`).

## Try it

    ./build/tether_cli echo-server 9000
    ./build/tether_cli echo-client 127.0.0.1 9000 hello world
    ./build/tether_cli recv 9001 out.bin --loss 0.1
    ./build/tether_cli send 127.0.0.1 9001 in.bin --loss 0.1

`--loss P` randomly drops that fraction of datagrams this process sends, so with both sides set you get loss in both directions.

`scripts/demo.sh` sends a random 5 MiB file at 0%, 5% and 20% loss and compares SHA-256 hashes.

## Results

One run of `scripts/demo.sh build 5` on a cloud container, over loopback with simulated loss in both directions:

| Loss | Time | Retransmits | Fast | Timer | Hash |
|---|---|---|---|---|---|
| 0% | 0.05 s | 0 | 0 | 0 | match |
| 5% | 0.36 s | 480 | 192 | 12 | match |
| 20% | 41.6 s | 3339 | 525 | 115 | match |

These are loopback numbers with simulated loss, not a real network, and one run each. Treat them as a sanity check, not a benchmark.

## Testing

45 tests, all passing, also under ASan and UBSan: packet encode/decode and fuzzing, handshake loss cases, a loss sweep over many seeds, chaos (loss plus duplication plus corruption plus reordering), sequence wraparound, backoff and give-up, RTT convergence, fast retransmit, flow control, close, protocol violations, and real UDP loopback tests.

`scripts/mutation_check.py` breaks the protocol on purpose (13 mutations: disable retransmission, skip the checksum, ignore the window, deliver duplicates, and so on), rebuilds, and checks the suite fails. All 13 are caught. The first run caught only 11; the two survivors showed real test gaps (nothing checked that Fin waits for outstanding data, and Karn's rule was not actually exercised), so I added tests for both.

## Things that broke

- Backoff test was off by one (99 vs 100) because the first Data packet is logged one millisecond late in the simulator. I fixed the assertion, not the protocol.
- A test constructor failed under `-Werror` because of member initializer order.
- My last UDP test was placeholder code that tested nothing; I rewrote it as a real "server restarts, client gets Rst" test.
- The mutation check found two weak tests (see above). The Karn test was also too short at first: it stopped before the late ack came back, so the bad mutation survived until I let the network drain.
- The first demo script hung: it used `kill 0` in a trap, which killed its own process group, and it parsed stats with the wrong format.

## Known limits

- No congestion control. This is why 20% loss is slow: it only has timers and a window, and no pacing.
- IPv4 and POSIX only.
- No encryption or authentication. A peer can forge packets if it can guess the connection id.
- Single-threaded; one `Endpoint` per thread.
- Max message size 4 MiB and send queue 8 MiB by default (configurable).
