#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "esp_console.h"
#include "esp_wifi.h"
#include "esp_app_desc.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "cJSON.h"
#include "settings.h"
#include "wifi.h"
#include "auth.h"
#include "ota.h"
#include "web.h"
#include "console_cmds.h"
#include "features.h"

static void print_menu(void)
{
    printf("\n"
           "=============== %s %s ===============\n"
           "  status                 board, firmware and network info\n"
           "  scan                   list nearby Wi-Fi networks\n"
           "  wifi <ssid> [password] save Wi-Fi credentials and reconnect\n"
           "  name [hostname]        set the DHCP/mDNS name ('name -' resets it)\n"
           "  passwd <password>      set the web admin password ('passwd -' removes it)\n"
           "  ota check              check GitHub for a newer release\n"
           "  ota update             install the release found by 'ota check'\n"
           "  reboot                 restart the board\n"
           "  help                   full command list, including feature commands\n"
           "\n",
           esp_app_get_description()->project_name, esp_app_get_description()->version);
}

static int cmd_menu(int argc, char **argv)
{
    print_menu();
    return 0;
}

static int cmd_status(int argc, char **argv)
{
    cJSON *info = web_info_json();
    char *s = cJSON_Print(info);
    printf("%s\n", s);
    free(s);
    cJSON_Delete(info);
    return 0;
}

static int cmd_scan(int argc, char **argv)
{
    if (esp_wifi_scan_start(NULL, true) != ESP_OK) {
        printf("scan failed\n");
        return 1;
    }
    uint16_t n = 20;
    wifi_ap_record_t aps[20];
    esp_wifi_scan_get_ap_records(&n, aps);
    for (int i = 0; i < n; i++) {
        printf("  %-32s ch%-2d %4d dBm %s\n", (char *)aps[i].ssid, aps[i].primary, aps[i].rssi,
               aps[i].authmode == WIFI_AUTH_OPEN ? "open" : "");
    }
    return 0;
}

static int cmd_wifi(int argc, char **argv)
{
    if (argc < 2) {
        char ssid[SETTINGS_SSID_MAX], pass[SETTINGS_PASS_MAX];
        if (settings_get_wifi(ssid, pass) == ESP_OK) {
            printf("configured network: '%s'\n", ssid);
        } else {
            printf("no network configured\n");
        }
        printf("usage: wifi <ssid> [password]\n");
        return 0;
    }
    esp_err_t err = settings_set_wifi(argv[1], argc > 2 ? argv[2] : "");
    if (err != ESP_OK) {
        printf("could not save: %s\n", esp_err_to_name(err));
        return 1;
    }
    printf("saved, connecting to '%s'...\n", argv[1]);
    wifi_reconnect();
    if (wifi_wait_connected(15000)) {
        wifi_status_t ws;
        wifi_get_status(&ws);
        printf("connected: http://%s/  (http://%s.local/)\n", ws.ip, wifi_hostname());
    } else {
        printf("not connected yet; check the password with 'scan' and try again\n");
    }
    return 0;
}

static int cmd_name(int argc, char **argv)
{
    if (argc < 2) {
        printf("hostname: %s\nusage: name <hostname> | name -\n", wifi_hostname());
        return 0;
    }
    const char *name = strcmp(argv[1], "-") == 0 ? "" : argv[1];
    if (name[0] && !settings_valid_hostname(name)) {
        printf("use letters, digits and '-', up to %d characters\n", SETTINGS_NAME_MAX - 1);
        return 1;
    }
    esp_err_t err = settings_set_name(name);
    if (err != ESP_OK) {
        printf("could not save: %s\n", esp_err_to_name(err));
        return 1;
    }
    printf("saved, rebooting to apply\n");
    vTaskDelay(pdMS_TO_TICKS(200));
    esp_restart();
    return 0;
}

static int cmd_passwd(int argc, char **argv)
{
    if (argc < 2) {
        printf("admin password: %s\nusage: passwd <password> | passwd -\n", auth_enabled() ? "set" : "not set");
        return 0;
    }
    const char *pw = strcmp(argv[1], "-") == 0 ? "" : argv[1];
    if (pw[0] && (strlen(pw) < 4 || strlen(pw) > AUTH_PASS_MAX)) {
        printf("use 4-%d characters\n", AUTH_PASS_MAX);
        return 1;
    }
    esp_err_t err = auth_set_password(pw);
    if (err != ESP_OK) {
        printf("could not save: %s\n", esp_err_to_name(err));
        return 1;
    }
    printf(pw[0] ? "saved; the web UI now asks for user 'admin' and this password\n"
                 : "removed; the web UI is open again\n");
    return 0;
}

static int cmd_ota(int argc, char **argv)
{
    if (argc == 2 && strcmp(argv[1], "check") == 0) {
        if (ota_check() != ESP_OK) {
            ota_status_t st;
            ota_get_status(&st);
            printf("check failed: %s\n", st.error);
            return 1;
        }
        ota_status_t st;
        ota_get_status(&st);
        printf("running %s, latest %s: %s\n", esp_app_get_description()->version, st.latest_version,
               st.update_available ? "update available, run 'ota update'" : "up to date");
        return 0;
    }
    if (argc == 2 && strcmp(argv[1], "update") == 0) {
        if (ota_start_update() != ESP_OK) {
            printf("run 'ota check' first\n");
            return 1;
        }
        ota_status_t st;
        do {
            vTaskDelay(pdMS_TO_TICKS(1000));
            ota_get_status(&st);
            if (st.total_bytes > 0) {
                printf("  %d / %d bytes\n", st.bytes_read, st.total_bytes);
            }
        } while (st.state == OTA_DOWNLOADING);
        if (st.state == OTA_ERROR) {
            printf("update failed: %s\n", st.error);
            return 1;
        }
        printf("installed, rebooting\n");
        return 0;
    }
    printf("usage: ota check | ota update\n");
    return 1;
}

static int cmd_reboot(int argc, char **argv)
{
    esp_restart();
    return 0;
}

esp_err_t console_start(void)
{
    esp_console_repl_t *repl = NULL;
    esp_console_repl_config_t cfg = ESP_CONSOLE_REPL_CONFIG_DEFAULT();
    cfg.prompt = "esp32> ";
    cfg.task_stack_size = 8192;
    esp_err_t err = esp_console_new_repl_stdio(&cfg, &repl);
    if (err != ESP_OK) {
        return err;
    }

    static const esp_console_cmd_t cmds[] = {
        {.command = "menu", .help = "Show the setup menu", .func = cmd_menu},
        {.command = "status", .help = "Board, firmware and network info", .func = cmd_status},
        {.command = "scan", .help = "List nearby Wi-Fi networks", .func = cmd_scan},
        {.command = "wifi", .help = "Save Wi-Fi credentials and reconnect", .hint = "<ssid> [password]", .func = cmd_wifi},
        {.command = "name", .help = "Set the DHCP/mDNS hostname ('-' resets it)", .hint = "[hostname]", .func = cmd_name},
        {.command = "passwd", .help = "Set the web admin password ('-' removes it)", .hint = "<password>", .func = cmd_passwd},
        {.command = "ota", .help = "Check for or install a GitHub release", .hint = "check|update", .func = cmd_ota},
        {.command = "reboot", .help = "Restart the board", .func = cmd_reboot},
    };
    for (int i = 0; i < sizeof(cmds) / sizeof(cmds[0]); i++) {
        esp_console_cmd_register(&cmds[i]);
    }
    features_register_console();
    esp_console_register_help_command();

    print_menu();
    return esp_console_start_repl(repl);
}
