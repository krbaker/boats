// Wi-Fi co-processor (ESP32-C6) firmware version and update.

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// The co-processor's firmware version as "major.minor.patch" ("unknown" if it
// can't be read).
bool cp_update_version(char *out, size_t len);

// Streaming update from an uploaded ESP32-C6 firmware image (.bin). On success
// the display restarts shortly after finish() returns, to switch the C6 over.
typedef struct cp_update cp_update_t;
cp_update_t *cp_update_begin(void);
bool cp_update_feed(cp_update_t *u, const uint8_t *data, size_t len);
// Frees u. On failure err says why.
bool cp_update_finish(cp_update_t *u, char *err, size_t errlen);
