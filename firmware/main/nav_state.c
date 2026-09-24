#include "nav_state.h"

#include <string.h>

#include "FreeRTOS.h"
#include "semphr.h"

#include "bflb_mtimer.h"

static nav_state_t g_state;
static SemaphoreHandle_t g_lock;

/* Last navigation packet, to tell a genuinely new instruction from a repeat. */
static uint8_t g_last_packet[80];
static uint16_t g_last_packet_len;

static bool packet_is_new(const uint8_t *d, uint16_t len)
{
    if (len == g_last_packet_len && len <= sizeof(g_last_packet) &&
        memcmp(g_last_packet, d, len) == 0) {
        return false;
    }
    g_last_packet_len = len <= sizeof(g_last_packet) ? len : 0;
    if (g_last_packet_len) {
        memcpy(g_last_packet, d, g_last_packet_len);
    }
    return true;
}

static uint32_t rd_u32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static uint16_t rd_u16(const uint8_t *p)
{
    return (uint16_t)(p[0] | (p[1] << 8));
}

/* Copy printable text, stopping at NUL, and never split a UTF-8 sequence. */
static void copy_text(char *dst, size_t cap, const uint8_t *src, size_t len)
{
    size_t n = 0;
    while (n < len && n < cap - 1 && src[n] != 0) {
        n++;
    }
    while (n > 0 && n < len && (src[n] & 0xC0) == 0x80) {
        n--; /* next byte is a continuation byte: drop the partial character */
    }
    for (size_t i = 0; i < n; i++) {
        dst[i] = (src[i] < 0x20) ? ' ' : (char)src[i];
    }
    dst[n] = '\0';
}

void nav_state_init(void)
{
    memset(&g_state, 0, sizeof(g_state));
    g_state.remaining_m = NAV_UNKNOWN_U32;
    g_state.minutes_left = NAV_UNKNOWN_U16;
    g_state.speed_kmh = NAV_UNKNOWN_U8;
    g_lock = xSemaphoreCreateMutex();
}

static bool apply_locked(const uint8_t *d, uint16_t len, uint32_t now_ms)
{
    nav_state_t *s = &g_state;

    if (d[0] == 0x01 || d[0] == 0x02) {
        s->last_packet_ms = now_ms;
        if (packet_is_new(d, len)) {
            s->last_change_ms = now_ms;
        }
    }

    switch (d[0]) {
        case 0x01: /* basic, Sygic compatible */
            if (len < 3) {
                return false;
            }
            if (d[2] == DIR_NONE) {
                /* Sygic keeps sending while it has no route to give; that is our cue
                 * to show the home screen rather than the last turn forever. */
                s->mode = NAV_MODE_IDLE;
                s->direction = DIR_NONE;
                s->flags = 0;
                return true;
            }
            s->mode = NAV_MODE_BASIC;
            s->limit_kmh = d[1];
            s->direction = d[2] < DIR_COUNT ? d[2] : DIR_NONE;
            s->flags = (s->direction == DIR_OFF_ROUTE) ? NAV_FLAG_REROUTING : 0;
            copy_text(s->distance_text, sizeof(s->distance_text), d + 3, len - 3);
            s->street[0] = '\0';
            s->then_direction = DIR_NONE;
            s->remaining_m = NAV_UNKNOWN_U32;
            s->minutes_left = NAV_UNKNOWN_U16;
            s->speed_kmh = NAV_UNKNOWN_U8;
            return true;

        case 0x02: /* full state */
            if (len < 16) {
                return false;
            }
            s->mode = NAV_MODE_FULL;
            s->direction = d[1] < DIR_COUNT ? d[1] : DIR_NONE;
            s->flags = d[2];
            s->distance_m = rd_u32(d + 3);
            s->remaining_m = rd_u32(d + 7);
            s->minutes_left = rd_u16(d + 11);
            s->speed_kmh = d[13];
            s->limit_kmh = d[14];
            s->then_direction = d[15] < DIR_COUNT ? d[15] : DIR_NONE;
            copy_text(s->street, sizeof(s->street), d + 16, len - 16);
            s->distance_text[0] = '\0';
            return true;

        case 0x03: /* clock sync */
            if (len < 7) {
                return false;
            }
            s->clock_offset_s = (int64_t)rd_u32(d + 1) + (int16_t)rd_u16(d + 5) * 60 - (int64_t)(now_ms / 1000);
            s->clock_valid = true;
            return true;

        case 0x05: /* route shape ahead, already rotated heading-up by the phone */
            if (len < 3) {
                return false;
            }
            {
                uint8_t count = d[1];
                uint8_t unit = d[2];
                if (count > NAV_ROUTE_MAX || unit == 0 || len < (uint16_t)(3 + 2 * count)) {
                    return false;
                }
                for (uint8_t i = 0; i < count; i++) {
                    g_state.route_x[i] = (int8_t)d[3 + 2 * i];
                    g_state.route_y[i] = (int8_t)d[4 + 2 * i];
                }
                g_state.route_points = count;
                g_state.route_unit_m = unit;
            }
            return true;

        case 0x04: /* idle */
            s->mode = NAV_MODE_IDLE;
            s->direction = DIR_NONE;
            s->flags = 0;
            /* The shape goes with the route. Leaving it behind would draw the
             * last road the rider was on as though it were still ahead. */
            s->route_points = 0;
            return true;

        default:
            return false;
    }
}

