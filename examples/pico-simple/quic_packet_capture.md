# QUIC Packet Capture Walkthrough

This document explains the packets in `examples/pico-simple/quic.txt` captured on loopback.  
It shows a first connection (full handshake) and a second connection using 0-RTT resumption.

## Connection 1: Full handshake

### Packet 338 (Client → Server): Initial (ClientHello)
- **Type:** Initial long header.
- **Frames:** `PING`, `CRYPTO`.
- **Purpose:** Starts the QUIC connection and carries the TLS 1.3 ClientHello in a CRYPTO frame.
- **TLS details:** SNI `test.example.com`, ALPN `picoquic_sample`, transport parameters, key share.
- **Why it matters:** This is the first flight of the handshake; no resumption is indicated.

### Packet 341 (Server → Client): Initial + Handshake
- **Type:** Initial long header with `ACK` + `CRYPTO` (ServerHello), plus Handshake packets (encrypted, not decoded here).
- **Frames:** `ACK` acknowledges client Initial; `CRYPTO` carries TLS ServerHello.
- **Purpose:** Server acknowledges ClientHello and starts its side of the TLS handshake.

### Packet 342 (Server → Client): 1-RTT protected
- **Type:** Short header (1-RTT).
- **Purpose:** Server is already sending application-protected traffic. Payload is encrypted in the capture.

### Packet 343 (Client → Server): Handshake + 1-RTT
- **Type:** Handshake long header (client handshake data), plus short header 1-RTT.
- **Purpose:** Client continues TLS handshake and may send early 1-RTT data once keys are ready.

### Packet 344 (Client → Server): 1-RTT protected
- **Type:** Short header (1-RTT).
- **Purpose:** More protected traffic after handshake progresses.

### Packet 345 (Server → Client): 1-RTT protected
- **Type:** Short header (1-RTT).
- **Purpose:** Server’s protected response traffic.

### Packet 346 (Client → Server): 1-RTT protected
- **Type:** Short header (1-RTT).
- **Purpose:** Client’s protected traffic.

### Packet 347 (Server → Client): 1-RTT protected
- **Type:** Short header (1-RTT).
- **Purpose:** Final protected traffic in this exchange.

**Summary of Connection 1:** A normal TLS 1.3 handshake over QUIC. No session resumption indicators yet.

## Connection 2: 0-RTT / resumption

### Packet 458 (Client → Server): Initial + 0-RTT
- **Type:** Initial long header (ClientHello) and **0-RTT long header** in the same UDP datagram.
- **Frames in Initial:** `PING`, `CRYPTO` (ClientHello).
- **TLS details:** ClientHello includes `early_data` and `pre_shared_key` extensions.
- **Why it matters:** `early_data` + `pre_shared_key` means the client is attempting **resumption** and **0-RTT**.
- **0-RTT packet:** Explicitly shown as “Packet Type: 0-RTT”. This is application data sent before handshake completion.

### Packet 459 (Server → Client): Initial + Handshake (ServerHello)
- **Type:** Initial long header with `ACK` + `CRYPTO`, plus Handshake packets.
- **TLS details:** ServerHello includes the `pre_shared_key` extension.
- **Why it matters:** The server is acknowledging the PSK and continuing the resumption handshake.

### Packet 460 (Server → Client): 1-RTT protected
- **Type:** Short header (1-RTT).
- **Purpose:** Protected server traffic after handshake progresses.

### Packet 463 (Client → Server): Handshake
- **Type:** Handshake long header.
- **Purpose:** Client’s handshake completion data.

**Summary of Connection 2:** The client sends **0-RTT application data** alongside the Initial packet.  
The presence of `early_data` and `pre_shared_key` in the ClientHello, and `pre_shared_key` in the ServerHello, confirms resumption.  
This is consistent with a successful 0-RTT attempt.

## Key takeaways

- **0-RTT evidence:** Packet 458 contains an explicit 0-RTT packet, and the ClientHello includes `early_data` + `pre_shared_key`.
- **Resumption evidence:** ServerHello includes `pre_shared_key` (Packet 459), confirming a PSK-based handshake.
- **Practical impact:** The second connection can send application data immediately (0-RTT), reducing latency.
