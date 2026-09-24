#include "apple_link.h"

#include <stdio.h>
#include <string.h>
#include <sys/errno.h>

#include "bluetooth.h"
#include "conn.h"
#include "gatt.h"
#include "uuid.h"

#include "logbuf.h"
#include "nav_state.h"

/* Apple Media Service */
#define UUID_AMS        BT_UUID_DECLARE_128(BT_UUID_128_ENCODE(0x89D3502B, 0x0F36, 0x433A, 0x8EF4, 0xC502AD55F8DC))
#define UUID_AMS_REMOTE BT_UUID_DECLARE_128(BT_UUID_128_ENCODE(0x9B3C81D8, 0x57B1, 0x4A8A, 0xB8DF, 0x0E56F7CA51C2))
#define UUID_AMS_ENTITY BT_UUID_DECLARE_128(BT_UUID_128_ENCODE(0x2F7CABCE, 0x808D, 0x411F, 0x9A0C, 0xBB92BA96C102))
/* Apple Notification Center Service: everything the phone is showing, of which
 * only incoming calls are wanted here. */
#define UUID_ANCS       BT_UUID_DECLARE_128(BT_UUID_128_ENCODE(0x7905F431, 0xB5CE, 0x4E99, 0xA40F, 0x4B1E122D00D0))
#define UUID_ANCS_NOTIF BT_UUID_DECLARE_128(BT_UUID_128_ENCODE(0x9FBF120D, 0x6301, 0x42D9, 0x8C58, 0x25E699A21DBD))
#define UUID_ANCS_CTRL  BT_UUID_DECLARE_128(BT_UUID_128_ENCODE(0x69D1D8F3, 0x45E1, 0x49A8, 0x9821, 0x9BBDFDAAD9D9))
#define UUID_ANCS_DATA  BT_UUID_DECLARE_128(BT_UUID_128_ENCODE(0x22EAC6E9, 0x24D6, 0x4BB5, 0xBE44, 0xB36ACE7C7BFB))

#define ANCS_EVENT_ADDED     0
#define ANCS_EVENT_REMOVED   2
#define ANCS_CATEGORY_CALL   1
#define ANCS_FLAG_POSITIVE   0x08 /* the notification can be accepted */
#define ANCS_FLAG_NEGATIVE   0x10 /* ...and/or refused */
#define ANCS_CMD_GET_ATTRS   0
#define ANCS_CMD_ACTION      2
#define ANCS_ATTR_TITLE      1    /* for a call, the caller */
#define ANCS_ACTION_POSITIVE 0
#define ANCS_ACTION_NEGATIVE 1

/* Current Time Service */
#define UUID_CTS          BT_UUID_DECLARE_16(0x1805)
#define UUID_CURRENT_TIME BT_UUID_DECLARE_16(0x2A2B)
/* Generic Access device name, used only to prove the link can read at all */
#define UUID_DEVICE_NAME  BT_UUID_DECLARE_16(0x2A00)

#define ENTITY_PLAYER 0
#define ENTITY_TRACK  2
#define PLAYER_ATTR_PLAYBACK_INFO 1
#define TRACK_ATTR_ARTIST 0
#define TRACK_ATTR_TITLE  2

#define STEP_INTERVAL_MS 1500
#define MAX_ROUNDS 6

/* One request at a time, always started from apple_link_tick() and never from
 * inside a GATT callback: this stack rejects a discovery started while another
 * one is still finishing. */
enum step {
    STEP_DONE = 0,
    STEP_PROBE,      /* read the phone's device name, to get a real ATT error code */
    STEP_SERVICES,   /* list every primary service */
    STEP_AMS_CHARS,
    /* No descriptor-discovery steps: this stack's descriptor scan returns
     * nothing at all inside Apple's services, so the notification descriptor is
     * worked out from the characteristic properties and confirmed by writing. */
    STEP_CCC_REMOTE,
    STEP_CCC_ENTITY,
    STEP_CTS_CHARS,
    STEP_ANCS_CHARS,
    STEP_CCC_NOTIF,  /* ANCS Notification Source */
    STEP_CCC_DATA,   /* ANCS Data Source */
    STEP_SUBSCRIBE,
    STEP_WRITE_PLAYER,  /* ask for play/pause state */
    STEP_CLEAR_TRACK,   /* drop the track registration, so re-adding it resends */
    STEP_WRITE_TRACK,   /* ask for artist + title */
    STEP_READ_TIME,
};

static struct bt_conn *g_conn;
static struct bt_gatt_discover_params g_discover;
static struct bt_gatt_subscribe_params g_sub_media;
static struct bt_gatt_subscribe_params g_sub_time;
static struct bt_gatt_subscribe_params g_sub_notif;
static struct bt_gatt_subscribe_params g_sub_data;
static struct bt_gatt_read_params g_read;
static struct bt_gatt_write_params g_write;