bool nav_state_apply_packet(const uint8_t *data, uint16_t len, uint32_t now_ms)
{
    if (data == NULL || len == 0) {
        return false;
    }
    xSemaphoreTake(g_lock, portMAX_DELAY);
    bool ok = apply_locked(data, len, now_ms);
    if (ok) {
        g_state.version++;
    }
    xSemaphoreGive(g_lock);
    return ok;
}

void nav_state_set_connected(bool connected, uint32_t now_ms)
{
    (void)now_ms;
    xSemaphoreTake(g_lock, portMAX_DELAY);
    /* The last route stays on screen after a disconnect; the UI greys it out once
     * it is stale, and the phone resends everything when it reconnects. */
    g_state.connected = connected;
    g_state.version++;
    xSemaphoreGive(g_lock);
}

void nav_state_set_music_text(bool is_title, const char *text, uint16_t len)
{
    xSemaphoreTake(g_lock, portMAX_DELAY);
    char *dst = is_title ? g_state.music_title : g_state.music_artist;
    copy_text(dst, NAV_MUSIC_MAX + 1, (const uint8_t *)text, len);
    g_state.music_valid = g_state.music_title[0] != '\0' || g_state.music_artist[0] != '\0';
    g_state.version++;
    xSemaphoreGive(g_lock);
}

void nav_state_set_music_playing(bool playing)
{
    xSemaphoreTake(g_lock, portMAX_DELAY);
    if (g_state.music_playing != playing) {
        g_state.music_playing = playing;
        g_state.version++;
    }
    xSemaphoreGive(g_lock);
}

void nav_state_set_call(uint32_t uid, const char *name, uint16_t len,
                        bool can_answer, bool can_decline)
{
    xSemaphoreTake(g_lock, portMAX_DELAY);
    /* The name arrives in a second message, after the ring itself. Keep what is
     * already there rather than blanking the caller each time the phone
     * repeats the notification. */
    if (name != NULL && len > 0) {
        copy_text(g_state.call_name, NAV_CALLER_MAX + 1, (const uint8_t *)name, len);
    } else if (!g_state.call_ringing || g_state.call_uid != uid) {
        g_state.call_name[0] = '\0';
    }
    g_state.call_ringing = true;
    g_state.call_uid = uid;
    g_state.call_can_answer = can_answer;
    g_state.call_can_decline = can_decline;
    g_state.version++;
    xSemaphoreGive(g_lock);
}

void nav_state_clear_call(void)
{
    xSemaphoreTake(g_lock, portMAX_DELAY);
    if (g_state.call_ringing) {
        g_state.version++;
    }
    g_state.call_ringing = false;
    g_state.call_name[0] = '\0';
    g_state.call_uid = 0;
    g_state.call_can_answer = false;
    g_state.call_can_decline = false;
    xSemaphoreGive(g_lock);
}

void nav_state_set_volume(uint8_t percent)
{
    xSemaphoreTake(g_lock, portMAX_DELAY);
    if (!g_state.volume_valid || g_state.volume_percent != percent) {
        g_state.volume_percent = percent;
        g_state.volume_valid = true;
        g_state.version++;
    }
    xSemaphoreGive(g_lock);
}

void nav_state_clear_music(void)
{
    xSemaphoreTake(g_lock, portMAX_DELAY);
    g_state.music_title[0] = '\0';
    g_state.music_artist[0] = '\0';
    g_state.music_valid = false;
    g_state.music_playing = false;
    g_state.version++;
    xSemaphoreGive(g_lock);
}

void nav_state_set_local_time(uint32_t seconds_of_day)
{
    xSemaphoreTake(g_lock, portMAX_DELAY);
    /* The UI reads (uptime + offset) % 86400, so store the difference. Uptime comes
     * from the same clock the UI uses. */
    g_state.clock_offset_s = (int64_t)seconds_of_day - (int64_t)(bflb_mtimer_get_time_ms() / 1000);
    g_state.clock_valid = true;
    g_state.version++;
    xSemaphoreGive(g_lock);
}

void nav_state_set_apple_status(bool paired, bool media, bool clock, uint8_t services,
                                uint8_t att_err, bool subscribed)
{
    xSemaphoreTake(g_lock, portMAX_DELAY);
    g_state.apple_paired = paired;
    g_state.apple_media = media;
    g_state.apple_clock = clock;
    g_state.apple_services = services;
    g_state.apple_att_err = att_err;
    g_state.apple_subscribed = subscribed;
    g_state.version++;
    xSemaphoreGive(g_lock);
}

void nav_state_set_apple_debug(uint16_t entity, uint16_t ccc, uint8_t write_err,
                               uint16_t notifs, bool sub_lost, uint8_t ccc_err, uint8_t sec)
{
    xSemaphoreTake(g_lock, portMAX_DELAY);
    g_state.apple_entity = entity;
    g_state.apple_ccc = ccc;
    g_state.apple_write_err = write_err;
    g_state.apple_notifs = notifs;
    g_state.apple_sub_lost = sub_lost;
    g_state.apple_ccc_err = ccc_err;
    g_state.apple_sec = sec;
    g_state.version++;
    xSemaphoreGive(g_lock);
}

void nav_state_snapshot(nav_state_t *out)
{
    xSemaphoreTake(g_lock, portMAX_DELAY);
    *out = g_state;
    xSemaphoreGive(g_lock);
}
