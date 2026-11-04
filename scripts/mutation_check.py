#!/usr/bin/env python3
"""Poor man's mutation testing: break the protocol on purpose and confirm the test suite notices.

Each mutation edits one line of the source, rebuilds, runs the tests and restores the file. A mutation
that the tests do NOT catch is reported as SURVIVED and the script exits non-zero.
"""
import os
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
BUILD = os.path.join(ROOT, "build-mutation")

MUTATIONS = [
    ("timer retransmission disabled", "src/connection.cpp",
     "    if (!expired) return;\n    const Millis rto_at_expiry", "    return;\n    const Millis rto_at_expiry"),
    ("checksum not verified", "src/packet.cpp",
     "if (get32(data + 24) != checksum_of(data, size)) return fail(error, DecodeError::BadChecksum);", ""),
    ("receive window not enforced", "src/connection.cpp",
     "if (static_cast<std::uint32_t>(d) >= advertised_window()) {", "if (static_cast<std::uint32_t>(d) >= 65535) {"),
    ("duplicate data delivered again", "src/connection.cpp",
     "        ++stats_.duplicates_received;  // already delivered; the repeated ack tells the sender\n        return;",
     "        ++stats_.duplicates_received;\n        deliver(p.flags, p.payload);\n        return;"),
    ("fast retransmit never triggers", "src/connection.cpp",
     "constexpr int kFastRetransmitThreshold = 3;", "constexpr int kFastRetransmitThreshold = 100000;"),
    ("no exponential backoff", "src/connection.cpp",
     "rto_ = std::min(rto_ * 2, cfg_.max_rto_ms);  // exponential", "rto_ = std::min(rto_ * 1, cfg_.max_rto_ms);  // exponential"),
    ("sack bitmap off by one", "src/connection.cpp",
     "if (recv_buf_.count(recv_next_ + 1 + i) != 0) bits |= (1U << i);", "if (recv_buf_.count(recv_next_ + i) != 0) bits |= (1U << i);"),
    ("sender ignores the peer window", "src/connection.cpp",
     " &&\n           static_cast<std::uint32_t>(next_seq_ - peer_ack_) < peer_window_) {", ") {"),
    ("fin sent before data is acknowledged", "src/connection.cpp",
     "pending_.empty() && inflight_.empty()) {\n        fin_sent_ = true;", "pending_.empty()) {\n        fin_sent_ = true;"),
    ("stale acks accepted", "src/connection.cpp",
     "    if (seq_lt(a, send_base_)) return;   // stale or reordered acknowledgement\n", ""),
    ("acks for unsent data accepted", "src/connection.cpp",
     "    if (seq_lt(next_seq_, a)) return;    // acknowledges data we never sent\n", ""),
    ("rtt sample taken from retransmitted packets", "src/connection.cpp",
     "if (o.tx_count == 1 && (!have_sample", "if (o.tx_count >= 1 && (!have_sample"),
    ("sequence comparison ignores wrap-around", "src/connection.cpp",
     "inline bool seq_lt(std::uint32_t a, std::uint32_t b) { return static_cast<std::int32_t>(a - b) < 0; }",
     "inline bool seq_lt(std::uint32_t a, std::uint32_t b) { return a < b; }"),
]


def run(cmd, **kw):
    return subprocess.run(cmd, capture_output=True, text=True, **kw)


def main():
    os.makedirs(BUILD, exist_ok=True)
    if run(["cmake", ROOT, "-DCMAKE_BUILD_TYPE=Release"], cwd=BUILD).returncode != 0:
        sys.exit("cmake failed")
    survived = []
    for name, rel, old, new in MUTATIONS:
        path = os.path.join(ROOT, rel)
        original = open(path).read()
        if old not in original:
            sys.exit(f"mutation target not found for '{name}' (the source changed?)")
        open(path, "w").write(original.replace(old, new, 1))
        try:
            built = run(["make", "-j4", "tether_tests"], cwd=BUILD)
            if built.returncode != 0:
                status = "CAUGHT (does not even build)"
            else:
                try:
                    r = run(["./tether_tests"], cwd=BUILD, timeout=600)
                    status = "CAUGHT" if r.returncode != 0 else "SURVIVED"
                except subprocess.TimeoutExpired:
                    status = "CAUGHT (hangs)"
        finally:
            open(path, "w").write(original)
        print(f"{status:32} {name}", flush=True)
        if status == "SURVIVED":
            survived.append(name)
    run(["make", "-j4", "tether_tests"], cwd=BUILD)  # leave a clean build behind
    if survived:
        print(f"\n{len(survived)} mutation(s) survived: the tests are missing something")
        sys.exit(1)
    print(f"\nall {len(MUTATIONS)} mutations were caught")


if __name__ == "__main__":
    main()