static volatile enum step g_step;
static volatile bool g_busy;          /* a request is in flight */
static uint32_t g_last_step_ms;
static uint8_t g_rounds;

static uint16_t g_ams_start, g_ams_end, g_cts_start, g_cts_end;
static uint16_t g_ancs_start, g_ancs_end;
static uint16_t g_entity_handle, g_remote_handle, g_time_handle;
static uint16_t g_notif_handle, g_ctrl_handle, g_data_handle;
static uint16_t g_entity_ccc, g_time_ccc, g_notif_ccc, g_data_ccc;
static uint8_t g_entity_props, g_remote_props, g_time_props;
static uint8_t g_notif_props, g_ctrl_props, g_data_props;

/* The call currently ringing, if any. */
static uint32_t g_call_uid;
static bool g_call_active;
static bool g_call_answerable, g_call_declinable;
static bool g_want_title;      /* ask the phone who is calling, from the tick */
static uint8_t g_pending_action; /* 1 = answer, 2 = decline, 0 = nothing */
/* The caller's name can arrive split across several notifications, so it is
 * reassembled here rather than assumed to fit in one. */
static char g_title[NAV_CALLER_MAX + 1];
static uint8_t g_title_len;
static uint16_t g_title_left;
static bool g_subscribed;
static uint8_t g_services_seen;
static bool g_paired;
static uint8_t g_att_err = 0xFF;      /* 0xFF = not probed yet */
static uint8_t g_write_err = 0xFF;    /* ATT error of the entity-update write */
static uint16_t g_notif_count;
static bool g_sub_lost;
static bool g_have_title;
static uint16_t g_remote_ccc;
static uint8_t g_ccc_err = 0xFF;      /* phone's answer to enabling notifications */
static uint8_t g_sec_level;

static void publish_status(void)
{
    nav_state_set_apple_status(g_paired, g_entity_handle != 0, g_time_handle != 0,
                               g_services_seen, g_att_err, g_subscribed);
    nav_state_set_apple_debug(g_entity_handle, g_entity_ccc, g_write_err, g_notif_count, g_sub_lost,
                              g_ccc_err, g_sec_level);
}

static void next_step(enum step step)
{
    g_step = step;
    g_busy = false;
    g_last_step_ms = 0; /* run it on the next tick */
}

/* ---------------- notifications ---------------- */

static uint8_t on_media_update(struct bt_conn *conn, struct bt_gatt_subscribe_params *params,
                               const void *data, uint16_t length)
{
    (void)conn;
    if (data == NULL) { /* the phone refused or dropped the subscription */
        params->value_handle = 0;
        g_sub_lost = true;
        g_subscribed = false;
        publish_status();
        return BT_GATT_ITER_STOP;
    }
    g_notif_count++;
    g_sub_lost = false; /* something arrived, so the subscription is alive now */
    const uint8_t *d = data;
    if (g_notif_count <= 6) {
        logbuf_add("notif e=%u a=%u len=%u", d[0], length > 1 ? d[1] : 0, length);
    }
    if (length < 3) {
        publish_status();
        return BT_GATT_ITER_CONTINUE;
    }
    publish_status();
    const char *value = (const char *)(d + 3);
    uint16_t value_len = length - 3;

    if (d[0] == ENTITY_TRACK && d[1] == TRACK_ATTR_TITLE) {
        nav_state_set_music_text(true, value, value_len);
        if (value_len > 0) {
            g_have_title = true;
        }
    } else if (d[0] == ENTITY_TRACK && d[1] == TRACK_ATTR_ARTIST) {
        nav_state_set_music_text(false, value, value_len);
    } else if (d[0] == ENTITY_PLAYER && d[1] == PLAYER_ATTR_PLAYBACK_INFO) {
        nav_state_set_music_playing(value_len > 0 && value[0] == '1');
    }
    return BT_GATT_ITER_CONTINUE;
}

/* Current Time: year(2) month day hours minutes seconds ... */
static void apply_current_time(const uint8_t *d, uint16_t length)
{
    if (length < 7 || d[4] > 23 || d[5] > 59 || d[6] > 59) {
        return;
    }
    nav_state_set_local_time(d[4] * 3600 + d[5] * 60 + d[6]);
}

static uint8_t on_time_update(struct bt_conn *conn, struct bt_gatt_subscribe_params *params,
                              const void *data, uint16_t length)
{
    (void)conn;
    if (data == NULL) {
        params->value_handle = 0;
        return BT_GATT_ITER_STOP;
    }
    apply_current_time(data, length);
    return BT_GATT_ITER_CONTINUE;
}

/* ---------------- request callbacks ---------------- */

static uint8_t on_probe_read(struct bt_conn *conn, uint8_t err, struct bt_gatt_read_params *params,
                             const void *data, uint16_t length)
{
    (void)conn;
    (void)params;
    (void)data;
    g_att_err = err;
    logbuf_add("[apple] probe: att err 0x%02x, %u bytes", err, length);
    publish_status();
    next_step(STEP_SERVICES);
    return BT_GATT_ITER_STOP;
}

