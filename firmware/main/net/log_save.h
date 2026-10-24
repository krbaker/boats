// Saves the buffered log (log_buf) to the microSD card, so it can be kept even
// when the network is down, with the last crash's core dump beside it if there
// is one. The newest LOG_SAVE_KEEP saves are kept.

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <time.h>

#include "map_store.h"

#define LOG_SAVE_DIR  MAP_ROOT "/logs"
#define LOG_SAVE_KEEP 10

typedef enum {
    LOG_SAVE_IDLE,
    LOG_SAVE_BUSY,
    LOG_SAVE_OK,
    LOG_SAVE_FAILED,
} log_save_state_t;

typedef struct {
    char name[48];          // the log, e.g. log_2026-10-24_143205.txt
    uint32_t size;
    time_t mtime;           // 0 if the clock wasn't set when it was saved
    uint32_t dump_size;     // its core dump (same name, .bin); 0 = none
} log_file_t;

// Save in the background (nothing if a save is already running).
void log_save_start(void);

// The last save's state; msg gets the file name, or why it failed.
log_save_state_t log_save_state(char *msg, size_t len);

// The saved logs, newest first. Returns how many (at most max).
int log_save_list(log_file_t *out, int max);

// True if name is one of ours (log_*.txt / .bin, no path): safe to open.
bool log_save_valid_name(const char *name);
