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
    DIR_COUNT = 39,
};

#define NAV_FLAG_REROUTING 0x01
#define NAV_FLAG_ARRIVED   0x02
#define NAV_FLAG_GPS_WEAK  0x04

#define NAV_UNKNOWN_U32 0xFFFFFFFFu
#define NAV_UNKNOWN_U16 0xFFFFu
#define NAV_UNKNOWN_U8  0xFFu

#define NAV_STREET_MAX  48
#define NAV_TEXT_MAX    16
#define NAV_MUSIC_MAX   40
#define NAV_CALLER_MAX  28

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
    uint8_t apple_ccc_err;  /* phone's answer to enabling notifications */
    uint8_t apple_sec;      /* link security level */

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

/* Copy the current state out (thread safe). */
void nav_state_snapshot(nav_state_t *out);

#endif