/* ---------------- incoming calls ---------------- */

static void publish_call(void)
{
    if (!g_call_active) {
        nav_state_clear_call();
        return;
    }
    nav_state_set_call(g_call_uid, g_title_len ? g_title : NULL, g_title_len,
                       g_call_answerable, g_call_declinable);
}

/* The phone announces every notification it shows. Only calls are wanted, and
 * only the fact of them: the caller's name has to be asked for separately. */
static uint8_t on_notif_source(struct bt_conn *conn, struct bt_gatt_subscribe_params *params,
                               const void *data, uint16_t length)
{
    (void)conn;
    if (data == NULL) {
        params->value_handle = 0;
        return BT_GATT_ITER_STOP;
    }
    if (length < 8) {
        return BT_GATT_ITER_CONTINUE;
    }
    const uint8_t *d = data;
    uint8_t event = d[0];
    uint8_t flags = d[1];
    uint8_t category = d[2];
    uint32_t uid = (uint32_t)d[4] | ((uint32_t)d[5] << 8) |
                   ((uint32_t)d[6] << 16) | ((uint32_t)d[7] << 24);

    if (category != ANCS_CATEGORY_CALL) {
        return BT_GATT_ITER_CONTINUE;
    }
    if (event == ANCS_EVENT_REMOVED) {
        /* Answered on the phone, rung off, or missed - either way it is over. */
        if (g_call_active && uid == g_call_uid) {
            g_call_active = false;
            publish_call();
        }
        return BT_GATT_ITER_CONTINUE;
    }
    if (event != ANCS_EVENT_ADDED) {
        return BT_GATT_ITER_CONTINUE; /* a modify tells us nothing new */
    }

    g_call_active = true;
    g_call_uid = uid;
    g_call_answerable = (flags & ANCS_FLAG_POSITIVE) != 0;
    g_call_declinable = (flags & ANCS_FLAG_NEGATIVE) != 0;
    g_title_len = 0;
    g_title_left = 0;
    g_title[0] = '\0';
    publish_call();
    /* Asked for from the tick, never from in here: this stack will not take a
     * second request while one is still finishing. */
    g_want_title = true;
    logbuf_add("call uid=%lu f=%02x", (unsigned long)uid, flags);
    return BT_GATT_ITER_CONTINUE;
}

/* The answer to "who is calling". Long names arrive split across several
 * notifications, with only the first carrying a header. */
static uint8_t on_data_source(struct bt_conn *conn, struct bt_gatt_subscribe_params *params,
                              const void *data, uint16_t length)
{
    (void)conn;
    if (data == NULL) {
        params->value_handle = 0;
        return BT_GATT_ITER_STOP;
    }
    const uint8_t *d = data;
    uint16_t offset = 0;

    if (g_title_left == 0) {
        if (length < 8 || d[0] != ANCS_CMD_GET_ATTRS || d[5] != ANCS_ATTR_TITLE) {
            return BT_GATT_ITER_CONTINUE;
        }
        /* A late answer to a request about an earlier call would otherwise put
         * the wrong name on the screen while a different one is ringing. */
        uint32_t uid = (uint32_t)d[1] | ((uint32_t)d[2] << 8) |
                       ((uint32_t)d[3] << 16) | ((uint32_t)d[4] << 24);
        if (!g_call_active || uid != g_call_uid) {
            return BT_GATT_ITER_CONTINUE;
        }
        g_title_left = (uint16_t)d[6] | ((uint16_t)d[7] << 8);
        g_title_len = 0;
        offset = 8;
    }

    uint16_t avail = length > offset ? length - offset : 0;
    if (avail > g_title_left) {
        avail = g_title_left;
    }
    for (uint16_t i = 0; i < avail && g_title_len < NAV_CALLER_MAX; i++) {
        g_title[g_title_len++] = (char)d[offset + i];
    }
    g_title[g_title_len] = '\0';
    g_title_left -= avail;

    if (g_title_left == 0 && g_call_active) {
        publish_call();
    }
    return BT_GATT_ITER_CONTINUE;
}

/* Where discovery goes once the clock has been dealt with. Notifications come
 * last on purpose: they are the newest thing here, and if the phone will not
 * offer them, music and clock should still finish. */
static enum step step_after_cts(void)
{
    return g_ancs_start ? STEP_ANCS_CHARS : STEP_SUBSCRIBE;
}

