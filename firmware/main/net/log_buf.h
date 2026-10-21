// Keeps the most recent log output in RAM, as well as printing it to the
// console as usual, so it can be read over the network (GET /log).

#pragma once

#include <stddef.h>

// Call first thing: anything logged before it only goes to the console.
void log_buf_init(void);

// The buffered log, oldest first, starting at a whole line, in a new buffer
// the caller frees. NULL if there's no memory.
char *log_buf_copy(size_t *len);
