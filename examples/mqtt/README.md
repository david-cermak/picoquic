### MQTT-over-QUIC (PoC)

This example is a **proof-of-concept “MQTT transport over QUIC”** using the `impl/esp-http3` QUIC stack.

- **No MQTT library**
- **No MQTT protocol implementation** beyond sending a single raw **MQTT CONNECT** packet and logging the response
- Uses **TLS ALPN = `"mqtt"`**
- Connects to the public EMQX broker:
  - **host**: `broker.emqx.io`
  - **QUIC port**: `14567`

### How it works

- **UDP socket**: resolves `broker.emqx.io`, creates a UDP socket, and `connect()`s it to port `14567`.
- **QUIC handshake**: constructs `esp_http3::QuicConnection` with:
  - `qc.alpn = "mqtt"`
  - `qc.enable_http3 = false` (important: prevents any HTTP/3 control/QPACK/SETTINGS traffic)
- **Raw stream**: opens a client-initiated bidirectional stream via `OpenBidirectionalStream()`.
- **Raw MQTT CONNECT**: sends a minimal MQTT 3.1.1 CONNECT message via `WriteStreamRaw()`.
- **Receive/print**: logs the first bytes received on the stream (typically a 4-byte CONNACK).

### Expected result

If the broker accepts the CONNECT, you should see the CONNACK bytes:

- **`20 02 00 00`**
  - `0x20`: CONNACK packet type
  - `0x02`: remaining length
  - `0x00 0x00`: “session present = 0”, “return code = 0 (Success)”

### Example output (linux target)

```
I (...) mqtt: QUIC connected (ALPN=mqtt)
I (...) mqtt: Sent MQTT CONNECT over QUIC: stream=0 bytes=22
I (...) mqtt: RX stream=0 len=4 fin=0
I (...) mqtt: RX bytes (first 4): 20 02 00 00
I (...) mqtt: Holding connection open for 15 seconds...
E (...) mqtt: QUIC disconnected: code=0 reason=done
```

The final disconnect is triggered by the example calling `conn.Close(0, "done")` after the hold period.