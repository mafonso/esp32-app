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

// Sessions for the page: it trades the password for a random token once and then sends
// the token. A request carrying "Authorization: Basic admin:..." every second (the log
// viewer) matches the "ET SCAN Tomcat Auth Brute Force" IPS rule, and routers that run
// it (UniFi, pfSense, OPNsense) then block the client. Kept in RAM, so a reboot or a
// password change signs everyone out.
#define TOKEN_SLOTS 4
static struct {
    uint8_t tok[AUTH_TOKEN_BYTES];
    int64_t used_us;  // 0 = free
} s_tokens[TOKEN_SLOTS];

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
    memset(s_tokens, 0, sizeof(s_tokens));
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

static void to_hex(const uint8_t *b, size_t n, char *out)
{
    static const char hex[] = "0123456789abcdef";
    for (size_t i = 0; i < n; i++) {
        out[2 * i] = hex[b[i] >> 4];
        out[2 * i + 1] = hex[b[i] & 15];
    }
    out[2 * n] = '\0';
}

esp_err_t auth_new_token(char out[AUTH_TOKEN_HEX_LEN + 1])
{
    if (!auth_enabled()) {
        return ESP_ERR_INVALID_STATE;
    }
    int slot = 0;  // a free slot, else the least recently used one
    for (int i = 1; i < TOKEN_SLOTS; i++) {
        if (s_tokens[i].used_us < s_tokens[slot].used_us) {
            slot = i;
        }
    }
    esp_fill_random(s_tokens[slot].tok, AUTH_TOKEN_BYTES);
    s_tokens[slot].used_us = esp_timer_get_time();
    to_hex(s_tokens[slot].tok, AUTH_TOKEN_BYTES, out);
    return ESP_OK;
}

// Constant-time match of the X-Admin-Token header against the live tokens.
static bool check_token(httpd_req_t *req)
{
    char hdr[AUTH_TOKEN_HEX_LEN + 2];
    size_t hlen = httpd_req_get_hdr_value_len(req, "X-Admin-Token");
    if (hlen == 0) {
        return false;
    }
    if (hlen != AUTH_TOKEN_HEX_LEN || httpd_req_get_hdr_value_str(req, "X-Admin-Token", hdr, sizeof(hdr)) != ESP_OK) {
        return false;
    }
    bool ok = false;
    for (int i = 0; i < TOKEN_SLOTS; i++) {
        if (!s_tokens[i].used_us) {
            continue;
        }
        char want[AUTH_TOKEN_HEX_LEN + 1];
        to_hex(s_tokens[i].tok, AUTH_TOKEN_BYTES, want);
        uint8_t diff = 0;
        for (int j = 0; j < AUTH_TOKEN_HEX_LEN; j++) {
            diff |= want[j] ^ hdr[j];
        }
        if (diff == 0) {
            s_tokens[i].used_us = esp_timer_get_time();
            ok = true;
        }
    }
    return ok;
}

// Counts a failed attempt toward the lockout and slows the caller down.
static void note_failure(int64_t now)
{
    if (++s_failures >= FAIL_LOCK_AFTER) {
        s_failures = 0;
        s_locked_until_us = now + FAIL_LOCK_S * 1000000LL;
        ESP_LOGW(TAG, "%d failed logins, locked for %d s", FAIL_LOCK_AFTER, FAIL_LOCK_S);
    }
    vTaskDelay(pdMS_TO_TICKS(FAIL_DELAY_MS));
}

static bool locked_out(httpd_req_t *req, int64_t now)
{
    if (now < s_locked_until_us) {
        httpd_resp_send_custom_err(req, "429 Too Many Requests", "too many failed logins; try again shortly");
        return true;
    }
    return false;
}

auth_login_t auth_login(httpd_req_t *req, const char *pw)
{
    int64_t now = esp_timer_get_time();
    if (locked_out(req, now)) {
        return AUTH_LOGIN_REPLIED;
    }
    if (auth_check(AUTH_USER, pw)) {
        s_failures = 0;
        return AUTH_LOGIN_OK;
    }
    note_failure(now);
    return AUTH_LOGIN_WRONG;
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
    if (locked_out(req, now)) {
        return false;
    }
    bool attempted;
    if (check_token(req)) {
        return true;
    }
    if (check_header(req, &attempted)) {
        s_failures = 0;
        return true;
    }
    // A stale token (after a reboot) is not a guess: only wrong passwords count.
    if (attempted) {
        note_failure(now);
    }
    // Only challenge direct visits. The page's fetch() calls (X-Requested-With) ask for the
    // password themselves: browsers handle a challenge on fetch() inconsistently, and iOS
    // WebKit never retries a POST with saved credentials, so Save hangs.
    if (httpd_req_get_hdr_value_len(req, "X-Requested-With") == 0) {
        httpd_resp_set_hdr(req, "WWW-Authenticate", "Basic realm=\"esp32\", charset=\"UTF-8\"");
    }
    httpd_resp_send_err(req, HTTPD_401_UNAUTHORIZED, "admin password required (user: admin)");
    return false;
}
