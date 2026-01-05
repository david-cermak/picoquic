### MQTT-over-QUIC (picoquic backend)

This example runs a **real ESP-IDF MQTT client (`esp-mqtt`) over QUIC**, using **picoquic** as the QUIC/TLS backend.

- **MQTT library**: `impl/idf/components/mqtt/esp-mqtt` (via `mqtt_client.h`)
- **Transport**: custom `esp_transport_handle_t` backed by picoquic
- **TLS ALPN**: `"mqtt"`
- **Broker**: `broker.emqx.io:14567` (public EMQX QUIC listener used throughout this repo)

### How it works

- `app_main()` configures `esp_mqtt_client_config_t` and sets:
  - `mqtt_config.network.transport = esp_transport_picoquic_mqtt_init()`
- The transport (`mqtt_picoquic_transport.cpp`) creates a picoquic connection and:
  - starts a **picoquic network thread** (`picoquic_start_network_thread`)
  - opens one **bidirectional stream**
  - exposes that stream as a byte stream to `esp-mqtt` via `esp_transport` callbacks (`connect/read/write/poll/close`)

### Where to look

- **Example app**: `examples/pico-mqtt/main/pquic.c`
- **Custom transport**: `examples/pico-mqtt/main/mqtt_picoquic_transport.{h,cpp}`

