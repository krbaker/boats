// Country flags beside ships: 32x24 PNGs in flags/ on the microSD card (from
// mapbuilder/make_flags.py), each read and decoded the first time it's wanted
// and then kept. Without the card or the file there's no flag, and callers
// show the country as text. A flag found missing is looked for again once
// flags have been uploaded.

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_heap_caps.h"
#include "map_store.h"
#include "map_tiles.h"
#include "ui.h"

#define MAX_FLAGS   256         // more than there are countries
#define MAX_FILE    8192        // a 32x24 flag is well under 1 KB

typedef struct {
    char iso[3];
    bool ok;
    uint32_t version;       // map_store_flags_version() when it was looked for
    lv_image_dsc_t dsc;
} flag_t;

static flag_t *s_flags;
static int s_nflags;

const lv_image_dsc_t *ui_flag_get(uint16_t mid, int *reads)
{
    const char *iso = mid ? ais_mid_iso(mid) : NULL;
    if (!iso) return NULL;
    uint32_t version = map_store_flags_version();
    flag_t *f = NULL;
    for (int i = 0; i < s_nflags; i++) {
        if (strcmp(s_flags[i].iso, iso) == 0) {
            // A flag already read is kept (list rows may be showing it).
            if (s_flags[i].ok || s_flags[i].version == version) return s_flags[i].ok ? &s_flags[i].dsc : NULL;
            f = &s_flags[i];
            break;
        }
    }
    if (!reads || *reads <= 0 || !map_store_card_ok()) return NULL; // not remembered as missing
    (*reads)--;
    if (!f) {
        if (!s_flags) s_flags = heap_caps_calloc(MAX_FLAGS, sizeof(flag_t), MALLOC_CAP_SPIRAM);
        if (!s_flags || s_nflags == MAX_FLAGS) return NULL;
        f = &s_flags[s_nflags++];
        strcpy(f->iso, iso);
    }
    f->version = version;
    char path[48];
    snprintf(path, sizeof(path), FLAGS_DIR "/%s.png", iso);
    FILE *fp = fopen(path, "rb");
    if (!fp) return NULL;
    uint8_t *buf = heap_caps_malloc(MAX_FILE, MALLOC_CAP_SPIRAM);
    if (buf) {
        size_t n = fread(buf, 1, MAX_FILE, fp);
        if (n > 0 && n < MAX_FILE) f->ok = map_tile_decode(buf, n, &f->dsc);
        free(buf);
    }
    fclose(fp);
    return f->ok ? &f->dsc : NULL;
}

void ui_flag_country(uint32_t mmsi, char *buf, size_t len)
{
    uint16_t mid = ais_mmsi_mid(mmsi);
    if (mid) snprintf(buf, len, "%s (%u)", ais_mid_country(mid), mid);
    else snprintf(buf, len, "-");
}
