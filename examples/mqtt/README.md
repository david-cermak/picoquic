### MQTT-over-QUIC (esp-http3 backend)

This example runs a **real ESP-IDF MQTT client (`esp-mqtt`) over QUIC**, using the `impl/esp-http3` QUIC stack as the backend.

- **MQTT library**: `impl/idf/components/mqtt/esp-mqtt` (via `mqtt_client.h`)
- **Transport**: custom `esp_transport_handle_t` backed by `esp_http3::QuicConnection`
- **TLS ALPN**: `"mqtt"` (and `enable_http3=false` to avoid sending HTTP/3 bytes)
- **Broker**: `broker.emqx.io:14567`

### How it works

- `app_main()` configures `esp_mqtt_client_config_t` and sets:
  - `mqtt_config.network.transport = esp_transport_quic_mqtt_init()`
- The transport (`mqtt_quic_transport.cpp`) does:
  - UDP `connect()` to the broker
  - QUIC handshake with `esp_http3::QuicConnection` (`alpn="mqtt"`, `enable_http3=false`)
  - opens one bidirectional stream and exposes it as a byte stream to `esp-mqtt`

### Where to look

- **Example app**: `examples/mqtt/main/simple.cpp`
- **Custom transport**: `examples/mqtt/main/mqtt_quic_transport.{h,cpp}`

### Notes

- For non-HTTP3 protocols over QUIC, we must not send any HTTP/3 control/QPACK/SETTINGS bytes:
  - `enable_http3=false` is required here.