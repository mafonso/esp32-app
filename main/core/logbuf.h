#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

#define LOGBUF_LINE_MAX 200

// Hook esp_log so every line also lands in a 16 KB PSRAM ring buffer (USB
// output is unchanged). Call first thing in app_main to keep the boot log.
esp_err_t logbuf_init(void);

// Call emit() for each buffered line with sequence number > after, oldest
// first, until about max_bytes of text. Lines are numbered from 1; an `after`
// beyond the newest line (e.g. from before a reboot) starts from the oldest.
// *last is set to the sequence number of the last line passed (or the newest
// line if none), *dropped to true if lines after `after` were overwritten.
// emit() runs outside the lock and may log.
void logbuf_read(uint32_t after, size_t max_bytes, uint32_t *last, bool *dropped,
                 void (*emit)(const char *line, size_t len, void *ctx), void *ctx);
