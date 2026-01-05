# Examples

This directory contains small examples/PoCs that exercise QUIC + MQTT integrations in this repo.

## QUIC backends in this repo

You currently have **two QUIC backends** used by examples:

- **`impl/esp-http3`**: C++ QUIC/HTTP3 client that can also run as “plain QUIC” (no HTTP/3) for other protocols.
- **`picoquic`**: picoquic-based QUIC stack (C) used by the `pico-mqtt` example.

Both examples use the same MQTT library:

- **`impl/idf/components/mqtt/esp-mqtt`** (`mqtt_client.h`) with a custom `esp_transport_handle_t`.

## esp-http3 component changes (summary)

`impl/esp-http3` is primarily an HTTP/3 client, but it can also be used as a **plain QUIC** transport for other application protocols.

### What changed (esp-http3)

- **Configurable TLS ALPN**
  - `esp_http3::QuicConfig` now includes `alpn` (default: `"h3"`).
  - TLS ClientHello generation now uses this value (instead of hard-coding `"h3"`).

- **Optional HTTP/3 layer**
  - `esp_http3::QuicConfig` now includes `enable_http3` (default: `true`).
  - When `enable_http3=false`, the connection will not send HTTP/3 control/QPACK streams or SETTINGS,
    and received STREAM data is delivered directly via `QuicConnection::SetOnStreamData()`.

- **Raw QUIC stream API (no HTTP/3 framing)**
  - `QuicConnection::OpenBidirectionalStream()` allocates a client-initiated bidi stream (0, 4, 8, …).
  - `QuicConnection::WriteStreamRaw()` writes bytes as QUIC STREAM payload (no HTTP/3 DATA frames).

### Why it matters

Non-HTTP3 protocols over QUIC typically need:
- a custom **ALPN** (e.g. `"mqtt"`)
- **no HTTP/3 bytes** on the wire
- direct access to raw QUIC streams

That’s what `enable_http3=false` + the raw stream API provides.

## Examples

- **`examples/simple`**: minimal ESP32 example doing QUIC + http3.
- **`examples/mqtt`**: `esp-mqtt` over QUIC using the `impl/esp-http3` backend (custom `esp_transport`).
- **`examples/pico-mqtt`**: `esp-mqtt` over QUIC using the `picoquic` backend (custom `esp_transport`).