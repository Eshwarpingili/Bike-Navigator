#ifndef PREFS_H
#define PREFS_H

#include <stdint.h>

/* Settings that survive a power cycle, in the flash key-value store.
 *
 * The bike's ignition cuts power without warning, so anything the rider
 * chooses has to be written when it changes rather than on the way down. */

/* Call once, after easyflash_init(). */
void prefs_init(void);

/* LVGL rotation, stored raw. Landscape either way up. */
uint8_t prefs_rotation(void);
void prefs_set_rotation(uint8_t rot);

/* Backlight: 0 means follow the clock (bright by day, dim at night),
 * anything else is a fixed 5..100 percent. */
#define BRIGHTNESS_AUTO 0
uint8_t prefs_brightness(void);
void prefs_set_brightness(uint8_t percent);

#endif