static uint8_t on_service(struct bt_conn *conn, const struct bt_gatt_attr *attr,
                          struct bt_gatt_discover_params *params)
{
    (void)conn;
    (void)params;
    if (attr == NULL) {
        logbuf_add("svc=%u ams=%u-%u cts=%u ancs=%u", g_services_seen, g_ams_start, g_ams_end,
                   g_cts_start, g_ancs_start);
        publish_status();
        next_step(g_ams_start ? STEP_AMS_CHARS
                              : (g_cts_start ? STEP_CTS_CHARS : step_after_cts()));
        return BT_GATT_ITER_STOP;
    }
    const struct bt_gatt_service_val *service = attr->user_data;
    g_services_seen++;
    if (!bt_uuid_cmp(service->uuid, UUID_AMS)) {
        g_ams_start = attr->handle + 1;
        g_ams_end = service->end_handle;
    } else if (!bt_uuid_cmp(service->uuid, UUID_CTS)) {
        g_cts_start = attr->handle + 1;
        g_cts_end = service->end_handle;
    } else if (!bt_uuid_cmp(service->uuid, UUID_ANCS)) {
        g_ancs_start = attr->handle + 1;
        g_ancs_end = service->end_handle;
    }
    return BT_GATT_ITER_CONTINUE;
}

static uint8_t on_ams_char(struct bt_conn *conn, const struct bt_gatt_attr *attr,
                           struct bt_gatt_discover_params *params)
{
    (void)conn;
    (void)params;
    if (attr == NULL) {
        logbuf_add("ent=%u/%02x rem=%u/%02x", g_entity_handle, g_entity_props,
                   g_remote_handle, g_remote_props);
        next_step(g_entity_handle ? STEP_CCC_REMOTE
                                  : (g_cts_start ? STEP_CTS_CHARS : step_after_cts()));
        return BT_GATT_ITER_STOP;
    }
    const struct bt_gatt_chrc *chrc = attr->user_data;
    if (!bt_uuid_cmp(chrc->uuid, UUID_AMS_ENTITY)) {
        g_entity_handle = chrc->value_handle;
        g_entity_props = chrc->properties;
    } else if (!bt_uuid_cmp(chrc->uuid, UUID_AMS_REMOTE)) {
        g_remote_handle = chrc->value_handle;
        g_remote_props = chrc->properties;
    }
    return BT_GATT_ITER_CONTINUE;
}

static uint8_t on_cts_char(struct bt_conn *conn, const struct bt_gatt_attr *attr,
                           struct bt_gatt_discover_params *params)
{
    (void)conn;
    (void)params;
    if (attr == NULL) {
        next_step(step_after_cts());
        return BT_GATT_ITER_STOP;
    }
    const struct bt_gatt_chrc *chrc = attr->user_data;
    if (!bt_uuid_cmp(chrc->uuid, UUID_CURRENT_TIME)) {
        g_time_handle = chrc->value_handle;
        g_time_props = chrc->properties;
    }
    return BT_GATT_ITER_CONTINUE;
}

static uint8_t on_ancs_char(struct bt_conn *conn, const struct bt_gatt_attr *attr,
                            struct bt_gatt_discover_params *params)
{
    (void)conn;
    (void)params;
    if (attr == NULL) {
        logbuf_add("ancs ns=%u cp=%u ds=%u", g_notif_handle, g_ctrl_handle, g_data_handle);
        next_step(g_notif_handle ? STEP_CCC_NOTIF : STEP_SUBSCRIBE);
        return BT_GATT_ITER_STOP;
    }
    const struct bt_gatt_chrc *chrc = attr->user_data;
    if (!bt_uuid_cmp(chrc->uuid, UUID_ANCS_NOTIF)) {
        g_notif_handle = chrc->value_handle;
        g_notif_props = chrc->properties;
    } else if (!bt_uuid_cmp(chrc->uuid, UUID_ANCS_CTRL)) {
        g_ctrl_handle = chrc->value_handle;
        g_ctrl_props = chrc->properties;
    } else if (!bt_uuid_cmp(chrc->uuid, UUID_ANCS_DATA)) {
        g_data_handle = chrc->value_handle;
        g_data_props = chrc->properties;
    }
    return BT_GATT_ITER_CONTINUE;
}

static const uint8_t g_ccc_on[] = { 0x01, 0x00 };

/* Handles to try, as an offset from the characteristic's value. "Extended
 * properties" means a read-only 0x2900 descriptor comes first, so the
 * notification descriptor is one further along than the usual layout. */
static uint8_t g_ccc_try;      /* which candidate we are on, 0..2 */
static uint8_t g_ancs_try;     /* the same ladder, for the notification service */
static uint16_t g_ccc_tried;   /* the handle of the attempt in flight */

static uint16_t ccc_candidate(uint16_t value_handle, uint16_t found, uint8_t props, uint8_t attempt)
{
    static const uint8_t with_ext[3] = { 2, 1, 3 };
    static const uint8_t plain[3] = { 1, 2, 3 };

    if (found) {
        return found;
    }
    if (value_handle == 0) {
        return 0;
    }
    const uint8_t *offsets = (props & BT_GATT_CHRC_EXT_PROP) ? with_ext : plain;
    return value_handle + offsets[attempt < 3 ? attempt : 2];
}

