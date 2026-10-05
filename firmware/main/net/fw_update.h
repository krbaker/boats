// Display firmware: version, and updates from an uploaded image into the
// other app slot. A new image boots on trial; fw_update_confirm_later() marks
// it good once it has run for a while, or the bootloader rolls back.

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct {
    char version[32];
    char build_date[32];        // "<date> <time>" the image was built
    char running[17];           // app slot label, e.g. "ota_0"
    bool pending_verify;        // running a new image on trial
    bool rolled_back;           // the last update failed and the previous image is running
    bool can_update;            // there's a slot to update into (false on the old single-app table)
} fw_info_t;

void fw_update_info(fw_info_t *out);

// Call once at startup: marks a trial image good after it has run for a while.
void fw_update_confirm_later(void);

typedef struct fw_update fw_update_t;
// NULL with err set if no update can start.
fw_update_t *fw_update_begin(size_t image_size, char *err, size_t errlen);
bool fw_update_feed(fw_update_t *u, const uint8_t *data, size_t len);
// Frees u. On success the display restarts shortly after; on failure err says why.
bool fw_update_finish(fw_update_t *u, char *err, size_t errlen);
