#ifndef NAV_STATE_H
#define NAV_STATE_H

#include <stdbool.h>
#include <stdint.h>

/* Direction codes, see PROTOCOL.md. */
enum {
    DIR_NONE = 0,
    DIR_START = 1,
    DIR_DESTINATION = 4,
    DIR_VIA = 5,
    DIR_OFF_ROUTE = 9,
    DIR_COUNT = 47,  /* keep in step with gen_arrows.py */
};

#define NAV_FLAG_REROUTING 0x01
#define NAV_FLAG_ARRIVED   0x02
#define NAV_FLAG_GPS_WEAK  0x04

#define NAV_UNKNOWN_U32 0xFFFFFFFFu
#define NAV_UNKNOWN_U16 0xFFFFu
#define NAV_UNKNOWN_U8  0xFFu

/* 0x02 is 16 bytes of fixed fields plus the street, and ble_nav's RX_MAX is
 * 128, so 96 is what fits. 48 was cutting the descriptive instructions -
 * "3rd exit, right onto NH-65" and longer - mid-word, before the label's own
 * ellipsis ever saw them. */
#define NAV_STREET_MAX  96
#define NAV_TEXT_MAX    16
/* Long enough for a real song title. 40 silently cut most of them: the board
 * showed the first 40 bytes for the whole song and the diagnostic still said
 * the music chain was fine. AMS can carry ~241 bytes in one notification at
 * the negotiated MTU, so the only limit here was this number. */
#define NAV_MUSIC_MAX   120
#define NAV_CALLER_MAX  28
/* Route shape ahead of the rider. The phone does the projection - it has the
 * route and the heading and a floating point unit - and sends points already
 * only has to scale them to pixels. 60 points is what fits in one write. */

typedef enum {
    NAV_MODE_IDLE = 0,  /* connected or not, no route */
    NAV_MODE_FULL,      /* 0x02 packets from the BikeNav app */
    NAV_MODE_BASIC,     /* 0x01 packets (Sygic) */
} nav_mode_t;

typedef struct {
    nav_mode_t mode;
    uint8_t direction;
    uint8_t flags;
    uint32_t distance_m;
    uint32_t remaining_m;
    uint16_t minutes_left;
    uint8_t speed_kmh;
    uint8_t limit_kmh;
    uint8_t then_direction;
    char street[NAV_STREET_MAX + 1];
    char distance_text[NAV_TEXT_MAX + 1]; /* basic mode only */

    bool connected;
    bool clock_valid;
    int64_t clock_offset_s;  /* local time = uptime_s + clock_offset_s */

    /* Now playing, from the phone's Apple Media Service */
    char music_title[NAV_MUSIC_MAX + 1];
    char music_artist[NAV_MUSIC_MAX + 1];
    bool music_valid;
    bool music_playing;
    /* The phone's own volume, as it reports it - not what the board asked for. */
    uint8_t volume_percent;
    bool volume_valid;

    /* Incoming call, from the phone's notification service (ANCS). The uid is
     * what an answer or a decline has to quote back to the phone. */
    bool call_ringing;
    char call_name[NAV_CALLER_MAX + 1];
    uint32_t call_uid;
    bool call_can_answer;
    bool call_can_decline;

    /* How far the phone link got, shown on the home screen while diagnosing */
    bool apple_paired;
    bool apple_media;
    bool apple_clock;
    uint8_t apple_services; /* how many services the phone exposed */
    uint8_t apple_att_err;  /* ATT error from the read probe, 0xFF = not tried */
    bool apple_subscribed;  /* notifications enabled on the media service */
    uint16_t apple_entity;  /* media characteristic handle */
    uint16_t apple_ccc;     /* notification descriptor handle in use */
    uint8_t apple_write_err;
    uint16_t apple_notifs;
    bool apple_sub_lost;
    /* Notification service: found at all, and how many calls have arrived.
     * iOS gates this behind a per-device consent, so "found but silent" is a
     * real state and has to be told apart from "not found". */
    bool apple_ancs;
    uint16_t apple_calls;
    uint8_t apple_ccc_err;  /* phone's answer to enabling notifications */
    uint8_t apple_sec;      /* link security level */
    /* Why the music chain is not working. A rising apple_rounds means discovery
     * keeps aborting and starting over; apple_fail_step names the step that
     * failed. Both are on the settings screen, because the song name going
     * missing has looked identical to four different faults. */
    uint8_t apple_rounds;
    uint8_t apple_fail_step; /* 0xFF = nothing has failed */
    int8_t apple_fail_err;
    bool apple_have_title;   /* the phone has sent a title at least once */
    bool apple_trunc;        /* AMS said it cut the value short */
    uint16_t apple_mtu;      /* what the link actually negotiated */


    uint32_t last_packet_ms; /* uptime of the last navigation packet (link health) */
    uint32_t last_change_ms; /* uptime of the last packet that said something new */
    uint32_t version;        /* bumps on every change, so the UI can skip redraws */
} nav_state_t;

void nav_state_init(void);

/* Parse one write from the phone. Returns false if the packet was rejected. */
bool nav_state_apply_packet(const uint8_t *data, uint16_t len, uint32_t now_ms);

void nav_state_set_connected(bool connected, uint32_t now_ms);

/* Now playing (title when is_title, else artist); text need not be NUL terminated. */
void nav_state_set_music_text(bool is_title, const char *text, uint16_t len);
void nav_state_set_music_playing(bool playing);
void nav_state_set_volume(uint8_t percent);
void nav_state_clear_music(void);

/* A call is ringing. name may be NULL until the phone sends the caller's. */
void nav_state_set_call(uint32_t uid, const char *name, uint16_t len,
                        bool can_answer, bool can_decline);
void nav_state_clear_call(void);

/* Set the clock from the phone, as seconds since local midnight. */
void nav_state_set_local_time(uint32_t seconds_of_day);

/* Progress of the phone link: bonded, media service found, clock service found. */
void nav_state_set_apple_status(bool paired, bool media, bool clock, uint8_t services,
                                uint8_t att_err, bool subscribed);

/* Extra numbers shown while diagnosing the phone link. */
void nav_state_set_apple_debug(uint16_t entity, uint16_t ccc, uint8_t write_err,
                               uint16_t notifs, bool sub_lost, uint8_t ccc_err, uint8_t sec);

/* Notification service state, for telling a missing service from a muted one. */
void nav_state_set_ancs(bool found, uint16_t calls);

/* How the music chain is getting on, in the few numbers that identify a fault. */
void nav_state_set_apple_chain(uint8_t rounds, uint8_t fail_step, int8_t fail_err,
                               bool have_title, bool truncated);

/* What the link negotiated. A small MTU caps what AMS can send in one go, and
 * the boot log that used to carry this is only on the diagnostics screen,
 * which never shows while the link is working. */
void nav_state_set_mtu(uint16_t mtu);

/* Copy the current state out (thread safe). */
void nav_state_snapshot(nav_state_t *out);

#endif
