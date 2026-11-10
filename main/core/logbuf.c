#include <stdio.h>
#include <string.h>
#include <stdarg.h>
#include "freertos/FreeRTOS.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "logbuf.h"

// Nothing in this file may log: the hook would recurse into itself.

#define LOGBUF_SIZE 16384

// Records are [uint16 len][text], packed back to back and wrapping around.
static uint8_t *s_buf;
static size_t s_head;             // next write offset
static size_t s_tail;             // oldest record
static size_t s_used;
static uint32_t s_first_seq = 1;  // sequence number of the record at s_tail
static uint32_t s_next_seq = 1;
// A line can arrive in several vprintf calls; collect it here until '\n'.
static char s_partial[LOGBUF_LINE_MAX];
static size_t s_partial_len;
static portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;
static vprintf_like_t s_prev;

static void ring_put(const void *src, size_t n)
{
    size_t first = n < LOGBUF_SIZE - s_head ? n : LOGBUF_SIZE - s_head;
    memcpy(s_buf + s_head, src, first);
    memcpy(s_buf, (const uint8_t *)src + first, n - first);
    s_head = (s_head + n) % LOGBUF_SIZE;
}

static void ring_get(size_t off, void *dst, size_t n)
{
    size_t first = n < LOGBUF_SIZE - off ? n : LOGBUF_SIZE - off;
    memcpy(dst, s_buf + off, first);
    memcpy((uint8_t *)dst + first, s_buf, n - first);
}

static uint16_t record_len(size_t off)
{
    uint16_t len;
    ring_get(off, &len, sizeof(len));
    return len;
}

// Caller holds s_mux.
static void commit_partial(void)
{
    size_t need = sizeof(uint16_t) + s_partial_len;
    while (LOGBUF_SIZE - s_used < need) {
        size_t sz = sizeof(uint16_t) + record_len(s_tail);
        s_tail = (s_tail + sz) % LOGBUF_SIZE;
        s_used -= sz;
        s_first_seq++;
    }
    uint16_t len = s_partial_len;
    ring_put(&len, sizeof(len));
    ring_put(s_partial, s_partial_len);
    s_used += need;
    s_next_seq++;
    s_partial_len = 0;
}

// Remove ANSI colour sequences (ESC [ ... letter) and carriage returns in place.
static size_t strip_ansi(char *s, size_t len)
{
    size_t out = 0;
    for (size_t i = 0; i < len; i++) {
        if (s[i] == '\x1b' && i + 1 < len && s[i + 1] == '[') {
            i += 2;
            while (i < len && !((s[i] >= 'A' && s[i] <= 'Z') || (s[i] >= 'a' && s[i] <= 'z'))) {
                i++;
            }
            continue;
        }
        if (s[i] != '\r') {
            s[out++] = s[i];
        }
    }
    return out;
}

// Runs in whatever task logged. Never blocks, allocates or logs.
static int log_hook(const char *fmt, va_list args)
{
    va_list copy;
    va_copy(copy, args);
    int ret = s_prev(fmt, args);

    char line[LOGBUF_LINE_MAX + 1];
    int n = vsnprintf(line, sizeof(line), fmt, copy);
    va_end(copy);
    if (n <= 0) {
        return ret;
    }
    bool truncated = n >= (int)sizeof(line);
    size_t len = strip_ansi(line, truncated ? sizeof(line) - 1 : (size_t)n);

    portENTER_CRITICAL_SAFE(&s_mux);
    for (size_t i = 0; i < len; i++) {
        if (line[i] == '\n') {
            commit_partial();
        } else if (s_partial_len < sizeof(s_partial)) {
            s_partial[s_partial_len++] = line[i];
        }
    }
    // The newline was cut off; don't glue the next line onto this one.
    if (truncated && s_partial_len) {
        commit_partial();
    }
    portEXIT_CRITICAL_SAFE(&s_mux);
    return ret;
}

esp_err_t logbuf_init(void)
{
    if (s_buf) {
        return ESP_OK;
    }
    s_buf = heap_caps_malloc(LOGBUF_SIZE, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!s_buf) {
        s_buf = heap_caps_malloc(LOGBUF_SIZE, MALLOC_CAP_8BIT);
    }
    if (!s_buf) {
        return ESP_ERR_NO_MEM;
    }
    s_prev = esp_log_set_vprintf(log_hook);
    if (!s_prev) {
        s_prev = vprintf;
    }
    return ESP_OK;
}

void logbuf_read(uint32_t after, size_t max_bytes, uint32_t *last, bool *dropped,
                 void (*emit)(const char *line, size_t len, void *ctx), void *ctx)
{
    *dropped = false;
    if (!s_buf) {
        *last = 0;
        return;
    }
    char line[LOGBUF_LINE_MAX];
    uint32_t seq;
    size_t off;

    portENTER_CRITICAL(&s_mux);
    if (after >= s_next_seq) {
        after = 0;  // the board restarted since the client last asked
    }
    *dropped = after + 1 < s_first_seq && after != 0;
    seq = s_first_seq;
    off = s_tail;
    while (seq <= after) {
        off = (off + sizeof(uint16_t) + record_len(off)) % LOGBUF_SIZE;
        seq++;
    }
    portEXIT_CRITICAL(&s_mux);

    // One record per lock, so writers never wait behind a long copy.
    size_t total = 0;
    while (total < max_bytes) {
        portENTER_CRITICAL(&s_mux);
        if (seq < s_first_seq) {
            // Overwritten while we were sending; skip ahead.
            seq = s_first_seq;
            off = s_tail;
            *dropped = true;
        }
        if (seq >= s_next_seq) {
            portEXIT_CRITICAL(&s_mux);
            break;
        }
        size_t len = record_len(off);
        ring_get((off + sizeof(uint16_t)) % LOGBUF_SIZE, line, len);
        off = (off + sizeof(uint16_t) + len) % LOGBUF_SIZE;
        seq++;
        portEXIT_CRITICAL(&s_mux);

        emit(line, len, ctx);
        total += len;
    }
    *last = seq - 1;
}
