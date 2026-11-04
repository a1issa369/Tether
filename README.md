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

One run of `scripts/demo.sh build 5` on a MacBook Pro (Apple clang), over loopback with simulated loss in both directions:

| Loss | Time | Retransmits | Fast | Timer | Hash |
|---|---|---|---|---|---|
| 0% | 0.84 s | 0 | 0 | 0 | match |
| 5% | 1.38 s | 472 | 189 | 14 | match |
| 20% | 34.34 s | 2737 | 756 | 163 | match |

These are loopback numbers with simulated loss, not a real network, and one run each. Treat them as a sanity check, not a benchmark.

## Testing

45 tests, all passing, also under ASan and UBSan: packet encode/decode and fuzzing, handshake loss cases, a loss sweep over many seeds, chaos (loss plus duplication plus corruption plus reordering), sequence wraparound, backoff and give-up, RTT convergence, fast retransmit, flow control, close, protocol violations, and real UDP loopback tests.

`scripts/mutation_check.py` breaks the protocol on purpose (13 mutations: disable retransmission, skip the checksum, ignore the window, deliver duplicates, and so on), rebuilds, and checks the suite fails. All 13 are caught. The first run caught only 11; the two survivors showed real test gaps (nothing checked that Fin waits for outstanding data, and Karn's rule was not actually exercised), so I added tests for both.

## Design tradeoffs

- **Pure state machine, no sockets or clock inside.** `Connection` takes datagrams and a timestamp and returns datagrams. This cost an extra layer (`Endpoint`, `Session`) but makes every test deterministic, with seeded loss, a virtual clock and no sleeping. Real-socket behavior is covered separately by a few UDP loopback tests.
- **Messages, not a byte stream.** The unit is a message split into fragments with Begin/End flags, so callers never reassemble framing themselves. The tradeoff is that sequence numbers count packets rather than bytes, which is simpler but means a message cannot be sent partly and resumed.
- **Per-packet retransmission timers with one shared backoff.** Each in-flight packet has its own deadline, so one lost packet does not wait behind others. The backoff doubles once per expiry event, not once per packet, so a burst of expiries does not blow the timeout up exponentially. Cost: more bookkeeping than a single timer.
- **Cumulative ack plus a 32-bit SACK bitmap.** Cheap to encode and enough to detect holes quickly, but it can only describe 32 packets beyond the cumulative ack. A bigger window would need a different encoding.
- **Fast retransmit after 3 SACKed packets above a hole, once per packet.** Matches TCP's duplicate-ack heuristic. A heavily reordering network can trigger it needlessly, which costs a duplicate packet but never correctness.
- **Advertised window counts only complete messages waiting for the app.** If partial messages counted too, a message larger than the receive window could never finish arriving and the connection would deadlock. The cost is that a receiver can briefly hold more than the window while a large message assembles, so memory is bounded by the window plus one message limit (`max_message_size`).
- **Window measured in packets, not bytes.** Simple and enough for fixed-size fragments, but it ignores how big each packet actually is.
- **1200-byte default payload.** Stays under a typical 1500-byte path MTU so datagrams are not fragmented by IP, at the cost of some per-packet header overhead (28 bytes). No path MTU discovery.
- **CRC-32 checksum, not authentication.** It catches corruption, not attackers. Anyone who can guess the connection id can inject packets. Adding a keyed MAC would be the next step.
- **Single-threaded `poll()` loop.** Easy to reason about and to test, and plenty for loopback demos, but one `Endpoint` uses one core and the API is not thread-safe.
- **No congestion control.** I chose to get reliability, flow control and the test harness right first. The result is visible in the 20% loss run: it recovers every byte but is slow, because it keeps sending at a fixed window instead of backing off.

## Things that broke

These are the real problems I hit while building and testing it, in roughly the order they appeared.

- **A test constructor failed under `-Werror`** because the member initializer order did not match the declaration order. Fixed by reordering, which also taught me to keep warnings as errors from the start.
- **The backoff test was off by one** (expected 99, got 100). The first Data packet is logged one millisecond after the simulator step it was queued in, so the test's timing assumption was wrong, not the protocol. I fixed the assertion and added a comment explaining the offset.
- **One UDP test was placeholder code that tested nothing.** I noticed it during review and rewrote it as a real "server restarts, client gets Rst" test.
- **Mutation testing found two weak tests.** I broke the protocol on purpose in 13 ways. Eleven were caught on the first run; two survived: nothing checked that Fin waits for unacknowledged data, and Karn's rule (no RTT sample from a retransmitted packet) was never really exercised. I wrote tests for both.
- **The first Karn test still let the bug through.** It stopped as soon as the receiver had the message, before the late ack returned, so the bad mutation was never observed. It needed to let the network drain before asserting.
- **Clang rejected an unused helper that gcc ignored.** The first build on my Mac failed under `-Werror` with an unused-function error. I had only tested with gcc. I removed the helper and CI now builds with both compilers.
- **The first demo script hung.** A cleanup trap used `kill 0`, which killed the script's own process group, and it parsed the stats with the wrong format. Fixed both.
- **20% loss is slow, and that was a finding, not a fix.** Everything arrives and the hashes match, but it took 34 seconds for 5 MiB because there is no congestion control. I documented it instead of hiding the number.

## Known limits

- No congestion control. This is why 20% loss is slow: it only has timers and a window, and no pacing.
- IPv4 and POSIX only.
- No encryption or authentication. A peer can forge packets if it can guess the connection id.
- Single-threaded; one `Endpoint` per thread.
- Max message size 4 MiB and send queue 8 MiB by default (configurable).
