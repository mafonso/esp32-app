#include <string.h>
#include "nvs_flash.h"
#include "nvs.h"
#include "esp_log.h"
#include "settings.h"

static const char *TAG = "settings";
static const char *NS = "app";

esp_err_t settings_init(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    if (err != ESP_OK) {
        return err;
    }

    char ssid[SETTINGS_SSID_MAX], pass[SETTINGS_PASS_MAX];
    if (settings_get_wifi(ssid, pass) != ESP_OK && strlen(CONFIG_APP_WIFI_PASSWORD) > 0) {
        ESP_LOGI(TAG, "seeding Wi-Fi credentials for '%s' from build config", CONFIG_APP_WIFI_SSID);
        return settings_set_wifi(CONFIG_APP_WIFI_SSID, CONFIG_APP_WIFI_PASSWORD);
    }
    return ESP_OK;
}

esp_err_t settings_get_wifi(char ssid[SETTINGS_SSID_MAX], char pass[SETTINGS_PASS_MAX])
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(NS, NVS_READONLY, &h);
    if (err != ESP_OK) {
        return err;
    }
    size_t len = SETTINGS_SSID_MAX;
    err = nvs_get_str(h, "ssid", ssid, &len);
    if (err == ESP_OK) {
        len = SETTINGS_PASS_MAX;
        if (nvs_get_str(h, "pass", pass, &len) != ESP_OK) {
            pass[0] = '\0';
        }
    }
    nvs_close(h);
    return err;
}

esp_err_t settings_set_wifi(const char *ssid, const char *pass)
{
    if (strlen(ssid) >= SETTINGS_SSID_MAX || strlen(pass) >= SETTINGS_PASS_MAX) {
        return ESP_ERR_INVALID_SIZE;
    }
    nvs_handle_t h;
    esp_err_t err = nvs_open(NS, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        return err;
    }
    err = nvs_set_str(h, "ssid", ssid);
    if (err == ESP_OK) {
        err = nvs_set_str(h, "pass", pass);
    }
    if (err == ESP_OK) {
        err = nvs_commit(h);
    }
    nvs_close(h);
    return err;
}

esp_err_t settings_get_name(char name[SETTINGS_NAME_MAX])
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(NS, NVS_READONLY, &h);
    if (err != ESP_OK) {
        return err;
    }
    size_t len = SETTINGS_NAME_MAX;
    err = nvs_get_str(h, "name", name, &len);
    nvs_close(h);
    return err;
}

esp_err_t settings_set_name(const char *name)
{
    if (strlen(name) >= SETTINGS_NAME_MAX) {
        return ESP_ERR_INVALID_SIZE;
    }
    nvs_handle_t h;
    esp_err_t err = nvs_open(NS, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        return err;
    }
    // An empty name clears the override and restores the MAC-based default.
    err = name[0] ? nvs_set_str(h, "name", name) : nvs_erase_key(h, "name");
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        err = ESP_OK;
    }
    if (err == ESP_OK) {
        err = nvs_commit(h);
    }
    nvs_close(h);
    return err;
}
