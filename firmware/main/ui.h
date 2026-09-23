#ifndef UI_H
#define UI_H

#include <stdint.h>

#include "lvgl.h"
#include "nav_state.h"

/* Build all widgets on the active screen. Call once, from the LVGL task. */
void ui_init(const char *device_name);

/* Refresh the widgets from a state snapshot. Call periodically from the LVGL task. */
void ui_update(const nav_state_t *s, uint32_t now_ms);

/* Which transport control was tapped on the home screen:
 * 0 = previous, 1 = play/pause, 2 = next, -1 = none. */
int ui_media_zone(lv_coord_t x, lv_coord_t y);

#endif
