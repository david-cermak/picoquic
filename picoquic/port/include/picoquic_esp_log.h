/*
 * Picoquic ESP-IDF logging backend
 *
 * This provides a lightweight text logging sink that routes picoquic's unified
 * logging events through ESP_LOGx() instead of FILE-based logs / qlog.
 *
 * The picoquic library only emits unified "text log" events when a text log
 * backend is installed (quic->F_log != NULL). This backend installs such a
 * backend without requiring filesystem access.
 */

#ifndef PICOQUIC_ESP_LOG_H
#define PICOQUIC_ESP_LOG_H

#ifdef __cplusplus
extern "C" {
#endif

#include "picoquic.h"

/* Enable picoquic unified "text logs" and route them to ESP_LOGx().
 *
 * - tag: ESP logging tag (e.g., "pquic"). If NULL, a default tag is used.
 * - log_packets: if non-zero, emit packet/pdu level logs (can be noisy).
 *
 * Returns 0 on success, or -1 on error (e.g., OOM).
 */
int picoquic_set_esp_log(picoquic_quic_t* quic, const char* tag, int log_packets);

#ifdef __cplusplus
}
#endif

#endif /* PICOQUIC_ESP_LOG_H */


