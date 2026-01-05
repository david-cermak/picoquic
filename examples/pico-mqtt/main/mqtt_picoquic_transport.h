#pragma once

#include "esp_transport.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Create an esp_transport that uses picoquic as the QUIC backend and exposes a single bidirectional stream
 *        as a byte-stream suitable for esp-mqtt.
 *
 * Notes:
 * - QUIC TLS ALPN is set to "mqtt"
 * - A background picoquic network thread is used (picoquic_start_network_thread)
 * - The returned transport is owned by the MQTT client and destroyed by esp_mqtt_client_destroy()
 */
esp_transport_handle_t esp_transport_picoquic_mqtt_init(void);

#ifdef __cplusplus
}
#endif


