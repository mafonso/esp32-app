#include <string.h>
#include <strings.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "esp_log.h"
#include "psa/crypto.h"
#include "mbedtls/base64.h"
#include "settings.h"
#include "auth.h"

static const char *TAG = "auth";

// Brute-force damping: every bad attempt costs a short delay, and a run of
// them locks the endpoint for a while. The HTTP server is single-threaded, so
// the delay is kept short.
#define FAIL_DELAY_MS 400
#define FAIL_LOCK_AFTER 5
#define FAIL_LOCK_S 30

static int s_cached = -1;  // -1 unknown, 0 off, 1 on
static uint8_t s_salt[SETTINGS_SALT_LEN];
static uint8_t s_hash[SETTINGS_HASH_LEN];
static int s_failures;
static int64_t s_locked_until_us;

static void load(void)
{
    if (s_cached < 0) {
        s_cached = settings_get_admin_hash(s_salt, s_hash) == ESP_OK;
    }
}

static esp_err_t hash_pw(const uint8_t salt[SETTINGS_SALT_LEN], const char *pw, uint8_t out[SETTINGS_HASH_LEN])
{
    uint8_t in[SETTINGS_SALT_LEN + AUTH_PASS_MAX];
    size_t len = strlen(pw);
    if (len > AUTH_PASS_MAX) {
        return ESP_ERR_INVALID_SIZE;
    }
    memcpy(in, salt, SETTINGS_SALT_LEN);
    memcpy(in + SETTINGS_SALT_LEN, pw, len);
    size_t olen = 0;
    psa_status_t st = psa_crypto_init();  // already done at boot; cheap no-op
    if (st == PSA_SUCCESS) {
        st = psa_hash_compute(PSA_ALG_SHA_256, in, SETTINGS_SALT_LEN + len, out, SETTINGS_HASH_LEN, &olen);
    }
    memset(in, 0, sizeof(in));
    return st == PSA_SUCCESS && olen == SETTINGS_HASH_LEN ? ESP_OK : ESP_FAIL;
}

bool auth_enabled(void)
{
    load();
    return s_cached == 1;
}

esp_err_t auth_set_password(const char *pw)
{
    esp_err_t err;
    if (!pw[0]) {
        err = settings_set_admin_hash(NULL, NULL);
        if (err == ESP_OK) {
            s_cached = 0;
            ESP_LOGI(TAG, "admin password removed");
        }
        return err;
    }
    uint8_t salt[SETTINGS_SALT_LEN], hash[SETTINGS_HASH_LEN];
    esp_fill_random(salt, sizeof(salt));
    err = hash_pw(salt, pw, hash);
    if (err == ESP_OK) {
        err = settings_set_admin_hash(salt, hash);
    }
    if (err == ESP_OK) {
        memcpy(s_salt, salt, sizeof(s_salt));
        memcpy(s_hash, hash, sizeof(s_hash));
        s_cached = 1;
        s_failures = 0;
        ESP_LOGI(TAG, "admin password set");
    }
    return err;
}

bool auth_check(const char *user, const char *pw)
{
    load();
    if (s_cached != 1) {
        return false;
    }
    uint8_t hash[SETTINGS_HASH_LEN];
    if (hash_pw(s_salt, pw, hash) != ESP_OK) {
        return false;
    }
    uint8_t diff = strcmp(user, AUTH_USER) != 0;
    for (int i = 0; i < SETTINGS_HASH_LEN; i++) {
        diff |= hash[i] ^ s_hash[i];
    }
    return diff == 0;
}

// Parse "Basic base64(user:pass)" and check it. False if missing or wrong;
// *attempted tells a wrong password apart from no credentials at all.
static bool check_header(httpd_req_t *req, bool *attempted)
{
    *attempted = false;
    char hdr[160];  // "Basic " + base64 of "admin:" + a 64-char password, with room to spare
    size_t hlen = httpd_req_get_hdr_value_len(req, "Authorization");
    if (hlen == 0) {
        return false;
    }
    *attempted = true;
    if (hlen >= sizeof(hdr) || httpd_req_get_hdr_value_str(req, "Authorization", hdr, sizeof(hdr)) != ESP_OK ||
        strncasecmp(hdr, "Basic ", 6) != 0) {
        return false;
    }
    char cred[AUTH_PASS_MAX + sizeof(AUTH_USER) + 2];
    size_t clen = 0;
    const char *b64 = hdr + 6;
    if (mbedtls_base64_decode((unsigned char *)cred, sizeof(cred) - 1, &clen,
                              (const unsigned char *)b64, strlen(b64)) != 0) {
        return false;
    }
    cred[clen] = '\0';
    char *colon = strchr(cred, ':');
    bool ok = false;
    if (colon) {
        *colon = '\0';
        ok = auth_check(cred, colon + 1);
    }
    memset(cred, 0, sizeof(cred));
    return ok;
}

bool web_require_admin(httpd_req_t *req)
{
    if (!auth_enabled()) {
        return true;
    }
    int64_t now = esp_timer_get_time();
    if (now < s_locked_until_us) {
        httpd_resp_send_custom_err(req, "429 Too Many Requests", "too many failed logins; try again shortly");
        return false;
    }
    bool attempted;
    if (check_header(req, &attempted)) {
        s_failures = 0;
        return true;
    }
    if (attempted) {
        if (++s_failures >= FAIL_LOCK_AFTER) {
            s_failures = 0;
            s_locked_until_us = now + FAIL_LOCK_S * 1000000LL;
            ESP_LOGW(TAG, "%d failed logins, locked for %d s", FAIL_LOCK_AFTER, FAIL_LOCK_S);
        }
        vTaskDelay(pdMS_TO_TICKS(FAIL_DELAY_MS));
    }
    httpd_resp_set_hdr(req, "WWW-Authenticate", "Basic realm=\"esp32\", charset=\"UTF-8\"");
    httpd_resp_send_err(req, HTTPD_401_UNAUTHORIZED, "admin password required (user: admin)");
    return false;
}
