#include <stdio.h>
#include <string.h>

#include "esp_log.h"
#include "nvs_flash.h"
#include "esp_event.h"
#include "esp_timer.h"
#include "mqtt_client.h"
#include "mqtt_quic_transport.h"
#include "protocol_examples_common.h"
#include "esp_netif.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#define TAG "mqtt"

static volatile bool s_connected = false;

static void mqtt_event_handler(void *handler_args, esp_event_base_t base, int32_t event_id, void *event_data)
{
    ESP_LOGD(TAG, "Event dispatched from event loop base=%s, event_id=%" PRIi32, base, event_id);
    esp_mqtt_event_handle_t event = (esp_mqtt_event_handle_t)event_data;
    esp_mqtt_client_handle_t client = event->client;
    int msg_id;
    switch ((esp_mqtt_event_id_t)event_id) {
    case MQTT_EVENT_CONNECTED:
        s_connected = true;
        ESP_LOGI(TAG, "MQTT_EVENT_CONNECTED");
        msg_id = esp_mqtt_client_subscribe(client, "topic/qos0", 0);
        ESP_LOGI(TAG, "sent subscribe successful, msg_id=%d", msg_id);

        msg_id = esp_mqtt_client_subscribe(client, "topic/qos1", 1);
        ESP_LOGI(TAG, "sent subscribe successful, msg_id=%d", msg_id);

        msg_id = esp_mqtt_client_unsubscribe(client, "topic/qos1");
        ESP_LOGI(TAG, "sent unsubscribe successful, msg_id=%d", msg_id);
        break;
    case MQTT_EVENT_DISCONNECTED:
        ESP_LOGI(TAG, "MQTT_EVENT_DISCONNECTED");
        s_connected = false;
        break;

    case MQTT_EVENT_SUBSCRIBED:
        ESP_LOGI(TAG, "MQTT_EVENT_SUBSCRIBED, msg_id=%d, return code=0x%02x ", event->msg_id, (uint8_t)*event->data);
        msg_id = esp_mqtt_client_publish(client, "topic/qos0", "data", 0, 0, 0);
        ESP_LOGI(TAG, "sent publish successful, msg_id=%d", msg_id);
        break;
    case MQTT_EVENT_UNSUBSCRIBED:
        ESP_LOGI(TAG, "MQTT_EVENT_UNSUBSCRIBED, msg_id=%d", event->msg_id);
        break;
    case MQTT_EVENT_PUBLISHED:
        ESP_LOGI(TAG, "MQTT_EVENT_PUBLISHED, msg_id=%d", event->msg_id);
        break;
    case MQTT_EVENT_DATA:
        ESP_LOGI(TAG, "MQTT_EVENT_DATA");
        printf("TOPIC=%.*s\r\n", event->topic_len, event->topic);
        printf("DATA=%.*s\r\n", event->data_len, event->data);
        break;
    case MQTT_EVENT_ERROR:
        ESP_LOGI(TAG, "MQTT_EVENT_ERROR");
        break;
    default:
        ESP_LOGI(TAG, "Other event id:%d", event->event_id);
        break;
    }
}

extern "C" void app_main(void)
{

        // Initialize ESP-IDF components
        ESP_ERROR_CHECK(nvs_flash_init());
        ESP_ERROR_CHECK(esp_event_loop_create_default());
#if !defined(CONFIG_IDF_TARGET_LINUX)
        ESP_ERROR_CHECK(esp_netif_init());
        ESP_ERROR_CHECK(example_connect());
#endif        

        // MQTT-over-QUIC (esp-mqtt + custom esp_transport): connect to broker.emqx.io:14567 using ALPN "mqtt"
        static constexpr const char* kHost = "broker.emqx.io";
        static constexpr uint16_t kPort = 14567;

        // Create custom QUIC-backed esp_transport and hand it to esp-mqtt
        esp_transport_handle_t quic_transport = esp_transport_quic_mqtt_init();
        if (!quic_transport) {
                ESP_LOGE(TAG, "failed to create QUIC transport");
                return;
        }

        esp_mqtt_client_config_t mqtt_config = {};
        mqtt_config.broker.address.hostname = kHost;
        mqtt_config.broker.address.port = kPort;
        mqtt_config.broker.address.transport = MQTT_TRANSPORT_OVER_TCP; // scheme selection only; actual IO comes from network.transport
        mqtt_config.credentials.client_id = "esp-quic";
        mqtt_config.session.protocol_ver = MQTT_PROTOCOL_V_3_1_1;
        mqtt_config.session.keepalive = 60;
        mqtt_config.network.timeout_ms = 10000;
        mqtt_config.network.transport = quic_transport;

        esp_mqtt_client_handle_t mqtt_client = esp_mqtt_client_init(&mqtt_config);
        if (!mqtt_client) {
                ESP_LOGE(TAG, "esp_mqtt_client_init failed");
                esp_transport_destroy(quic_transport);
                return;
        }
        ESP_ERROR_CHECK(esp_mqtt_client_register_event(mqtt_client, MQTT_EVENT_ANY, mqtt_event_handler, nullptr));
        ESP_ERROR_CHECK(esp_mqtt_client_start(mqtt_client));

        // Wait for CONNECT/CONNACK
        const uint64_t start_ms = esp_timer_get_time() / 1000;
        while (!s_connected && (esp_timer_get_time() / 1000) - start_ms < 8000) {
                vTaskDelay(pdMS_TO_TICKS(50));
        }
        if (!s_connected) {
                ESP_LOGW(TAG, "MQTT not connected yet (timeout waiting for CONNECT)");
        } else {
                int msg_id = esp_mqtt_client_publish(mqtt_client, "/pquic/test", "hello over quic", 0, 0, 0);
                ESP_LOGI(TAG, "published msg_id=%d", msg_id);
        }

        // Keep connection alive for a bit so we can see whether it stays up.
        // If the disconnect only happens after this delay, it was just app_main exiting/destructor cleanup.
        ESP_LOGI(TAG, "Holding connection open for 15 seconds...");
        const uint64_t hold_start_ms = esp_timer_get_time() / 1000;
        while ((esp_timer_get_time() / 1000) - hold_start_ms < 15000) {
                vTaskDelay(pdMS_TO_TICKS(100));
        }

        ESP_ERROR_CHECK(esp_mqtt_client_stop(mqtt_client));
        ESP_ERROR_CHECK(esp_mqtt_client_destroy(mqtt_client));

}
