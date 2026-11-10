idf_component_register(SRCS "app_main.c" "settings.c" "wifi.c" "ota.c" "web.c" "console_cmds.c"
                       INCLUDE_DIRS "."
                       EMBED_TXTFILES "index.html"
                       REQUIRES esp_wifi esp_netif esp_event nvs_flash esp_http_server esp_http_client
                                esp_https_ota app_update esp_partition esp_psram spi_flash esp_timer
                                esp_driver_tsens console mbedtls esp_app_format)
