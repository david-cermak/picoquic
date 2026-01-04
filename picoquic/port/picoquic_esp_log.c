/*
 * Picoquic ESP-IDF logging backend
 *
 * Implements a picoquic_unified_logging_t vtable that routes logs to ESP_LOGx().
 */

#include "picoquic_esp_log.h"

#ifdef ESP_PLATFORM

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "esp_log.h"

#include "picoquic_internal.h"
#include "picoquic_unified_log.h"

/* NOTE:
 * picoquic_quic_t::F_log is used in multiple places as a FILE* (e.g., fflush(quic->F_log)).
 * We must keep it a valid FILE* when logging is enabled.
 *
 * We keep our configuration in static globals (sufficient for typical ESP-IDF usage where
 * there is a single quic context in an application).
 */
static const char* g_picoquic_esp_log_tag = "picoquic";
static int g_picoquic_esp_log_packets = 0;

static const char* picoquic_esp_log_get_tag(picoquic_quic_t* quic)
{
    (void)quic;
    return (g_picoquic_esp_log_tag != NULL && g_picoquic_esp_log_tag[0] != 0) ? g_picoquic_esp_log_tag : "picoquic";
}

static int picoquic_esp_log_get_log_packets(picoquic_quic_t* quic)
{
    (void)quic;
    return g_picoquic_esp_log_packets;
}

static void picoquic_esp_vlog(picoquic_quic_t* quic, esp_log_level_t level, const char* fmt, va_list vargs)
{
    /* Keep buffers small to avoid stack bloat; truncate is OK for debug logs. */
    char buf[256];
    (void)vsnprintf(buf, sizeof(buf), fmt, vargs);
    ESP_LOG_LEVEL_LOCAL(level, picoquic_esp_log_get_tag(quic), "%s", buf);
}

static void esp_log_quic_app_message(picoquic_quic_t* quic, const picoquic_connection_id_t* cid,
    const char* fmt, va_list vargs)
{
    (void)cid;
    picoquic_esp_vlog(quic, ESP_LOG_INFO, fmt, vargs);
}

static void esp_log_quic_pdu(picoquic_quic_t* quic, int receiving, uint64_t current_time, uint64_t cid64,
    const struct sockaddr* addr_peer, const struct sockaddr* addr_local, size_t packet_length)
{
    (void)addr_peer;
    (void)addr_local;
    (void)current_time;
    if (!picoquic_esp_log_get_log_packets(quic)) {
        return;
    }
    ESP_LOGD(picoquic_esp_log_get_tag(quic), "quic pdu %s cid64=%016llx len=%u",
        (receiving != 0) ? "RX" : "TX",
        (unsigned long long)cid64, (unsigned)packet_length);
}

static void esp_log_quic_close(picoquic_quic_t* quic)
{
    quic->F_log = NULL;
    quic->text_log_fns = NULL;
    quic->should_close_log = 0;
}

static void esp_log_app_message(picoquic_cnx_t* cnx, const char* fmt, va_list vargs)
{
    picoquic_esp_vlog(cnx->quic, ESP_LOG_INFO, fmt, vargs);
}

static void esp_log_pdu(picoquic_cnx_t* cnx, int receiving, uint64_t current_time,
    const struct sockaddr* addr_peer, const struct sockaddr* addr_local, size_t packet_length,
    uint64_t unique_path_id, unsigned char ecn)
{
    (void)addr_peer;
    (void)addr_local;
    (void)current_time;
    (void)unique_path_id;
    (void)ecn;
    if (!picoquic_esp_log_get_log_packets(cnx->quic)) {
        return;
    }
    ESP_LOGD(picoquic_esp_log_get_tag(cnx->quic), "pdu %s len=%u", (receiving != 0) ? "RX" : "TX", (unsigned)packet_length);
}

static const char* esp_log_ptype_name(picoquic_packet_type_enum ptype)
{
    switch (ptype) {
    case picoquic_packet_initial: return "initial";
    case picoquic_packet_retry: return "retry";
    case picoquic_packet_handshake: return "handshake";
    case picoquic_packet_0rtt_protected: return "0rtt";
    case picoquic_packet_1rtt_protected: return "1rtt";
    case picoquic_packet_version_negotiation: return "vn";
    default: return "other";
    }
}

static void esp_log_packet(picoquic_cnx_t* cnx, picoquic_path_t* path_x, int receiving, uint64_t current_time,
    struct st_picoquic_packet_header_t* ph, const uint8_t* bytes, size_t bytes_max)
{
    (void)path_x;
    (void)current_time;
    (void)bytes;
    (void)bytes_max;
    if (!picoquic_esp_log_get_log_packets(cnx->quic)) {
        return;
    }
    ESP_LOGD(picoquic_esp_log_get_tag(cnx->quic), "pkt %s type=%s pn=%u",
        (receiving != 0) ? "RX" : "TX", esp_log_ptype_name(ph->ptype), (unsigned)ph->pn);
}

static void esp_log_dropped_packet(picoquic_cnx_t* cnx, picoquic_path_t* path_x,
    struct st_picoquic_packet_header_t* ph, size_t packet_size, int err, uint64_t current_time)
{
    (void)path_x;
    (void)current_time;
    ESP_LOGW(picoquic_esp_log_get_tag(cnx->quic), "dropped pkt type=%s pn=%u size=%u err=%d",
        esp_log_ptype_name(ph->ptype), (unsigned)ph->pn, (unsigned)packet_size, err);
}

