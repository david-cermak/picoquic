#include "mqtt_picoquic_transport.h"

#include <errno.h>
#include <string.h>

#include <algorithm>
#include <condition_variable>
#include <mutex>
#include <string>
#include <vector>

#include <picoquic.h>
#include <picoquic_utils.h>
#include <picoquic_packet_loop.h>
#include "picoquic_bbr.h"
#include "picoquic_esp_log.h"

#include "esp_log.h"
#include "esp_timer.h"

namespace {

static const char *TAG = "mqtt_picoquic_transport";

static constexpr const char *kAlpn = "mqtt";
static constexpr size_t kMaxBuffered = 256 * 1024; // safety cap (RX+TX)

struct picoquic_mqtt_ctx {
    picoquic_quic_t *quic = nullptr;
    picoquic_cnx_t *cnx = nullptr;
    picoquic_network_thread_ctx_t *net = nullptr;
    picoquic_packet_loop_param_t loop_param = {};

    uint64_t stream_id = UINT64_MAX;
    bool ready = false;
    bool closed = false;

    std::mutex mu;
    std::condition_variable cv_state;
    std::condition_variable cv_rx;
    std::condition_variable cv_tx;

    std::vector<uint8_t> rx;
    std::vector<uint8_t> tx;
};

static int loop_cb(picoquic_quic_t *quic, picoquic_packet_loop_cb_enum cb_mode, void *callback_ctx, void *callback_arg)
{
    (void)quic;
    (void)callback_arg;
    auto *ctx = (picoquic_mqtt_ctx *)callback_ctx;
    if (!ctx) {
        return PICOQUIC_ERROR_UNEXPECTED_ERROR;
    }

    if (cb_mode == picoquic_packet_loop_wake_up) {
        // This callback runs in the network thread, so it's safe to call picoquic APIs here.
        std::unique_lock<std::mutex> lk(ctx->mu);
        if (ctx->closed || ctx->cnx == nullptr || ctx->stream_id == UINT64_MAX) {
            return 0;
        }
        if (!ctx->tx.empty()) {
            (void)picoquic_mark_active_stream(ctx->cnx, ctx->stream_id, 1, nullptr);
        }
        return 0;
    }

    if (cb_mode == picoquic_packet_loop_after_send) {
        std::unique_lock<std::mutex> lk(ctx->mu);
        if (ctx->closed) {
            return PICOQUIC_NO_ERROR_TERMINATE_PACKET_LOOP;
        }
        return 0;
    }

    return 0;
}

static int mqtt_client_callback(picoquic_cnx_t *cnx,
                                uint64_t stream_id,
                                uint8_t *bytes,
                                size_t length,
                                picoquic_call_back_event_t fin_or_event,
                                void *callback_ctx,
                                void *v_stream_ctx)
{
    (void)v_stream_ctx;
    auto *ctx = (picoquic_mqtt_ctx *)callback_ctx;
    if (!ctx) {
        return -1;
    }

    switch (fin_or_event) {
    case picoquic_callback_ready: {
        std::unique_lock<std::mutex> lk(ctx->mu);
        if (!ctx->ready) {
            ctx->stream_id = picoquic_get_next_local_stream_id(cnx, 0 /* bidir */);
            (void)picoquic_mark_active_stream(cnx, ctx->stream_id, 0, nullptr);
            ctx->ready = true;
            ESP_LOGI(TAG, "QUIC ready (ALPN=%s), opened stream=%" PRIu64,
                     picoquic_tls_get_negotiated_alpn(cnx), ctx->stream_id);
            ctx->cv_state.notify_all();
        }
        break;
    }

    case picoquic_callback_prepare_to_send: {
        std::unique_lock<std::mutex> lk(ctx->mu);
        if (ctx->closed) {
            return -1;
        }
        if (ctx->stream_id == UINT64_MAX || stream_id != ctx->stream_id) {
            break;
        }

        if (!ctx->tx.empty() && length > 0) {
            size_t n = std::min(length, ctx->tx.size());
            int still_active = (ctx->tx.size() > n) ? 1 : 0;
            uint8_t *buf = picoquic_provide_stream_data_buffer(bytes, n, 0 /* fin */, still_active);
            if (buf == nullptr) {
                ESP_LOGE(TAG, "picoquic_provide_stream_data_buffer failed");
                return -1;
            }
            memcpy(buf, ctx->tx.data(), n);
            ctx->tx.erase(ctx->tx.begin(), ctx->tx.begin() + (ptrdiff_t)n);
            ctx->cv_tx.notify_all();
        }
        break;
    }

    case picoquic_callback_stream_data:
    case picoquic_callback_stream_fin: {
        std::unique_lock<std::mutex> lk(ctx->mu);
        if (ctx->stream_id != UINT64_MAX && stream_id == ctx->stream_id && length > 0) {
            if (ctx->rx.size() + length + ctx->tx.size() > kMaxBuffered) {
                ESP_LOGE(TAG, "buffer cap exceeded, closing");
                ctx->closed = true;
                ctx->cv_state.notify_all();
                ctx->cv_rx.notify_all();
                ctx->cv_tx.notify_all();
                (void)picoquic_close(cnx, 0);
                break;
            }
            size_t old = ctx->rx.size();
            ctx->rx.resize(old + length);
            memcpy(ctx->rx.data() + old, bytes, length);
            ctx->cv_rx.notify_all();
        }
        if (fin_or_event == picoquic_callback_stream_fin) {
            ctx->closed = true;
            ctx->cv_state.notify_all();
            ctx->cv_rx.notify_all();
            ctx->cv_tx.notify_all();
        }
        break;
    }

    case picoquic_callback_close:
    case picoquic_callback_application_close:
    case picoquic_callback_stateless_reset: {
        std::unique_lock<std::mutex> lk(ctx->mu);
        ctx->closed = true;
        ctx->cv_state.notify_all();
        ctx->cv_rx.notify_all();
        ctx->cv_tx.notify_all();
        break;
    }

    default:
        break;
    }

    return 0;
}

static int tp_connect(esp_transport_handle_t t, const char *host, int port, int timeout_ms)
{
    auto *ctx = (picoquic_mqtt_ctx *)esp_transport_get_context_data(t);
    if (!ctx || !host) {
        errno = EINVAL;
        return -1;
    }

    // Clean any previous state (best-effort)
    {
        std::unique_lock<std::mutex> lk(ctx->mu);
        ctx->ready = false;
        ctx->closed = false;
        ctx->stream_id = UINT64_MAX;
        ctx->rx.clear();
        ctx->tx.clear();
    }

    struct sockaddr_storage server_address;
    int is_name = 0;
    int ret = picoquic_get_server_address(host, port, &server_address, &is_name);
    if (ret != 0) {
        ESP_LOGE(TAG, "picoquic_get_server_address(%s:%d) failed: %d", host, port, ret);
        errno = EHOSTUNREACH;
        return -1;
    }

    uint64_t now = picoquic_current_time();
    ctx->quic = picoquic_create(1, NULL, NULL, NULL, kAlpn, NULL, NULL, NULL, NULL, NULL, now, NULL, NULL, NULL, 0);
    if (ctx->quic == nullptr) {
        errno = ENOMEM;
        return -1;
    }
    picoquic_set_default_congestion_algorithm(ctx->quic, picoquic_bbr_algorithm);
    picoquic_set_log_level(ctx->quic, 1);
    (void)picoquic_set_esp_log(ctx->quic, TAG, 0 /* log_packets */);

    ctx->cnx = picoquic_create_cnx(ctx->quic,
                                  picoquic_null_connection_id,
                                  picoquic_null_connection_id,
                                  (struct sockaddr *)&server_address,
                                  now,
                                  0,
                                  host /* sni */,
                                  kAlpn,
                                  1);
    if (ctx->cnx == nullptr) {
        picoquic_free(ctx->quic);
        ctx->quic = nullptr;
        errno = ECONNREFUSED;
        return -1;
    }

    picoquic_set_callback(ctx->cnx, mqtt_client_callback, ctx);
    ret = picoquic_start_client_cnx(ctx->cnx);
    if (ret < 0) {
        ESP_LOGE(TAG, "picoquic_start_client_cnx failed: %d", ret);
        picoquic_free(ctx->quic);
        ctx->quic = nullptr;
        ctx->cnx = nullptr;
        errno = ECONNREFUSED;
        return -1;
    }

    // Start background network thread
    memset(&ctx->loop_param, 0, sizeof(ctx->loop_param));
    ctx->loop_param.local_port = 0;
    ctx->loop_param.local_af = server_address.ss_family;
    ctx->loop_param.dest_if = 0;

    int thread_ret = 0;
    ctx->net = picoquic_start_network_thread(ctx->quic, &ctx->loop_param, loop_cb, ctx, &thread_ret);
    if (ctx->net == nullptr || thread_ret != 0) {
        ESP_LOGE(TAG, "picoquic_start_network_thread failed: %d", thread_ret);
        picoquic_free(ctx->quic);
        ctx->quic = nullptr;
        ctx->cnx = nullptr;
        ctx->net = nullptr;
        errno = ECONNREFUSED;
        return -1;
    }

    // Wait until connection is ready (stream opened) or timeout
    const uint64_t start_ms = esp_timer_get_time() / 1000;
    std::unique_lock<std::mutex> lk(ctx->mu);
    while (!ctx->ready && !ctx->closed) {
        if (timeout_ms == 0) {
            errno = ETIMEDOUT;
            return -1;
        }
        const uint64_t now_ms = esp_timer_get_time() / 1000;
        if (timeout_ms > 0 && (now_ms - start_ms) >= (uint64_t)timeout_ms) {
            errno = ETIMEDOUT;
            return -1;
        }
        ctx->cv_state.wait_for(lk, std::chrono::milliseconds(50));
    }
    if (!ctx->ready) {
        errno = ECONNRESET;
        return -1;
    }

    return 0;
}

static int tp_poll_read(esp_transport_handle_t t, int timeout_ms)
{
    auto *ctx = (picoquic_mqtt_ctx *)esp_transport_get_context_data(t);
    if (!ctx) {
        errno = EINVAL;
        return -1;
    }
    std::unique_lock<std::mutex> lk(ctx->mu);
    if (!ctx->rx.empty()) {
        return 1;
    }
    if (ctx->closed) {
        return -1;
    }
    if (timeout_ms == 0) {
        return 0;
    }
    if (timeout_ms < 0) {
        ctx->cv_rx.wait(lk, [&]() { return !ctx->rx.empty() || ctx->closed; });
    } else {
        ctx->cv_rx.wait_for(lk, std::chrono::milliseconds(timeout_ms), [&]() { return !ctx->rx.empty() || ctx->closed; });
    }
    if (!ctx->rx.empty()) {
        return 1;
    }
    if (ctx->closed) {
        return -1;
    }
    return 0;
}

static int tp_read(esp_transport_handle_t t, char *buffer, int len, int timeout_ms)
{
    if (!buffer || len <= 0) {
        errno = EINVAL;
        return -1;
    }
    auto *ctx = (picoquic_mqtt_ctx *)esp_transport_get_context_data(t);
    if (!ctx) {
        errno = EINVAL;
        return -1;
    }

    int pr = tp_poll_read(t, timeout_ms);
    if (pr < 0) {
        return ERR_TCP_TRANSPORT_CONNECTION_CLOSED_BY_FIN;
    }
    if (pr == 0) {
        return ERR_TCP_TRANSPORT_CONNECTION_TIMEOUT; // == 0
    }

    std::unique_lock<std::mutex> lk(ctx->mu);
    int n = (int)std::min((size_t)len, ctx->rx.size());
    memcpy(buffer, ctx->rx.data(), (size_t)n);
    ctx->rx.erase(ctx->rx.begin(), ctx->rx.begin() + n);
    return n;
}

static int tp_write(esp_transport_handle_t t, const char *buffer, int len, int timeout_ms)
{
    if (!buffer || len <= 0) {
        errno = EINVAL;
        return -1;
    }
    auto *ctx = (picoquic_mqtt_ctx *)esp_transport_get_context_data(t);
    if (!ctx) {
        errno = EINVAL;
        return -1;
    }

    const uint64_t start_ms = esp_timer_get_time() / 1000;
    std::unique_lock<std::mutex> lk(ctx->mu);
    while (!ctx->ready && !ctx->closed) {
        if (timeout_ms == 0) {
            errno = ETIMEDOUT;
            return 0;
        }
        const uint64_t now_ms = esp_timer_get_time() / 1000;
        if (timeout_ms > 0 && (now_ms - start_ms) >= (uint64_t)timeout_ms) {
            errno = ETIMEDOUT;
            return 0;
        }
        ctx->cv_state.wait_for(lk, std::chrono::milliseconds(10));
    }
    if (ctx->closed) {
        errno = ECONNRESET;
        return -1;
    }

    if (ctx->rx.size() + ctx->tx.size() + (size_t)len > kMaxBuffered) {
        errno = ENOBUFS;
        return -1;
    }

    size_t old = ctx->tx.size();
    ctx->tx.resize(old + (size_t)len);
    memcpy(ctx->tx.data() + old, buffer, (size_t)len);

    // Wake the network thread so it can mark stream active and flush tx.
    picoquic_network_thread_ctx_t *net = ctx->net;
    lk.unlock();
    if (net) {
        (void)picoquic_wake_up_network_thread(net);
    }
    return len;
}

static int tp_poll_write(esp_transport_handle_t t, int timeout_ms)
{
    (void)timeout_ms;
    auto *ctx = (picoquic_mqtt_ctx *)esp_transport_get_context_data(t);
    if (!ctx) {
        return -1;
    }
    std::unique_lock<std::mutex> lk(ctx->mu);
    if (ctx->closed) {
        return -1;
    }
    return 1;
}

static int tp_close(esp_transport_handle_t t)
{
    auto *ctx = (picoquic_mqtt_ctx *)esp_transport_get_context_data(t);
    if (!ctx) {
        return 0;
    }

    picoquic_network_thread_ctx_t *net = nullptr;
    picoquic_cnx_t *cnx = nullptr;
    picoquic_quic_t *quic = nullptr;
    {
        std::unique_lock<std::mutex> lk(ctx->mu);
        ctx->closed = true;
        net = ctx->net;
        cnx = ctx->cnx;
        quic = ctx->quic;
        ctx->cv_state.notify_all();
        ctx->cv_rx.notify_all();
        ctx->cv_tx.notify_all();
    }

    if (cnx) {
        (void)picoquic_close(cnx, 0);
    }
    if (net) {
        (void)picoquic_wake_up_network_thread(net);
        picoquic_delete_network_thread(net);
        ctx->net = nullptr;
    }
    if (quic) {
        picoquic_free(quic);
        ctx->quic = nullptr;
        ctx->cnx = nullptr;
    }

    return 0;
}

static int tp_destroy(esp_transport_handle_t t)
{
    auto *ctx = (picoquic_mqtt_ctx *)esp_transport_get_context_data(t);
    if (!ctx) {
        return 0;
    }
    tp_close(t);
    delete ctx;
    esp_transport_set_context_data(t, nullptr);
    return 0;
}

} // namespace

extern "C" esp_transport_handle_t esp_transport_picoquic_mqtt_init(void)
{
    esp_transport_handle_t t = esp_transport_init();
    if (!t) {
        return nullptr;
    }
    auto *ctx = new picoquic_mqtt_ctx();
    esp_transport_set_context_data(t, ctx);
    esp_transport_set_default_port(t, 14567);
    esp_transport_set_func(t, tp_connect, tp_read, tp_write, tp_close, tp_poll_read, tp_poll_write, tp_destroy);
    return t;
}


