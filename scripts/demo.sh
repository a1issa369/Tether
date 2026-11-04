#!/usr/bin/env bash
# Transfers a random file over tether on loopback with simulated packet loss and checks the hash.
# Usage: scripts/demo.sh [build-dir] [size-in-MiB]
set -euo pipefail
BUILD="${1:-build}"
MIB="${2:-5}"
CLI="$BUILD/tether_cli"
TMP="$(mktemp -d)"
SRV=
trap '[ -n "$SRV" ] && kill "$SRV" 2>/dev/null; rm -rf "$TMP"' EXIT
head -c $((MIB * 1024 * 1024)) /dev/urandom > "$TMP/in.bin"
SUM_IN="$(sha256sum "$TMP/in.bin" | cut -d' ' -f1)"
PORT=$((20000 + RANDOM % 20000))
printf '%-6s %-9s %-8s %-8s %-9s %s\n' loss seconds retrans fast timeouts result
for LOSS in 0 0.05 0.2; do
  rm -f "$TMP/out.bin"
  "$CLI" recv "$PORT" "$TMP/out.bin" --loss "$LOSS" 2> "$TMP/recv.log" &
  SRV=$!
  sleep 0.3
  START=$(date +%s.%N)
  "$CLI" send 127.0.0.1 "$PORT" "$TMP/in.bin" --loss "$LOSS" 2> "$TMP/send.log"
  END=$(date +%s.%N)
  wait "$SRV" || true
  SRV=
  SUM_OUT="$(sha256sum "$TMP/out.bin" | cut -d' ' -f1)"
  RES=MISMATCH; [ "$SUM_IN" = "$SUM_OUT" ] && RES=OK
  STATS="$(grep -m1 'data packets' "$TMP/send.log" || true)"
  R="$(sed -n 's/.* \([0-9]*\) retransmits.*/\1/p' <<<"$STATS")"
  F="$(sed -n 's/.*(\([0-9]*\) fast.*/\1/p' <<<"$STATS")"
  T="$(sed -n 's/.*, \([0-9]*\) timer).*/\1/p' <<<"$STATS")"
  printf '%-6s %-9.2f %-8s %-8s %-9s %s\n' "$LOSS" "$(echo "$END - $START" | bc)" "${R:-?}" "${F:-?}" "${T:-?}" "$RES"
  PORT=$((PORT + 1))
done
