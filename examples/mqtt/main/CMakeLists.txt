idf_component_register(SRCS "simple.cpp"
                    INCLUDE_DIRS "."
                    REQUIRES esp-http3 esp_event esp_timer protocol_examples_common nvs_flash)
