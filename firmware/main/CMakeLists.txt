idf_component_register(SRCS "main.c" "api.c"
                       INCLUDE_DIRS "."
                       REQUIRES board display audio gesture net esp_http_server app_update json
                                esp_timer nvs_flash esp_psram)
