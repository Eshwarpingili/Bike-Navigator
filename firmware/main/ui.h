#ifndef UI_H
#define UI_H

#include <stdbool.h>
#include <stdint.h>

#include "lvgl.h"
#include "nav_state.h"

/* Build all widgets on the active screen. Call once, from the LVGL task. */
void ui_init(const char *device_name);

/* Refresh the widgets from a state snapshot. Call periodically from the LVGL task. */
void ui_update(const nav_state_t *s, uint32_t now_ms);

/* What a tap asked for.
 *
 * Brightness and moving between screens are settled inside the UI, because
 * nothing outside it needs to know. Anything that has to leave the board comes
 * back here, for main.c to send to the phone. */
typedef enum {
    UI_ACT_NONE = 0,
    UI_ACT_MEDIA,        /* arg is a MEDIA_CMD_* value */
    UI_ACT_CALL_ANSWER,
    UI_ACT_CALL_DECLINE,
} ui_act_kind_t;

typedef struct {
    ui_act_kind_t kind;
    uint8_t arg;
} ui_action_t;

/* Route one tap to whatever is under it. */
ui_action_t ui_tap(lv_coord_t x, lv_coord_t y);

#endif
