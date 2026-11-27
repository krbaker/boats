// Wi-Fi co-processor (ESP32-C6, or C5 on the WIFI6-DB) firmware version and update.

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

// If the microSD card has coprocessor/build/eh_cp_ota_coprocessor_ota.bin on
// it, installs it on the C6 and restarts (renaming the file so it's installed
// once). Call before Wi-Fi starts: with no Wi-Fi traffic, the link only
// carries the update, which gets a C6 whose old firmware the link can't keep
// up with under load (BOARDS_PLAN.md) onto a matching version.
void cp_update_from_card(void);