static void on_ccc_written(struct bt_conn *conn, uint8_t err, struct bt_gatt_write_params *params)
{
    (void)conn;
    if (err) {
        logbuf_add("ccc %u write err=%02x", params->handle, err);
    }
    if (g_step == STEP_CCC_REMOTE) {
        if (!err) {
            g_remote_ccc = g_ccc_tried;
        }
        next_step(STEP_CCC_ENTITY);
        return;
    }
    /* The notification service uses the same ladder, but a failure there is not
     * allowed to hold up music or the clock: it just means no caller ID. */
    if (g_step == STEP_CCC_NOTIF || g_step == STEP_CCC_DATA) {
        bool on_notif = g_step == STEP_CCC_NOTIF;
        if (!err) {
            if (on_notif) {
                g_notif_ccc = g_ccc_tried;
            } else {
                g_data_ccc = g_ccc_tried;
            }
        } else if (g_ancs_try < 2) {
            g_ancs_try++;
            next_step(g_step); /* the descriptor is the next one along */
            return;
        }
        g_ancs_try = 0;
        next_step(on_notif ? STEP_CCC_DATA : STEP_SUBSCRIBE);
        return;
    }
    g_ccc_err = err; /* the one that matters is Entity Update */
    if (err && g_entity_ccc == 0 && g_ccc_try < 2) {
        g_ccc_try++;
        publish_status();
        next_step(STEP_CCC_ENTITY); /* the descriptor is the next one along */
        return;
    }
    if (!err) {
        g_entity_ccc = g_ccc_tried; /* confirmed by the phone accepting the write */
        logbuf_add("ccc %u ON", g_entity_ccc);
    }
    publish_status();
    next_step(g_cts_start ? STEP_CTS_CHARS : step_after_cts());
}

static int write_ccc(uint16_t handle)
{
    memset(&g_write, 0, sizeof(g_write));
    g_write.func = on_ccc_written;
    g_write.handle = handle;
    g_write.offset = 0;
    g_write.data = g_ccc_on;
    g_write.length = sizeof(g_ccc_on);
    return bt_gatt_write(g_conn, &g_write);
}

static void on_write_done(struct bt_conn *conn, uint8_t err, struct bt_gatt_write_params *params)
{
    (void)conn;
    (void)params;
    g_write_err = err;
    publish_status();
    if (err) {
        logbuf_add("entity write err=%02x", err);
    }
    if (g_step == STEP_WRITE_PLAYER) {
        next_step(STEP_CLEAR_TRACK);
    } else if (g_step == STEP_CLEAR_TRACK) {
        next_step(STEP_WRITE_TRACK);
    } else {
        next_step(STEP_READ_TIME);
    }
}

static uint8_t on_time_read(struct bt_conn *conn, uint8_t err, struct bt_gatt_read_params *params,
                            const void *data, uint16_t length)
{
    (void)conn;
    (void)params;
    if (!err && data != NULL) {
        apply_current_time(data, length);
    }
    next_step(STEP_DONE);
    return BT_GATT_ITER_STOP;
}

/* ---------------- steps ---------------- */

static int start_probe(void)
{
    memset(&g_read, 0, sizeof(g_read));
    g_read.func = on_probe_read;
    g_read.handle_count = 0; /* read by UUID */
    g_read.by_uuid.uuid = UUID_DEVICE_NAME;
    g_read.by_uuid.start_handle = 0x0001;
    g_read.by_uuid.end_handle = 0xFFFF;
    return bt_gatt_read(g_conn, &g_read);
}

static int start_discovery(bt_gatt_discover_func_t func, uint8_t type, uint16_t start, uint16_t end)
{
    memset(&g_discover, 0, sizeof(g_discover));
    g_discover.uuid = NULL; /* list everything and match client-side */
    g_discover.func = func;
    g_discover.start_handle = start;
    g_discover.end_handle = end;
    g_discover.type = type;
    return bt_gatt_discover(g_conn, &g_discover);
}

/* The descriptor we confirmed by writing, or the one the properties point at. */
static void subscribe(struct bt_gatt_subscribe_params *params, uint16_t value_handle,
                      uint16_t ccc_handle, uint8_t props, bt_gatt_notify_func_t notify)
{
    if (value_handle == 0) {
        return;
    }
    params->notify = notify;
    params->value_handle = value_handle;
    params->ccc_handle = ccc_candidate(value_handle, ccc_handle, props, 0);
    params->value = BT_GATT_CCC_NOTIFY;
    int err = bt_gatt_subscribe(g_conn, params);
    if (err && err != -EALREADY) {
        logbuf_add("[apple] subscribe %u failed: %d", value_handle, err);
    }
}

