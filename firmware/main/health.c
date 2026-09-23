#include "health.h"

#include <stdbool.h>

#include "bflb_wdg.h"
#include "logbuf.h"

/* 1 kHz clock, so the compare value is milliseconds directly. Eight seconds is
 * long enough that a slow flash write or a burst of BLE work never trips it,
 * and short enough that a rider notices a reboot as a blink rather than a gap. */
#define WDG_TIMEOUT_MS 8000
/* A task is considered stopped once it has been quiet for this long. Kept under
 * the watchdog period so the reboot is attributable to the task that stalled. */
#define TASK_QUIET_MS 5000

static struct bflb_device_s *g_wdg;
static volatile uint32_t g_last_seen[HEALTH_TASK_COUNT];
static bool g_ready;   /* the watchdog exists and is configured */
static bool g_armed;   /* ...and every task has checked in at least once */

void health_init(void)
{
    struct bflb_wdg_config_s cfg = {
        .clock_source = WDG_CLKSRC_1K,
        .clock_div = 0,
        .comp_val = WDG_TIMEOUT_MS,
        .mode = WDG_MODE_RESET,
    };

    g_wdg = bflb_device_get_by_name("watchdog");
    if (g_wdg == NULL) {
        logbuf_add("no watchdog");
        return;
    }
    for (int i = 0; i < HEALTH_TASK_COUNT; i++) {
        g_last_seen[i] = 0;
    }
    bflb_wdg_init(g_wdg, &cfg);
    bflb_wdg_reset_countervalue(g_wdg);
    /* Deliberately NOT started here. Bringing up the radio and the display takes
     * longer than the watchdog period, and a watchdog running during start-up
     * reboots the board before it has finished starting - forever. It is armed
     * on the first feed, once every task has reported in at least once. */
    g_ready = true;
}

void health_alive(int task, uint32_t now_ms)
{
    if (task >= 0 && task < HEALTH_TASK_COUNT) {
        /* 0 means "never seen", so never store it. */
        g_last_seen[task] = now_ms ? now_ms : 1;
    }
}

void health_feed(uint32_t now_ms)
{
    if (!g_ready) {
        return;
    }
    for (int i = 0; i < HEALTH_TASK_COUNT; i++) {
        uint32_t seen = g_last_seen[i];
        if (seen == 0) {
            return; /* still starting up: not watching yet */
        }
        if (g_armed && now_ms - seen > TASK_QUIET_MS) {
            return; /* a task has stopped: let the watchdog restart us */
        }
    }
    if (!g_armed) {
        g_armed = true;
        logbuf_add("watchdog armed");
        bflb_wdg_start(g_wdg);
    }
    bflb_wdg_reset_countervalue(g_wdg);
}

int health_rebooted_by_watchdog(void)
{
    return 0; /* the reset cause register is not read yet */
}
