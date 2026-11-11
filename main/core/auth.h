#pragma once

#include <stdbool.h>
#include "esp_err.h"
#include "esp_http_server.h"

#define AUTH_USER "admin"
#define AUTH_PASS_MAX 64
#define AUTH_TOKEN_BYTES 16
#define AUTH_TOKEN_HEX_LEN (2 * AUTH_TOKEN_BYTES)

// Optional admin password, off until one is set. The page signs in once
// (POST /api/login) and sends the token it gets in X-Admin-Token; HTTP Basic
// (user "admin") also works, for curl and direct visits.
bool auth_enabled(void);

// Store a new password as a salted SHA-256; "" removes it. Signs out every token.
esp_err_t auth_set_password(const char *pw);

// True if user is "admin" and pw matches (constant-time compare).
bool auth_check(const char *user, const char *pw);

typedef enum {
    AUTH_LOGIN_OK,       // password right: issue a token
    AUTH_LOGIN_WRONG,    // password wrong: reply 401
    AUTH_LOGIN_REPLIED,  // locked out: a 429 was already sent
} auth_login_t;

// Check a password typed into the page, with the same brute-force damping as
// web_require_admin().
auth_login_t auth_login(httpd_req_t *req, const char *pw);

// A new session token as lowercase hex (replaces the least recently used one).
// ESP_ERR_INVALID_STATE if no password is set.
esp_err_t auth_new_token(char out[AUTH_TOKEN_HEX_LEN + 1]);

// For admin-only routes. True if no password is set or the request carries a
// valid X-Admin-Token or Basic credentials. Otherwise sends 401 with WWW-Authenticate (or 429
// while locked out after repeated failures) and returns false; the handler
// should then return ESP_OK.
bool web_require_admin(httpd_req_t *req);
