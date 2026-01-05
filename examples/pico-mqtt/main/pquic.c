/*
* Author: Christian Huitema
* Copyright (c) 2020, Private Octopus, Inc.
* All rights reserved.
*
* Permission to use, copy, modify, and distribute this software for any
* purpose with or without fee is hereby granted, provided that the above
* copyright notice and this permission notice appear in all copies.
*
* THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND
* ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
* WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
* DISCLAIMED. IN NO EVENT SHALL Private Octopus, Inc. BE LIABLE FOR ANY
* DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
* (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
* LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND
* ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
* (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
* SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
*/

#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <inttypes.h>

#include "nvs_flash.h"
#include "esp_event.h"
#include "protocol_examples_common.h"
#include "esp_timer.h"
#include "esp_netif.h"
#include "esp_log.h"
#include "mqtt_client.h"
#include "mqtt_picoquic_transport.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char* TAG = "pquic";
#define MQTT_QUIC_HOST "broker.emqx.io"
#define MQTT_QUIC_PORT 14567
static volatile bool s_connected = false;

static void mqtt_event_handler(void* handler_args, esp_event_base_t base, int32_t event_id, void* event_data)
{
    (void)handler_args;
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
        break;
    case MQTT_EVENT_DISCONNECTED:
        ESP_LOGI(TAG, "MQTT_EVENT_DISCONNECTED");
        s_connected = false;
        break;
    case MQTT_EVENT_SUBSCRIBED:
        ESP_LOGI(TAG, "MQTT_EVENT_SUBSCRIBED, msg_id=%d", event->msg_id);
        msg_id = esp_mqtt_client_publish(client, "topic/qos0", "hello over picoquic", 0, 0, 0);
        ESP_LOGI(TAG, "sent publish successful, msg_id=%d", msg_id);
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

void app_main(void)
{

    ESP_LOGI(TAG, "pico-mqtt (esp-mqtt + picoquic transport)");

    ESP_ERROR_CHECK(nvs_flash_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
#if !defined(CONFIG_IDF_TARGET_LINUX)
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(example_connect());
#endif

    esp_transport_handle_t tp = esp_transport_picoquic_mqtt_init();
    if (!tp) {
        ESP_LOGE(TAG, "failed to create picoquic transport");
        return;
    }

    esp_mqtt_client_config_t mqtt_config = { 0 };
    mqtt_config.broker.address.hostname = MQTT_QUIC_HOST;
    mqtt_config.broker.address.port = MQTT_QUIC_PORT;
    mqtt_config.broker.address.transport = MQTT_TRANSPORT_OVER_TCP; // scheme only
    mqtt_config.credentials.client_id = "esp-picoquic";
    mqtt_config.session.protocol_ver = MQTT_PROTOCOL_V_3_1_1;
    mqtt_config.session.keepalive = 60;
    mqtt_config.network.timeout_ms = 10000;
    mqtt_config.network.transport = tp;

    esp_mqtt_client_handle_t client = esp_mqtt_client_init(&mqtt_config);
    if (!client) {
        ESP_LOGE(TAG, "esp_mqtt_client_init failed");
        esp_transport_destroy(tp);
        return;
    }

    ESP_ERROR_CHECK(esp_mqtt_client_register_event(client, MQTT_EVENT_ANY, mqtt_event_handler, NULL));
    ESP_ERROR_CHECK(esp_mqtt_client_start(client));

    uint64_t start_ms = esp_timer_get_time() / 1000;
    while (!s_connected && (esp_timer_get_time() / 1000) - start_ms < 8000) {
        vTaskDelay(pdMS_TO_TICKS(50));
    }

    ESP_LOGI(TAG, "Holding connection open for 15 seconds...");
    vTaskDelay(pdMS_TO_TICKS(15000));

    ESP_ERROR_CHECK(esp_mqtt_client_stop(client));
    ESP_ERROR_CHECK(esp_mqtt_client_destroy(client));
}

#ifdef CONFIG_IDF_TARGET_LINUX
int main(void)
{
    app_main();
    return 0;
}
#endif