static const uint8_t g_track_request[] = { ENTITY_TRACK, TRACK_ATTR_ARTIST, TRACK_ATTR_TITLE };
static const uint8_t g_player_request[] = { ENTITY_PLAYER, PLAYER_ATTR_PLAYBACK_INFO };
/* An entity on its own, with no attributes, cancels that entity's registration.
 * The phone keeps registrations across reconnects, and only sends an attribute
 * when it changes, so re-registering unchanged leaves the screen blank until
 * the next song. Cancelling first makes the re-registration new again. */
static const uint8_t g_track_clear[] = { ENTITY_TRACK };

/* The Entity Update characteristic takes a write WITH response; a write command
 * is dropped silently, which is why nothing ever arrived. */
static int write_entity_request(const uint8_t *data, uint16_t len)
{
    memset(&g_write, 0, sizeof(g_write));
    g_write.func = on_write_done;
    g_write.handle = g_entity_handle;
    g_write.offset = 0;
    g_write.data = data;
    g_write.length = len;
    return bt_gatt_write(g_conn, &g_write);
}

static uint8_t on_remote_update(struct bt_conn *conn, struct bt_gatt_subscribe_params *params,
                                const void *data, uint16_t length)
{
    (void)conn;
    (void)length;
    if (data == NULL) {
        params->value_handle = 0;
    }
    return data == NULL ? BT_GATT_ITER_STOP : BT_GATT_ITER_CONTINUE;
}

static struct bt_gatt_subscribe_params g_sub_remote;

static void do_subscribe_step(void)
{
    subscribe(&g_sub_remote, g_remote_handle, g_remote_ccc, g_remote_props, on_remote_update);
    subscribe(&g_sub_media, g_entity_handle, g_entity_ccc, g_entity_props, on_media_update);
    subscribe(&g_sub_time, g_time_handle, g_time_ccc, g_time_props, on_time_update);
    /* Data Source first: the phone can answer a request the instant it is made,
     * so be listening before anything is asked for. */
    subscribe(&g_sub_data, g_data_handle, g_data_ccc, g_data_props, on_data_source);
    subscribe(&g_sub_notif, g_notif_handle, g_notif_ccc, g_notif_props, on_notif_source);
    g_subscribed = g_sub_media.value_handle != 0;
    publish_status();
    logbuf_add("subscribed media=%u clock=%u calls=%u", g_entity_handle, g_time_handle,
               g_notif_handle);
    next_step(g_entity_handle ? STEP_WRITE_PLAYER : STEP_READ_TIME);
}

/* ---------------- the notification control point ---------------- */

static struct bt_gatt_write_params g_cp_write;
static uint8_t g_cp_buf[8];

static void on_cp_written(struct bt_conn *conn, uint8_t err, struct bt_gatt_write_params *params)
{
    (void)conn;
    (void)params;
    g_busy = false;
    if (err) {
        /* 0xA0 here means the notification has already gone: the call was
         * answered on the phone, or the caller rang off. */
        logbuf_add("ancs cp err=%02x", err);
    }
}

static int write_control_point(uint8_t len)
{
    memset(&g_cp_write, 0, sizeof(g_cp_write));
    g_cp_write.func = on_cp_written; /* this stack calls it unconditionally */
    g_cp_write.handle = g_ctrl_handle;
    g_cp_write.data = g_cp_buf;
    g_cp_write.length = len;
    return bt_gatt_write(g_conn, &g_cp_write);
}

static void put_uid(uint32_t uid)
{
    g_cp_buf[1] = (uint8_t)(uid);
    g_cp_buf[2] = (uint8_t)(uid >> 8);
    g_cp_buf[3] = (uint8_t)(uid >> 16);
    g_cp_buf[4] = (uint8_t)(uid >> 24);
}

/* Anything the control point is asked to do is queued and sent from the tick,
 * never from inside a notification callback: this stack will not take a second
 * request while one is still finishing. */
static bool service_control_point(void)
{
    if (g_conn == NULL || g_ctrl_handle == 0 || g_busy) {
        return false;
    }
    if (g_pending_action) {
        uint8_t action = g_pending_action;
        g_pending_action = 0;
        if (!g_call_active) {
            return false;
        }
        g_cp_buf[0] = ANCS_CMD_ACTION;
        put_uid(g_call_uid);
        g_cp_buf[5] = action == 1 ? ANCS_ACTION_POSITIVE : ANCS_ACTION_NEGATIVE;
        g_busy = true;
        if (write_control_point(6)) {
            /* Put it back rather than dropping it: the rider pressed answer and
             * is waiting, and the next tick is a few milliseconds away. */
            g_pending_action = action;
            g_busy = false;
            return false;
        }
        logbuf_add("call %s", action == 1 ? "answered" : "declined");
        return true;
    }
    if (g_want_title) {
        g_want_title = false;
        if (!g_call_active) {
            return false;
        }
        g_cp_buf[0] = ANCS_CMD_GET_ATTRS;
        put_uid(g_call_uid);
        g_cp_buf[5] = ANCS_ATTR_TITLE;
        g_cp_buf[6] = NAV_CALLER_MAX;
        g_cp_buf[7] = 0;
        g_busy = true;
        if (write_control_point(8)) {
            g_busy = false;
            return false;
        }
        return true;
    }
    return false;
}

