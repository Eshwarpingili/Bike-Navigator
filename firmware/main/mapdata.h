#ifndef MAPDATA_H
#define MAPDATA_H

#include <stdbool.h>
#include <stdint.h>

/* The street network, stored in flash above the firmware.
 *
 * Geometry rather than pictures: the board can turn a list of points to face
 * the way the rider is going with a multiply and an add, where turning a
 * photograph the same way means resampling every pixel. It also needs no
 * network, no account and no phone battery, and it still works where there is
 * no signal. This is what a bike computer does.
 *
 * Built by tools/build_map.py from OpenStreetMap data (ODbL) and written to
 * MAP_FLASH_ADDR with flash.ps1 -Map. The 4 MB layout the SDK ships leaves the
 * top half of this 8 MB part unused, which is where it goes. */

/* Above everything the partition table claims. */
#define MAP_FLASH_ADDR 0x400000

/* The longest run of points handed back at once. Longer ways are split by the
 * builder, so this is a buffer size, not a limit on road length. */
#define MAP_MAX_POINTS 255

typedef struct {
    uint8_t road_class;              /* 1 = major .. 4 = minor */
    uint8_t count;
    int32_t lat_e7[MAP_MAX_POINTS];
    int32_t lon_e7[MAP_MAX_POINTS];
} map_way_t;

/* Reads and checks the header. Safe to call when no map has been flashed:
 * everything else then reports there is nothing to draw. */
void mapdata_init(void);

bool mapdata_ready(void);

/* True when the point is inside the area the stored map covers. */
bool mapdata_covers(int32_t lat_e7, int32_t lon_e7);

typedef void (*map_way_fn)(const map_way_t *way, void *user);

/* Every way in the tiles around a point. `radius` is in tiles, each about a
 * kilometre, so 1 covers a 3x3 block - more than a 320x240 screen can show. */
void mapdata_each_way(int32_t lat_e7, int32_t lon_e7, int radius,
                      map_way_fn fn, void *user);

#endif
