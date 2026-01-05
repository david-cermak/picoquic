#pragma once

#include "esp_transport.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Create an esp_transport that speaks "MQTT over QUIC stream" using esp_http3::QuicConnection
 *
 * Notes:
 * - Connects via UDP + QUIC handshake (ALPN="mqtt", enable_http3=false)
 * - Opens a single client-initiated bidirectional stream and exposes it as a byte stream for esp-mqtt
 * - No extra thread: read/write/poll pump the UDP socket and QUIC timers internally
 */
esp_transport_handle_t esp_transport_quic_mqtt_init(void);

#ifdef __cplusplus
}
#endif


