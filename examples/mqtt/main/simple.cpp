#include <stdio.h>
#include <string.h>
#include <errno.h>

#include "esp_http3.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "esp_event.h"
#include "esp_timer.h"
#include "protocol_examples_common.h"

#include "sys/socket.h"
#include "netdb.h"
#include "errno.h"
#include "fcntl.h"

#define TAG "mqtt"

extern "C" void app_main(void)
{

        // Initialize ESP-IDF components
        ESP_ERROR_CHECK(nvs_flash_init());
        // ESP_ERROR_CHECK(esp_netif_init());
        ESP_ERROR_CHECK(esp_event_loop_create_default());
        // ESP_ERROR_CHECK(example_connect());

        // MQTT-over-QUIC test: connect to broker.emqx.io:14567 using ALPN "mqtt"
        static constexpr const char* kHost = "broker.emqx.io";
        static constexpr const char* kPortStr = "14567";
        static constexpr uint16_t kPort = 14567;

        // Resolve hostname
        struct addrinfo hints;
        memset(&hints, 0, sizeof(hints));
        hints.ai_family = AF_INET;
        hints.ai_socktype = SOCK_DGRAM;

        struct addrinfo* res = nullptr;
        int err = getaddrinfo(kHost, kPortStr, &hints, &res);
        if (err != 0 || res == nullptr) {
                ESP_LOGE(TAG, "getaddrinfo(%s:%s) failed: err=%d", kHost, kPortStr, err);
                return;
        }

        int sock = socket(res->ai_family, res->ai_socktype, 0);
        if (sock < 0) {
                ESP_LOGE(TAG, "socket() failed: errno=%d", errno);
                freeaddrinfo(res);
                return;
        }

        if (connect(sock, res->ai_addr, res->ai_addrlen) != 0) {
                ESP_LOGE(TAG, "connect() failed: errno=%d", errno);
                close(sock);
                freeaddrinfo(res);
                return;
        }
        freeaddrinfo(res);

        // Non-blocking receive loop
        int flags = fcntl(sock, F_GETFL, 0);
        if (flags >= 0) {
                (void)fcntl(sock, F_SETFL, flags | O_NONBLOCK);
        }

        // Configure QUIC connection
        esp_http3::QuicConfig qc;
        qc.hostname = kHost;
        qc.port = kPort;
        qc.alpn = "mqtt";
        qc.enable_http3 = false;  // critical: don't send any HTTP/3 control/settings bytes
        qc.enable_debug = false;

        volatile bool connected = false;
        volatile bool got_any_data = false;

        esp_http3::QuicConnection conn(
                [sock](const uint8_t* data, size_t len) -> int {
                        int n = send(sock, data, len, 0);
                        return (n < 0) ? -1 : n;
                },
                qc);

        conn.SetOnConnected([&]() {
                ESP_LOGI(TAG, "QUIC connected (ALPN=mqtt)");
                connected = true;
        });
        conn.SetOnDisconnected([&](int code, const std::string& reason) {
                ESP_LOGE(TAG, "QUIC disconnected: code=%d reason=%s", code, reason.c_str());
        });
        conn.SetOnStreamData([&](int stream_id, const uint8_t* data, size_t len, bool fin) {
                got_any_data = true;
                ESP_LOGI(TAG, "RX stream=%d len=%u fin=%d", stream_id, (unsigned)len, fin ? 1 : 0);
                // Print first few bytes as hex (good enough to spot CONNACK: 20 02 00 00)
                char line[128];
                size_t n = (len > 16) ? 16 : len;
                size_t off = 0;
                for (size_t i = 0; i < n && off + 3 < sizeof(line); i++) {
                        off += snprintf(line + off, sizeof(line) - off, "%02X ", data[i]);
                }
                line[off] = 0;
                ESP_LOGI(TAG, "RX bytes (first %u): %s", (unsigned)n, line);
        });

        if (!conn.StartHandshake()) {
                ESP_LOGE(TAG, "StartHandshake failed");
                close(sock);
                return;
        }

        // Pump UDP receive + timer until connected or timeout
        uint8_t rxbuf[1500];
        const uint64_t start_ms = esp_timer_get_time() / 1000;
        while (!connected) {
                const uint64_t now_ms = esp_timer_get_time() / 1000;
                if (now_ms - start_ms > qc.handshake_timeout_ms + 2000) {
                        ESP_LOGE(TAG, "Handshake timeout");
                        close(sock);
                        return;
                }

                int r = recv(sock, rxbuf, sizeof(rxbuf), 0);
                if (r > 0) {
                        conn.ProcessReceivedData(rxbuf, (size_t)r);
                }

                conn.OnTimerTick(50);
                vTaskDelay(pdMS_TO_TICKS(50));
        }

        // Open a raw stream and send a raw MQTT CONNECT packet (MQTT 3.1.1)
        const int stream_id = conn.OpenBidirectionalStream();
        if (stream_id < 0) {
                ESP_LOGE(TAG, "OpenBidirectionalStream failed");
                close(sock);
                return;
        }

        // MQTT CONNECT:
        // Fixed header: 0x10, remaining length 0x14 (20)
        // Variable header: 00 04 'M' 'Q' 'T' 'T' 04 02 00 3C
        // Payload: 00 08 'e' 's' 'p' '-' 'q' 'u' 'i' 'c'
        static const uint8_t mqtt_connect[] = {
                0x10, 0x14,
                0x00, 0x04, 'M', 'Q', 'T', 'T',
                0x04,
                0x02,
                0x00, 0x3C,
                0x00, 0x08, 'e', 's', 'p', '-', 'q', 'u', 'i', 'c',
        };

        ssize_t sent = conn.WriteStreamRaw(stream_id, mqtt_connect, sizeof(mqtt_connect));
        ESP_LOGI(TAG, "Sent MQTT CONNECT over QUIC: stream=%d bytes=%d", stream_id, (int)sent);

        // Wait briefly for any response bytes (ideally CONNACK)
        const uint64_t wait_start_ms = esp_timer_get_time() / 1000;
        while ((esp_timer_get_time() / 1000) - wait_start_ms < 3000 && !got_any_data) {
                int r = recv(sock, rxbuf, sizeof(rxbuf), 0);
                if (r > 0) {
                        conn.ProcessReceivedData(rxbuf, (size_t)r);
                }
                conn.OnTimerTick(50);
                vTaskDelay(pdMS_TO_TICKS(50));
        }

        if (!got_any_data) {
                ESP_LOGW(TAG, "No response bytes observed yet (connection still may be OK)");
        }

        // Keep connection alive for a bit so we can see whether it stays up.
        // If the disconnect only happens after this delay, it was just app_main exiting/destructor cleanup.
        ESP_LOGI(TAG, "Holding connection open for 15 seconds...");
        const uint64_t hold_start_ms = esp_timer_get_time() / 1000;
        while ((esp_timer_get_time() / 1000) - hold_start_ms < 15000) {
                int r = recv(sock, rxbuf, sizeof(rxbuf), 0);
                if (r > 0) {
                        conn.ProcessReceivedData(rxbuf, (size_t)r);
                }
                conn.OnTimerTick(100);
                vTaskDelay(pdMS_TO_TICKS(100));
        }

        conn.Close(0, "done");
        close(sock);

}