void apple_link_call_action(bool accept)
{
    if (!g_call_active) {
        return;
    }
    g_pending_action = accept ? 1 : 2;
}

void apple_link_tick(uint32_t now_ms)
{
    if (g_conn == NULL || !g_paired) {
        return;
    }
    /* A call can arrive at any point in the sequence, and the answer has to go
     * out while it is still ringing - so this is checked before anything else. */
    if (service_control_point()) {
        return;
    }
    if (g_step == STEP_DONE) {
        if (g_last_step_ms == 0) {
            g_last_step_ms = now_ms; /* start the idle clock, or every tick looks overdue */
            return;
        }
        /* Registered, but no track has ever arrived: ask again. The phone sends
         * a track only when it has one, so a registration made while nothing was
         * playing needs repeating. */
        if (g_entity_handle && g_notif_count > 0 && !g_have_title &&
            now_ms - g_last_step_ms > 10000) {
            next_step(STEP_CLEAR_TRACK); /* cancel, then re-ask, to force a resend */
            return;
        }
        /* Nothing found? iOS sometimes needs a moment after pairing; try again. */
        bool incomplete = (g_entity_handle == 0) || g_notif_count == 0;
        uint32_t wait = g_rounds < MAX_ROUNDS ? 5000 : 20000;
        if (incomplete && now_ms - g_last_step_ms > wait) {
            g_rounds++;
            g_sec_level = (uint8_t)bt_conn_get_security(g_conn);
            g_services_seen = 0;
            g_ams_start = g_cts_start = g_ancs_start = 0;
            g_entity_ccc = g_remote_ccc = g_notif_ccc = g_data_ccc = 0;
            g_ccc_try = 0;
            g_ancs_try = 0;
            g_entity_handle = g_remote_handle = 0;
            g_notif_handle = g_ctrl_handle = g_data_handle = 0;
            /* The subscribe params stay linked in the stack's own list until the
             * peer goes away. Clearing them here unlinks the ones behind them
             * too, and then an arriving notification has nowhere to land. */
            (void)g_rounds;
            next_step(STEP_PROBE);
        }
        return;
    }
    if (g_busy || (g_last_step_ms && now_ms - g_last_step_ms < STEP_INTERVAL_MS)) {
        return;
    }

    g_last_step_ms = now_ms;
    g_busy = true;
    int err = 0;
    switch (g_step) {
        case STEP_PROBE:
            err = start_probe();
            break;
        case STEP_SERVICES:
            err = start_discovery(on_service, BT_GATT_DISCOVER_PRIMARY, 0x0001, 0xFFFF);
            break;
        case STEP_AMS_CHARS:
            err = start_discovery(on_ams_char, BT_GATT_DISCOVER_CHARACTERISTIC, g_ams_start, g_ams_end);
            break;
        case STEP_CCC_REMOTE:
            if (g_remote_handle == 0) {
                g_busy = false;
                next_step(STEP_CCC_ENTITY);
                return;
            }
            g_ccc_tried = ccc_candidate(g_remote_handle, g_remote_ccc, g_remote_props, 0);
            err = write_ccc(g_ccc_tried);
            break;
        case STEP_CCC_ENTITY:
            if (g_entity_handle == 0) {
                g_busy = false;
                next_step(g_cts_start ? STEP_CTS_CHARS : step_after_cts());
                return;
            }
            g_ccc_tried = ccc_candidate(g_entity_handle, g_entity_ccc, g_entity_props, g_ccc_try);
            err = write_ccc(g_ccc_tried);
            break;
        case STEP_CTS_CHARS:
            err = start_discovery(on_cts_char, BT_GATT_DISCOVER_CHARACTERISTIC, g_cts_start, g_cts_end);
            break;
        case STEP_ANCS_CHARS:
            err = start_discovery(on_ancs_char, BT_GATT_DISCOVER_CHARACTERISTIC, g_ancs_start, g_ancs_end);
            break;
        case STEP_CCC_NOTIF:
            g_ccc_tried = ccc_candidate(g_notif_handle, g_notif_ccc, g_notif_props, g_ancs_try);
            err = write_ccc(g_ccc_tried);
            break;
        case STEP_CCC_DATA:
            if (g_data_handle == 0) {
                g_busy = false;
                next_step(STEP_SUBSCRIBE);
                return;
            }
            g_ccc_tried = ccc_candidate(g_data_handle, g_data_ccc, g_data_props, g_ancs_try);
            err = write_ccc(g_ccc_tried);
            break;
        case STEP_CLEAR_TRACK:
            err = write_entity_request(g_track_clear, sizeof(g_track_clear));
            break;
        case STEP_WRITE_TRACK:
            err = write_entity_request(g_track_request, sizeof(g_track_request));
            break;
        case STEP_WRITE_PLAYER:
            err = write_entity_request(g_player_request, sizeof(g_player_request));
            break;
        case STEP_READ_TIME:
            if (g_time_handle == 0) {
                g_busy = false;
                next_step(STEP_DONE);
                return;
            }
            memset(&g_read, 0, sizeof(g_read));
            g_read.func = on_time_read;
            g_read.handle_count = 1;
            g_read.single.handle = g_time_handle;
            g_read.single.offset = 0;
            err = bt_gatt_read(g_conn, &g_read);
            break;
        case STEP_SUBSCRIBE:
            g_busy = false;
            do_subscribe_step();
            return;
        default:
            g_busy = false;
            return;
    }
    if (err) {
        logbuf_add("[apple] step %d failed: %d", (int)g_step, err);
        g_busy = false;          /* try the same step again on the next tick */
        if (err != -EINPROGRESS) {
            g_step = STEP_DONE;  /* hard failure: fall into the retry path */
        }
    }
}

