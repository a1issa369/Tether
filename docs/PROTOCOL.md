# tether wire protocol (version 1)

All integers are big-endian. Every datagram is a 28-byte header followed by a payload.

| Offset | Size | Field | Notes |
|---|---|---|---|
| 0 | 2 | magic | 0x5445 |
| 2 | 1 | version | 1 |
| 3 | 1 | type | Syn, SynAck, Data, Ack, Fin, FinAck, Rst |
| 4 | 1 | flags | Begin=1, End=2, Probe=4 |
| 5 | 1 | reserved | zero |
| 6 | 2 | window | receiver's advertised window, in packets |
| 8 | 4 | conn_id | chosen by the client |
| 12 | 4 | seq | packet sequence number (counts packets, not bytes) |
| 16 | 4 | ack | next sequence number the sender expects (cumulative) |
| 20 | 4 | sack | bit i set means packet ack+1+i has arrived |
| 24 | 4 | checksum | CRC-32 of the datagram with this field zeroed |

Payload length is the datagram size minus 28. Only Data packets carry a payload (at most 65000 bytes).

A decoder rejects: too short, bad magic, bad version, unknown type, unknown flags, bad checksum, payload on a non-Data packet.

## Messages
A message is split into fragments. The first fragment has Begin, the last has End; a one-fragment message has both. Fragments are delivered in order, so the receiver reassembles by appending until End.

## Handshake
Client sends Syn (its initial seq). Server replies SynAck (its initial seq, ack of the client's). Client's next packet acks it. Syn and SynAck are retransmitted on a timer.

## Reliability
Cumulative ack plus a 32-packet SACK bitmap. Each in-flight packet has its own timer. RTO uses RFC 6298 SRTT/RTTVAR, doubles once per expiry event, and resets on a fresh sample. Karn's rule: no sample from a retransmitted packet. Fast retransmit when three packets above a hole are SACKed. After 10 retries the connection fails with Timeout.

## Flow control
The advertised window is the receive window minus complete-message packets the application has not read yet. Partial messages are not counted, otherwise a message larger than the window could never complete. A zero window is probed with an Ack carrying the Probe flag. When the app reads and the window reopens, a window-update Ack is sent.

## Close
Fin is sent only after all data is acknowledged. FinAck acknowledges it. Each direction closes independently (half-close). A closed connection lingers to answer a repeated Fin. A packet for an unknown connection gets Rst.

## Not in the protocol
No congestion control, no encryption, no path MTU discovery (the default payload is 1200 bytes), no connection migration.
