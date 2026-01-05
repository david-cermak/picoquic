#include "mqtt_quic_transport.h"

#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <sys/socket.h>
#include <netdb.h>
#include <unistd.h>

#include <string>
#include <vector>

#include "esp_http3.h"
#include "esp_log.h"
#include "esp_timer.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

namespace {

static const char *TAG = "mqtt_quic_transport";

struct quic_mqtt_ctx {
    int sock = -1;
    esp_http3::QuicConfig qc;
    esp_http3::QuicConnection *conn = nullptr;
    int stream_id = -1;
    bool connected = false;
    bool disconnected = false;
    int disconnect_code = 0;
    std::string disconnect_reason;

    // Simple in-memory RX buffer (append on stream callback, pop on read)
    std::vector<uint8_t> rx;
};

static void ctx_close_socket(quic_mqtt_ctx *ctx)
{
    if (!ctx) {
        return;
    }
    if (ctx->sock >= 0) {
        close(ctx->sock);
        ctx->sock = -1;
    }
}

static int set_nonblocking(int fd)
{
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags < 0) {
        return -1;
    }
    return fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

static int udp_connect(quic_mqtt_ctx *ctx, const char *host, int port)
{
    if (!ctx || !host) {
        errno = EINVAL;
        return -1;
    }

    char port_str[16];
    snprintf(port_str, sizeof(port_str), "%d", port);

    struct addrinfo hints;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_INET;      // keep this aligned with the existing PoC
    hints.ai_socktype = SOCK_DGRAM;

    struct addrinfo *res = nullptr;
    int err = getaddrinfo(host, port_str, &hints, &res);
    if (err != 0 || res == nullptr) {
        ESP_LOGE(TAG, "getaddrinfo(%s:%s) failed: err=%d", host, port_str, err);
        errno = EHOSTUNREACH;
        return -1;
    }

    int sock = socket(res->ai_family, res->ai_socktype, 0);
    if (sock < 0) {
        ESP_LOGE(TAG, "socket() failed: errno=%d", errno);
        freeaddrinfo(res);
        return -1;
    }

    if (connect(sock, res->ai_addr, res->ai_addrlen) != 0) {
        ESP_LOGE(TAG, "connect(udp) failed: errno=%d", errno);
        close(sock);
        freeaddrinfo(res);
        return -1;
    }
    freeaddrinfo(res);

    if (set_nonblocking(sock) != 0) {
        ESP_LOGW(TAG, "failed to set O_NONBLOCK: errno=%d", errno);
        // not fatal
    }

    ctx->sock = sock;
    return 0;
}

static void quic_pump_one(quic_mqtt_ctx *ctx, uint32_t tick_ms)
{
    if (!ctx || !ctx->conn) {
        return;
    }

    uint8_t rxbuf[1500];
    for (;;) {
        int r = recv(ctx->sock, rxbuf, sizeof(rxbuf), 0);
        if (r > 0) {
            ctx->conn->ProcessReceivedData(rxbuf, (size_t)r);
            continue;
        }
        if (r < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            break;
        }
        // For UDP, r==0 is unusual; treat as no data
        break;
    }

    ctx->conn->OnTimerTick(tick_ms);
}

static int quic_connect(esp_transport_handle_t t, const char *host, int port, int timeout_ms)
{
    auto *ctx = (quic_mqtt_ctx *)esp_transport_get_context_data(t);
    if (!ctx) {
        errno = EINVAL;
        return -1;
    }

    // If we ever reconnect using the same transport, reset state
    ctx_close_socket(ctx);
    ctx->rx.clear();
    ctx->connected = false;
    ctx->disconnected = false;
    ctx->disconnect_code = 0;
    ctx->disconnect_reason.clear();
    ctx->stream_id = -1;
    delete ctx->conn;
    ctx->conn = nullptr;

    if (udp_connect(ctx, host, port) != 0) {
        return -1;
    }

    ctx->qc.hostname = host ? host : "";
    ctx->qc.port = (uint16_t)port;
    ctx->qc.alpn = "mqtt";
    ctx->qc.enable_http3 = false;
    ctx->qc.enable_debug = false;
    if (timeout_ms > 0) {
        ctx->qc.handshake_timeout_ms = (uint32_t)timeout_ms;
    }

    ctx->conn = new esp_http3::QuicConnection(
        [ctx](const uint8_t *data, size_t len) -> int {
            int n = send(ctx->sock, data, len, 0);
            return (n < 0) ? -1 : n;
        },
        ctx->qc);

    ctx->conn->SetOnConnected([ctx]() {
        ESP_LOGI(TAG, "QUIC connected (ALPN=mqtt)");
        ctx->connected = true;
    });
    ctx->conn->SetOnDisconnected([ctx](int code, const std::string &reason) {
        ESP_LOGW(TAG, "QUIC disconnected: code=%d reason=%s", code, reason.c_str());
        ctx->disconnected = true;
        ctx->disconnect_code = code;
        ctx->disconnect_reason = reason;
    });
    ctx->conn->SetOnStreamData([ctx](int stream_id, const uint8_t *data, size_t len, bool fin) {
        if (stream_id != ctx->stream_id) {
            // ignore other streams
            return;
        }
        ctx->rx.insert(ctx->rx.end(), data, data + len);
        if (fin) {
            ctx->disconnected = true;
        }
    });

    if (!ctx->conn->StartHandshake()) {
        ESP_LOGE(TAG, "StartHandshake failed");
        return -1;
    }

    const uint64_t start_ms = esp_timer_get_time() / 1000;
    const uint32_t max_wait_ms = (timeout_ms > 0) ? (uint32_t)timeout_ms : ctx->qc.handshake_timeout_ms;
    while (!ctx->connected && !ctx->disconnected) {
        const uint64_t now_ms = esp_timer_get_time() / 1000;
        if (now_ms - start_ms > max_wait_ms + 2000) {
            ESP_LOGE(TAG, "Handshake timeout");
            errno = ETIMEDOUT;
            return -1;
        }
        quic_pump_one(ctx, 20);
        vTaskDelay(pdMS_TO_TICKS(20));
    }
    if (!ctx->connected) {
        errno = ECONNRESET;
        return -1;
    }

    ctx->stream_id = ctx->conn->OpenBidirectionalStream();
    if (ctx->stream_id < 0) {
        ESP_LOGE(TAG, "OpenBidirectionalStream failed");
        errno = ECONNRESET;
        return -1;
    }

    return 0;
}

static int quic_poll_read(esp_transport_handle_t t, int timeout_ms)
{
    auto *ctx = (quic_mqtt_ctx *)esp_transport_get_context_data(t);
    if (!ctx || !ctx->conn) {
        errno = EINVAL;
        return -1;
    }

    if (!ctx->rx.empty()) {
        return 1;
    }
    if (ctx->disconnected) {
        return -1;
    }

    const uint64_t start_ms = esp_timer_get_time() / 1000;
    for (;;) {
        if (!ctx->rx.empty()) {
            return 1;
        }
        if (ctx->disconnected) {
            return -1;
        }
        if (timeout_ms == 0) {
            return 0;
        }
        const uint64_t now_ms = esp_timer_get_time() / 1000;
        if (timeout_ms > 0 && (now_ms - start_ms) >= (uint64_t)timeout_ms) {
            return 0;
        }

        quic_pump_one(ctx, 10);
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}

static int quic_read(esp_transport_handle_t t, char *buffer, int len, int timeout_ms)
{
    if (!buffer || len <= 0) {
        errno = EINVAL;
        return -1;
    }
    auto *ctx = (quic_mqtt_ctx *)esp_transport_get_context_data(t);
    if (!ctx || !ctx->conn) {
        errno = EINVAL;
        return -1;
    }

    // Fast path
    if (!ctx->rx.empty()) {
        int n = (int)ctx->rx.size();
        if (n > len) {
            n = len;
        }
        memcpy(buffer, ctx->rx.data(), (size_t)n);
        ctx->rx.erase(ctx->rx.begin(), ctx->rx.begin() + n);
        return n;
    }

    if (ctx->disconnected) {
        return ERR_TCP_TRANSPORT_CONNECTION_CLOSED_BY_FIN;
    }

    int pr = quic_poll_read(t, timeout_ms);
    if (pr < 0) {
        return ERR_TCP_TRANSPORT_CONNECTION_CLOSED_BY_FIN;
    }
    if (pr == 0) {
        return ERR_TCP_TRANSPORT_CONNECTION_TIMEOUT; // must be 0 for esp-mqtt
    }

    // Now there should be some data
    if (ctx->rx.empty()) {
        return ERR_TCP_TRANSPORT_CONNECTION_TIMEOUT;
    }
    int n = (int)ctx->rx.size();
    if (n > len) {
        n = len;
    }
    memcpy(buffer, ctx->rx.data(), (size_t)n);
    ctx->rx.erase(ctx->rx.begin(), ctx->rx.begin() + n);
    return n;
}

static int quic_write(esp_transport_handle_t t, const char *buffer, int len, int timeout_ms)
{
    if (!buffer || len <= 0) {
        errno = EINVAL;
        return -1;
    }
    auto *ctx = (quic_mqtt_ctx *)esp_transport_get_context_data(t);
    if (!ctx || !ctx->conn || ctx->stream_id < 0) {
        errno = EINVAL;
        return -1;
    }
    if (ctx->disconnected) {
        errno = ECONNRESET;
        return -1;
    }

    const uint64_t start_ms = esp_timer_get_time() / 1000;
    for (;;) {
        ssize_t w = ctx->conn->WriteStreamRaw(ctx->stream_id, (const uint8_t *)buffer, (size_t)len);
        if (w < 0) {
            errno = ECONNRESET;
            return -1;
        }
        if (w > 0) {
            return (int)w;
        }

        // Flow-control blocked: pump until writable or timeout
        if (timeout_ms == 0) {
            errno = EAGAIN;
            return 0;
        }
        const uint64_t now_ms = esp_timer_get_time() / 1000;
        if (timeout_ms > 0 && (now_ms - start_ms) >= (uint64_t)timeout_ms) {
            errno = ETIMEDOUT;
            return 0;
        }
        quic_pump_one(ctx, 10);
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}

static int quic_poll_write(esp_transport_handle_t t, int timeout_ms)
{
    (void)timeout_ms;
    auto *ctx = (quic_mqtt_ctx *)esp_transport_get_context_data(t);
    if (!ctx || !ctx->conn || ctx->disconnected) {
        return -1;
    }
    return 1;
}

static int quic_close(esp_transport_handle_t t)
{
    auto *ctx = (quic_mqtt_ctx *)esp_transport_get_context_data(t);
    if (!ctx) {
        return 0;
    }
    if (ctx->conn) {
        ctx->conn->Close(0, "close");
    }
    ctx_close_socket(ctx);
    return 0;
}

static int quic_destroy(esp_transport_handle_t t)
{
    auto *ctx = (quic_mqtt_ctx *)esp_transport_get_context_data(t);
    if (!ctx) {
        return 0;
    }
    quic_close(t);
    delete ctx->conn;
    ctx->conn = nullptr;
    delete ctx;
    esp_transport_set_context_data(t, nullptr);
    return 0;
}

} // namespace

extern "C" esp_transport_handle_t esp_transport_quic_mqtt_init(void)
{
    esp_transport_handle_t t = esp_transport_init();
    if (!t) {
        return nullptr;
    }

    auto *ctx = new quic_mqtt_ctx();
    esp_transport_set_context_data(t, ctx);

    // Default QUIC MQTT port (EMQX demo used by the PoC)
    esp_transport_set_default_port(t, 14567);

    esp_transport_set_func(t, quic_connect, quic_read, quic_write, quic_close, quic_poll_read, quic_poll_write, quic_destroy);
    return t;
}


