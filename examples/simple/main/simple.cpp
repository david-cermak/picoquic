#include <stdio.h>
#include "client/http3_client.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "esp_event.h"
#include "esp_log.h"
#include "protocol_examples_common.h"

#define TAG "simple"

extern "C" void app_main(void)
{

        // Initialize ESP-IDF components
        ESP_ERROR_CHECK(nvs_flash_init());
        ESP_ERROR_CHECK(esp_netif_init());
        ESP_ERROR_CHECK(esp_event_loop_create_default());
        ESP_ERROR_CHECK(example_connect());
// Configure connection
Http3ClientConfig config;
config.hostname = "quic.nginx.org";
config.port = 443;
config.receive_buffer_size = 4 * 1024;

// Create client (manages connection lifecycle)
Http3Client client(config);

// Simple GET request
Http3Response response;
if (client.Get("/", response)) {
    ESP_LOGI(TAG, "Status: %d", response.status);
    ESP_LOGI(TAG, "Body: %s", response.body.c_str());
}

}