static void esp_log_buffered_packet(picoquic_cnx_t* cnx, picoquic_path_t* path_x, picoquic_packet_type_enum ptype, uint64_t current_time)
{
    (void)path_x;
    (void)current_time;
    ESP_LOGD(picoquic_esp_log_get_tag(cnx->quic), "buffered pkt type=%s (keys unavailable)", esp_log_ptype_name(ptype));
}

static void esp_log_outgoing_packet(picoquic_cnx_t* cnx, picoquic_path_t* path_x,
    uint8_t* bytes, uint64_t sequence_number, size_t pn_length, size_t length,
    uint8_t* send_buffer, size_t send_length, uint64_t current_time)
{
    (void)path_x;
    (void)bytes;
    (void)pn_length;
    (void)send_buffer;
    (void)send_length;
    (void)current_time;
    if (!picoquic_esp_log_get_log_packets(cnx->quic)) {
        return;
    }
    ESP_LOGD(picoquic_esp_log_get_tag(cnx->quic), "outgoing pkt seq=%llu len=%u",
        (unsigned long long)sequence_number, (unsigned)length);
}

static void esp_log_packet_lost(picoquic_cnx_t* cnx, picoquic_path_t* path_x,
    picoquic_packet_type_enum ptype, uint64_t sequence_number, char const* trigger,
    picoquic_connection_id_t* dcid, size_t packet_size, uint64_t current_time)
{
    (void)path_x;
    (void)dcid;
    (void)current_time;
    ESP_LOGI(picoquic_esp_log_get_tag(cnx->quic), "lost pkt type=%s seq=%llu size=%u reason=%s",
        esp_log_ptype_name(ptype), (unsigned long long)sequence_number, (unsigned)packet_size,
        (trigger != NULL) ? trigger : "?");
}

static void esp_log_negotiated_alpn(picoquic_cnx_t* cnx, int is_local,
    uint8_t const* sni, size_t sni_len, uint8_t const* alpn, size_t alpn_len,
    const ptls_iovec_t* alpn_list, size_t alpn_count)
{
    (void)is_local;
    (void)sni;
    (void)sni_len;
    (void)alpn;
    (void)alpn_len;
    (void)alpn_list;
    (void)alpn_count;
    ESP_LOGD(picoquic_esp_log_get_tag(cnx->quic), "ALPN negotiation: count=%u", (unsigned)alpn_count);
}

static void esp_log_transport_extension(picoquic_cnx_t* cnx, int is_local, size_t param_length, uint8_t* params)
{
    (void)is_local;
    (void)params;
    ESP_LOGD(picoquic_esp_log_get_tag(cnx->quic), "transport params: %u bytes", (unsigned)param_length);
}

static void esp_log_tls_ticket(picoquic_cnx_t* cnx, uint8_t* ticket, uint16_t ticket_length)
{
    (void)ticket;
    ESP_LOGD(picoquic_esp_log_get_tag(cnx->quic), "TLS ticket: %u bytes", (unsigned)ticket_length);
}

static void esp_log_new_connection(picoquic_cnx_t* cnx)
{
    ESP_LOGI(picoquic_esp_log_get_tag(cnx->quic), "new connection");
}

static void esp_log_close_connection(picoquic_cnx_t* cnx)
{
    ESP_LOGI(picoquic_esp_log_get_tag(cnx->quic), "connection closed");
}

static void esp_log_cc_dump(picoquic_cnx_t* cnx, uint64_t current_time)
{
    (void)current_time;
    ESP_LOGD(picoquic_esp_log_get_tag(cnx->quic), "cc dump");
}

static struct st_picoquic_unified_logging_t esp_log_functions = {
    /* Per context log function */
    esp_log_quic_app_message,
    esp_log_quic_pdu,
    esp_log_quic_close,
    /* Per connection functions */
    esp_log_app_message,
    esp_log_pdu,
    esp_log_packet,
    esp_log_dropped_packet,
    esp_log_buffered_packet,
    esp_log_outgoing_packet,
    esp_log_packet_lost,
    esp_log_negotiated_alpn,
    esp_log_transport_extension,
    esp_log_tls_ticket,
    esp_log_new_connection,
    esp_log_close_connection,
    esp_log_cc_dump
};

int picoquic_set_esp_log(picoquic_quic_t* quic, const char* tag, int log_packets)
{
    if (quic == NULL) {
        return -1;
    }

    /* Close existing text logger (if any), respecting its own cleanup. */
    if (quic->text_log_fns != NULL && quic->text_log_fns->log_quic_close != NULL) {
        quic->text_log_fns->log_quic_close(quic);
    }

    g_picoquic_esp_log_tag = (tag != NULL && tag[0] != 0) ? tag : "picoquic";
    g_picoquic_esp_log_packets = (log_packets != 0);

    /* If packet logs are requested, make sure DEBUG logs are visible for this tag.
     * (Only works if ESP-IDF is built with dynamic log level control enabled.)
     */
    if (g_picoquic_esp_log_packets) {
        esp_log_level_set(g_picoquic_esp_log_tag, ESP_LOG_DEBUG);
    }

    /* Any non-NULL value enables unified text logging.
     * Keep this as a valid FILE* because picoquic will fflush() it in a few places.
     */
    quic->F_log = stdout;
    quic->text_log_fns = &esp_log_functions;
    quic->should_close_log = 0;

    return 0;
}

#else /* ESP_PLATFORM */

int picoquic_set_esp_log(picoquic_quic_t* quic, const char* tag, int log_packets)
{
    (void)quic;
    (void)tag;
    (void)log_packets;
    return -1;
}

#endif /* ESP_PLATFORM */


