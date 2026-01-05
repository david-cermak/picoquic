# pquic

Initial QUIC work for ESP-IDF, with **two QUIC backends** and a small set of runnable examples.

### QUIC backends

- **picoquic** (C): the original focus of this repo (initial port/integration to ESP-IDF)
- **esp-http3** (C++): QUIC + HTTP/3 client that can also run as “plain QUIC” for non-HTTP protocols (e.g. MQTT)

### Examples

All examples live under `examples/`:

- **`examples/mqtt`**: `esp-mqtt` over QUIC using the `impl/esp-http3` backend (custom `esp_transport`)
- **`examples/pico-mqtt`**: `esp-mqtt` over QUIC using the `picoquic` backend (custom `esp_transport`)
- **`examples/simple`**: minimal QUIC + HTTP/3 client example (esp-http3 backend)
- **`examples/pico-simple`**: minimal picoquic-based example

### Licensing

- **Repository**: MIT (see `LICENSE`)
- **`deps/esp-http3`**: Apache-2.0 (see the license headers / LICENSE file under that component)
