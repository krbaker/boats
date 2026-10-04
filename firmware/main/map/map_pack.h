// Tile pack files: one per zoom level, written by the map builder.
//
//   magic "AISPAK1\0", u16 cols, u16 rows, u16 tile_px, u16 reserved,
//   cols*rows x (u32 offset, u32 length), row-major from the top-left tile,
//   then PNG data. All little-endian. Length 0 = no tile there.
//
// Pure C (stdio only) so it can be unit-tested on the host.

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#define MAP_PACK_HEADER_SIZE 16

typedef struct {
    uint32_t offset, length;
} map_pack_entry_t;

typedef struct {
    FILE *f;
    uint16_t cols, rows, tile_px;
    map_pack_entry_t *index;    // cols * rows entries
} map_pack_t;

// Open a pack and read its index. On failure err explains why and the pack is left closed.
bool map_pack_open(map_pack_t *p, const char *path, char *err, size_t errlen);
void map_pack_close(map_pack_t *p);

// Read one tile's PNG into a malloc'd buffer (caller frees). Returns NULL with
// *len = 0 for an empty tile, or NULL with *len = 1 on a read error.
uint8_t *map_pack_read(map_pack_t *p, int col, int row, size_t *len);

// Check that a pack has the expected grid and that every index entry lies within the file.
bool map_pack_check(const char *path, int cols, int rows, int tile_px, char *err, size_t errlen);
