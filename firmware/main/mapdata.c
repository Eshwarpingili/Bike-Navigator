#include "mapdata.h"

#include <string.h>

#include "bflb_flash.h"

#include "logbuf.h"

/* Header, little endian, as written by tools/build_map.py:
 *   char magic[6] = "BNMAP1"
 *   u32  tile_deg    tile size in degrees * 1e7
 *   i32  lat0, lon0  south-west corner of the grid
 *   u16  rows, cols
 *   u32  tile_count
 *   then tile_count * (u32 offset, u32 length)
 *
 * The struct is not read directly onto this because the file is packed and the
 * compiler is free to pad; every field is pulled out by hand instead. */
#define HEADER_BYTES 26
#define INDEX_ENTRY  8

/* One tile of road, read from flash and parsed in RAM. Reading the blob once
 * and walking it in memory beats hundreds of small flash reads. Tiles in the
 * densest parts of a city run to a few kilobytes; anything past this is
 * truncated at a way boundary rather than dropped. */
static uint8_t g_tile[24 * 1024];

static struct {
    bool ready;
    uint32_t tile_deg;
    int32_t lat0, lon0;
    uint16_t rows, cols;
    uint32_t tile_count;
    uint32_t index_addr;
} g_map;

static map_way_t g_way;

static uint32_t rd32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static uint16_t rd16(const uint8_t *p)
{
    return (uint16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
}

void mapdata_init(void)
{
    uint8_t head[HEADER_BYTES];

    memset(&g_map, 0, sizeof(g_map));
    if (bflb_flash_read(MAP_FLASH_ADDR, head, sizeof(head)) != 0) {
        logbuf_add("map: flash read failed");
        return;
    }
    if (memcmp(head, "BNMAP1", 6) != 0) {
        /* No map flashed. Not a fault: the board works without one. */
        logbuf_add("map: none");
        return;
    }
    g_map.tile_deg = rd32(head + 6);
    g_map.lat0 = (int32_t)rd32(head + 10);
    g_map.lon0 = (int32_t)rd32(head + 14);
    g_map.rows = rd16(head + 18);
    g_map.cols = rd16(head + 20);
    g_map.tile_count = rd32(head + 22);
    g_map.index_addr = MAP_FLASH_ADDR + HEADER_BYTES;

    if (g_map.tile_deg == 0 || g_map.rows == 0 || g_map.cols == 0 ||
        g_map.tile_count != (uint32_t)g_map.rows * g_map.cols) {
        logbuf_add("map: header bad");
        return;
    }
    g_map.ready = true;
    logbuf_add("map %ux%u tiles", g_map.rows, g_map.cols);
}

bool mapdata_ready(void)
{
    return g_map.ready;
}

static bool tile_of(int32_t lat_e7, int32_t lon_e7, int *row, int *col)
{
    int64_t dr = (int64_t)lat_e7 - g_map.lat0;
    int64_t dc = (int64_t)lon_e7 - g_map.lon0;
    if (dr < 0 || dc < 0) {
        return false;
    }
    *row = (int)(dr / g_map.tile_deg);
    *col = (int)(dc / g_map.tile_deg);
    return *row < g_map.rows && *col < g_map.cols;
}

bool mapdata_covers(int32_t lat_e7, int32_t lon_e7)
{
    int row, col;
    return g_map.ready && tile_of(lat_e7, lon_e7, &row, &col);
}

/* Walk one tile's blob, handing each way to the caller. */
static void walk_tile(int row, int col, map_way_fn fn, void *user)
{
    uint8_t entry[INDEX_ENTRY];
    uint32_t slot = (uint32_t)row * g_map.cols + (uint32_t)col;

    if (bflb_flash_read(g_map.index_addr + slot * INDEX_ENTRY, entry, sizeof(entry)) != 0) {
        return;
    }
    uint32_t offset = rd32(entry);
    uint32_t length = rd32(entry + 4);
    if (offset == 0 || length == 0) {
        return; /* nothing built here */
    }
    if (length > sizeof(g_tile)) {
        length = sizeof(g_tile);
    }
    if (bflb_flash_read(MAP_FLASH_ADDR + offset, g_tile, length) != 0) {
        return;
    }

    /* Points are 16-bit offsets within the tile; this turns them back into
     * degrees. The divide is by 65536, so the compiler makes it a shift. */
    int32_t tlat = g_map.lat0 + (int32_t)((uint32_t)row * g_map.tile_deg);
    int32_t tlon = g_map.lon0 + (int32_t)((uint32_t)col * g_map.tile_deg);

    uint32_t pos = 0;
    while (pos + 2 <= length) {
        uint8_t road_class = g_tile[pos];
        if (road_class == 0 || road_class > 4) {
            break; /* end of tile, or a truncated read landing mid-way */
        }
        uint8_t count = g_tile[pos + 1];
        pos += 2;
        if (count < 2 || pos + (uint32_t)count * 4 > length) {
            break;
        }
        g_way.road_class = road_class;
        g_way.count = count;
        for (uint8_t i = 0; i < count; i++) {
            uint16_t x = rd16(&g_tile[pos + (uint32_t)i * 4]);
            uint16_t y = rd16(&g_tile[pos + (uint32_t)i * 4 + 2]);
            g_way.lon_e7[i] = tlon + (int32_t)(((uint64_t)x * g_map.tile_deg) >> 16);
            g_way.lat_e7[i] = tlat + (int32_t)(((uint64_t)y * g_map.tile_deg) >> 16);
        }
        pos += (uint32_t)count * 4;
        fn(&g_way, user);
    }
}

void mapdata_each_way(int32_t lat_e7, int32_t lon_e7, int radius,
                      map_way_fn fn, void *user)
{
    int row, col;

    if (!g_map.ready || fn == NULL) {
        return;
    }
    if (!tile_of(lat_e7, lon_e7, &row, &col)) {
        return;
    }
    for (int r = row - radius; r <= row + radius; r++) {
        if (r < 0 || r >= g_map.rows) {
            continue;
        }
        for (int c = col - radius; c <= col + radius; c++) {
            if (c < 0 || c >= g_map.cols) {
                continue;
            }
            walk_tile(r, c, fn, user);
        }
    }
}