/* ---------------- connection hooks ---------------- */

void apple_link_on_connect(struct bt_conn *conn)
{
    g_conn = conn;
    g_step = STEP_DONE;
    g_busy = false;
    g_rounds = 0;
    g_att_err = 0xFF;
    g_services_seen = 0;
    g_ams_start = g_ams_end = g_cts_start = g_cts_end = 0;
    g_ancs_start = g_ancs_end = 0;
    g_entity_handle = g_remote_handle = g_time_handle = 0;
    g_notif_handle = g_ctrl_handle = g_data_handle = 0;
    g_entity_ccc = g_time_ccc = g_notif_ccc = g_data_ccc = 0;
    g_entity_props = g_remote_props = g_time_props = 0;
    g_notif_props = g_ctrl_props = g_data_props = 0;
    g_ccc_try = 0;
    g_ancs_try = 0;
    g_call_active = false;
    g_want_title = false;
    g_pending_action = 0;
    g_title_len = 0;
    g_title_left = 0;
    nav_state_clear_call();
    g_subscribed = false;
    g_write_err = 0xFF;
    g_notif_count = 0;
    g_sub_lost = false;
    g_have_title = false;
    g_remote_ccc = 0;
    g_ccc_err = 0xFF;
    g_sec_level = 0;
    /* The subscribe params are not cleared here: for a bonded peer the stack
     * keeps them in its own list across a disconnect, and zeroing one unlinks
     * everything behind it. */
    g_paired = false;
    publish_status();

    /* Ask to pair: the phone only shares its clock and media over an encrypted link. */
    int err = bt_conn_set_security(conn, BT_SECURITY_L2);
    if (err) {
        logbuf_add("[apple] security request failed: %d", err);
    }
}

void apple_link_on_security_changed(struct bt_conn *conn, uint8_t level, uint8_t err)
{
    if (err || level < BT_SECURITY_L2 || conn != g_conn) {
        logbuf_add("[apple] not encrypted (level %u, err %u)", level, err);
        g_paired = false;
        publish_status();
        return;
    }
    g_paired = true;
    g_rounds = 0;
    g_sec_level = (uint8_t)bt_conn_get_security(conn);
    publish_status();
    next_step(STEP_PROBE);
}

void apple_link_on_disconnect(void)
{
    g_conn = NULL;
    g_paired = false;
    g_step = STEP_DONE;
    g_busy = false;
    g_entity_handle = g_remote_handle = g_time_handle = 0;
    g_notif_handle = g_ctrl_handle = g_data_handle = 0;
    g_entity_ccc = g_time_ccc = g_notif_ccc = g_data_ccc = 0;
    g_subscribed = false;
    g_services_seen = 0;
    g_att_err = 0xFF;
    g_call_active = false;
    g_want_title = false;
    g_pending_action = 0;
    publish_status();
    nav_state_clear_music();
    nav_state_clear_call();
}

static void on_command_written(struct bt_conn *conn, uint8_t err, struct bt_gatt_write_params *params)
{
    (void)conn;
    (void)params;
    if (err) {
        logbuf_add("cmd err=%02x", err);
    }
}

void apple_link_media_command(uint8_t command)
{
    if (g_conn == NULL || g_remote_handle == 0) {
        return;
    }
    static uint8_t cmd;
    cmd = command;
    if (g_remote_props & BT_GATT_CHRC_WRITE_WITHOUT_RESP) {
        bt_gatt_write_without_response(g_conn, g_remote_handle, &cmd, 1, false);
        return;
    }
    /* Remote Command is write-with-response here (properties 0x98), and this
     * stack calls params->func unconditionally when the reply lands, so the
     * callback is required rather than optional. */
    static struct bt_gatt_write_params params;
    memset(&params, 0, sizeof(params));
    params.func = on_command_written;
    params.handle = g_remote_handle;
    params.data = &cmd;
    params.length = 1;
    bt_gatt_write(g_conn, &params);
}
