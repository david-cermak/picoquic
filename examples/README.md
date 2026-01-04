# test examples with esp-http3 component

This directory contains small PoCs built on top of the `impl/esp-http3` component.

## esp-http3 component changes (summary)

`impl/esp-http3` is primarily an HTTP/3 client, but it can now also be used as a **plain QUIC**
transport for other application protocols (e.g. MQTT over QUIC).

### What changed

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
- **`examples/mqtt`**: dedicated “MQTT transport over QUIC” PoC (no MQTT library; just raw bytes).