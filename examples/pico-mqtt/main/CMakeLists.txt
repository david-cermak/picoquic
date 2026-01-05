idf_component_register(SRCS "pquic.c" "mqtt_picoquic_transport.cpp"
                    REQUIRES picoquic mqtt nvs_flash esp_event protocol_examples_common esp_netif)
#  esp_event protocol_examples_common)
