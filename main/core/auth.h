#pragma once

#include <stdbool.h>
#include "esp_err.h"
#include "esp_http_server.h"

#define AUTH_USER "admin"
#define AUTH_PASS_MAX 64

// Optional admin password (HTTP Basic, user "admin"). Off until one is set.
bool auth_enabled(void);

// Store a new password as a salted SHA-256; "" removes it.
esp_err_t auth_set_password(const char *pw);

// True if user is "admin" and pw matches (constant-time compare).
bool auth_check(const char *user, const char *pw);

// For admin-only routes. True if no password is set or the request carries
// valid Basic credentials. Otherwise sends 401 with WWW-Authenticate (or 429
// while locked out after repeated failures) and returns false; the handler
// should then return ESP_OK.
bool web_require_admin(httpd_req_t *req);
