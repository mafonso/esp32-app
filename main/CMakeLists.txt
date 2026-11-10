# Sets WEB_ASSET_SRC and WEB_ASSET_FILES from the files in main/web/.
if(NOT CMAKE_BUILD_EARLY_EXPANSION)
    include("${CMAKE_CURRENT_LIST_DIR}/web_assets.cmake")
endif()

# Any .c file in these folders is built; no list to update.
idf_component_register(SRC_DIRS "." "core" "features"
                       INCLUDE_DIRS "." "core" "features"
                       EMBED_FILES ${WEB_ASSET_FILES}
                       REQUIRES esp_wifi esp_netif esp_event nvs_flash esp_http_server esp_http_client
                                esp_https_ota app_update esp_partition esp_psram spi_flash esp_timer
                                esp_driver_tsens esp_driver_gpio console mbedtls esp_app_format)

if(NOT CMAKE_BUILD_EARLY_EXPANSION)
    # IDF ignores SRC_DIRS when SRCS is also given, so add the generated table here.
    target_sources(${COMPONENT_LIB} PRIVATE "${WEB_ASSET_SRC}")
endif()
