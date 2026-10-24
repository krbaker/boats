#include "log_save.h"

#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "esp_core_dump.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_partition.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "log_buf.h"
#include "tide.h"

static const char *TAG = "log_save";

#define DUMP_CHUNK 4096

static volatile log_save_state_t s_state;
static char s_msg[64];

bool log_save_valid_name(const char *name)
{
    size_t n = strlen(name);
    if (n < 9 || n >= sizeof(((log_file_t *)0)->name) || strncmp(name, "log_", 4) != 0) return false;
    if (strcmp(name + n - 4, ".txt") != 0 && strcmp(name + n - 4, ".bin") != 0) return false;
    for (size_t i = 0; i < n; i++) {
        char c = name[i];
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' || c == '-' ||
              (c == '.' && i == n - 4)))
            return false;
    }
    return true;
}

// path with ".bin" in place of ".txt"
static void dump_path(char *out, size_t len, const char *log_path)
{
    strlcpy(out, log_path, len);
    size_t n = strlen(out);
    if (n >= 4) strcpy(out + n - 4, ".bin");
}

static int by_newest(const void *a, const void *b)
{
    const log_file_t *x = a, *y = b;
    if (x->mtime != y->mtime) return x->mtime < y->mtime ? 1 : -1;
    return strcmp(y->name, x->name);
}

int log_save_list(log_file_t *out, int max)
{
    DIR *d = opendir(LOG_SAVE_DIR);
    if (!d) return 0;
    int n = 0;
    struct dirent *e;
    while (n < max && (e = readdir(d))) {
        size_t len = strlen(e->d_name);
        if (!log_save_valid_name(e->d_name) || strcmp(e->d_name + len - 4, ".txt") != 0) continue;
        log_file_t *f = &out[n];
        strlcpy(f->name, e->d_name, sizeof(f->name)); // fits: checked above
        char path[96], dump[96];
        struct stat st;
        snprintf(path, sizeof(path), LOG_SAVE_DIR "/%s", f->name);
        if (stat(path, &st) != 0) continue;
        n++;
        f->size = st.st_size;
        f->mtime = st.st_mtime < 365 * 86400 * 30 ? 0 : st.st_mtime; // FAT's 1980 = no clock
        dump_path(dump, sizeof(dump), path);
        f->dump_size = stat(dump, &st) == 0 ? st.st_size : 0;
    }
    closedir(d);
    qsort(out, n, sizeof(*out), by_newest);
    return n;
}

// Delete all but the newest LOG_SAVE_KEEP - 1, making room for one more.
static void prune(void)
{
    log_file_t *all = heap_caps_malloc(64 * sizeof(log_file_t), MALLOC_CAP_SPIRAM);
    if (!all) return;
    int n = log_save_list(all, 64);
    for (int i = LOG_SAVE_KEEP - 1; i < n; i++) {
        char path[96], dump[96];
        snprintf(path, sizeof(path), LOG_SAVE_DIR "/%s", all[i].name);
        dump_path(dump, sizeof(dump), path);
        unlink(path);
        unlink(dump);
    }
    free(all);
}

// The last crash's core dump, raw from flash, to path. False if there's none.
static bool save_dump(const char *path)
{
    size_t addr, size;
    const esp_partition_t *p =
        esp_partition_find_first(ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_COREDUMP, NULL);
    if (!p || esp_core_dump_image_get(&addr, &size) != ESP_OK) return false;
    FILE *f = fopen(path, "wb");
    char *buf = heap_caps_malloc(DUMP_CHUNK, MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA);
    bool ok = f && buf;
    for (size_t off = 0; ok && off < size; off += DUMP_CHUNK) {
        size_t n = size - off < DUMP_CHUNK ? size - off : DUMP_CHUNK;
        ok = esp_partition_read(p, addr - p->address + off, buf, n) == ESP_OK && fwrite(buf, 1, n, f) == n;
    }
    free(buf);
    if (f && fclose(f) != 0) ok = false;
    if (!ok) {
        ESP_LOGW(TAG, "core dump not saved");
        unlink(path);
    }
    return ok;
}

static bool fail(const char *why)
{
    ESP_LOGW(TAG, "%s", why);
    strlcpy(s_msg, why, sizeof(s_msg));
    return false;
}

static bool save(void)
{
    if (!map_store_card_ok()) return fail("no microSD card");
    mkdir(LOG_SAVE_DIR, 0775);
    prune();

    // Named by the time, or by time since start-up before the clock is set.
    char name[48], path[96];
    if (clock_is_set()) {
        time_t t = time(NULL);
        struct tm tm;
        localtime_r(&t, &tm);
        strftime(name, sizeof(name), "log_%Y-%m-%d_%H%M%S", &tm);
    } else {
        snprintf(name, sizeof(name), "log_uptime_%llus", (unsigned long long)(esp_timer_get_time() / 1000000));
    }
    size_t base = strlen(name);
    struct stat st;
    for (int i = 2;; i++) { // don't overwrite one from an earlier start-up
        snprintf(path, sizeof(path), LOG_SAVE_DIR "/%s.txt", name);
        if (stat(path, &st) != 0 || i > 99) break;
        snprintf(name + base, sizeof(name) - base, "-%d", i);
    }
    ESP_LOGI(TAG, "saving to %s", path); // so the save itself is in the log

    size_t len;
    char *log = log_buf_copy(&len);
    if (!log) return fail("out of memory");
    FILE *f = fopen(path, "wb");
    bool ok = f && fwrite(log, 1, len, f) == len;
    if (f && fclose(f) != 0) ok = false;
    free(log);
    if (!ok) {
        unlink(path);
        return fail("couldn't write to the card");
    }

    char dump[96];
    dump_path(dump, sizeof(dump), path);
    bool with_dump = save_dump(dump);
    snprintf(s_msg, sizeof(s_msg), "%s.txt%s", name, with_dump ? " + core dump" : "");
    ESP_LOGI(TAG, "saved %s", s_msg);
    return true;
}

static void save_task(void *arg)
{
    s_state = save() ? LOG_SAVE_OK : LOG_SAVE_FAILED;
    vTaskDelete(NULL);
}

void log_save_start(void)
{
    if (s_state == LOG_SAVE_BUSY) return;
    s_state = LOG_SAVE_BUSY;
    if (xTaskCreate(save_task, "log_save", 6144, NULL, 2, NULL) != pdPASS) {
        strlcpy(s_msg, "out of memory", sizeof(s_msg));
        s_state = LOG_SAVE_FAILED;
    }
}

log_save_state_t log_save_state(char *msg, size_t len)
{
    log_save_state_t st = s_state;
    if (msg) strlcpy(msg, st == LOG_SAVE_BUSY ? "" : s_msg, len);
    return st;
}
