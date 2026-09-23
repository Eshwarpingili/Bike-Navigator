/* BikeNav: turn-by-turn display for the Ai-Thinker Ai-M61-32S 2.4" panel.
 *
 * The phone sends navigation packets over BLE (ble_nav.c); they land in a shared
 * state (nav_state.c); the LVGL task draws that state (ui.c). */

#include <stdio.h>

#include <FreeRTOS.h>
#include <task.h>

#include "bflb_mtd.h"
#include "bflb_mtimer.h"
#include "board.h"
#include "easyflash.h"
#include "rfparam_adapter.h"

#include "lv_conf.h"
#include "lvgl.h"
#include "lv_port_disp.h"
#include "lv_port_indev.h"

#include "apple_link.h"
#include "backlight.h"
#include "ble_nav.h"
#include "health.h"
#include "nav_state.h"
#include "ui.h"

#define LVGL_STACK_WORDS 2048
#define BLE_STACK_WORDS  1024
#define FLIP_HOLD_MS     2000
#define ROTATION_KEY     "bikenav_rot"

static void lv_log_cb(const char *buf)
{
    printf("[lvgl] %s", buf);
}

/* Landscape either way up: 90 or 270 degrees, remembered across power cycles. */
static lv_disp_rot_t load_rotation(void)
{
    uint8_t rot = 0;
    size_t saved = 0;
    ef_get_env_blob(ROTATION_KEY, &rot, sizeof(rot), &saved);
    return (saved == sizeof(rot) && rot == LV_DISP_ROT_270) ? LV_DISP_ROT_270 : LV_DISP_ROT_90;
}

static void save_rotation(lv_disp_rot_t rot)
{
    uint8_t v = (uint8_t)rot;
    ef_set_env_blob(ROTATION_KEY, &v, sizeof(v));
}

/* Tap the controls for previous / play-pause / next.
 * Hold the top-left corner for two seconds to turn the picture upside down. */
static void on_screen_touch(lv_event_t *e)
{
    static uint32_t pressed_at;
    static bool flipped;
    static bool flip_allowed;
    lv_event_code_t code = lv_event_get_code(e);
    uint32_t t = (uint32_t)bflb_mtimer_get_time_ms();

    /* CLICKED, not SHORT_CLICKED: the latter only fires when the finger leaves
     * within 400 ms, and a deliberate press on a screen this size is slower.
     * The flip gesture is excluded by its own flag. */
    if (code == LV_EVENT_CLICKED && !flipped) {
        lv_point_t p;
        lv_indev_get_point(lv_indev_get_act(), &p);
        int zone = ui_media_zone(p.x, p.y);
        switch (zone) {
            case 0: apple_link_media_command(MEDIA_CMD_PREVIOUS); break;
            case 1: apple_link_media_command(MEDIA_CMD_TOGGLE); break;
            case 2: apple_link_media_command(MEDIA_CMD_NEXT); break;
            default: break;
        }
        return;
    }
    if (code == LV_EVENT_PRESSED) {
        lv_point_t p;
        lv_indev_get_point(lv_indev_get_act(), &p);
        pressed_at = t;
        flipped = false;
        /* Only the top-left corner flips the screen. A bag strap or a glove
         * resting on the glass would otherwise turn the display upside down
         * mid-ride, and the rider has no easy way to put it back. */
        flip_allowed = p.x < lv_disp_get_hor_res(NULL) / 3 &&
                       p.y < lv_disp_get_ver_res(NULL) / 3;
    } else if (code == LV_EVENT_PRESSING && flip_allowed && !flipped &&
               t - pressed_at > FLIP_HOLD_MS) {
        flipped = true;
        lv_disp_rot_t rot = lv_disp_get_rotation(NULL) == LV_DISP_ROT_90 ? LV_DISP_ROT_270 : LV_DISP_ROT_90;
        lv_disp_set_rotation(NULL, rot);
        save_rotation(rot);
        printf("[ui] rotation -> %d\r\n", (int)rot);
    }
}

static void lvgl_task(void *arg)
{
    (void)arg;
    nav_state_t snapshot;

    lv_log_register_print_cb(lv_log_cb);
    lv_init();
    lv_port_disp_init();
    lv_port_indev_init();
    backlight_init();
    lv_disp_set_rotation(NULL, load_rotation());

    ui_init(BIKENAV_DEVICE_NAME);
    lv_obj_add_event_cb(lv_scr_act(), on_screen_touch, LV_EVENT_ALL, NULL);

    uint32_t last_ui = 0;
    while (1) {
        uint32_t t = (uint32_t)bflb_mtimer_get_time_ms();
        if (t - last_ui >= 100) {
            last_ui = t;
            nav_state_snapshot(&snapshot);
            ui_update(&snapshot, t);
        }
        lv_task_handler();
        health_alive(HEALTH_TASK_UI, t);
        health_feed(t);
        vTaskDelay(pdMS_TO_TICKS(5));
    }
}

int main(void)
{
    board_init();
    printf("\r\nBikeNav starting\r\n");

    bflb_mtd_init();
    easyflash_init(); /* also required by the BLE stack */

    if (rfparam_init(0, NULL, 0) != 0) {
        printf("RF init failed\r\n");
    }

    nav_state_init();
    health_init();

    xTaskCreate(lvgl_task, "lvgl", LVGL_STACK_WORDS, NULL, 3, NULL);
    xTaskCreate(ble_nav_task, "ble_nav", BLE_STACK_WORDS, NULL, configMAX_PRIORITIES - 2, NULL);

    vTaskStartScheduler();
    while (1) {
    }
}
