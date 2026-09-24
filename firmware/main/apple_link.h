#ifndef APPLE_LINK_H
#define APPLE_LINK_H

#include <stdbool.h>
#include <stdint.h>

struct bt_conn;

/* Talks to three services the iPhone itself provides to any paired BLE
 * accessory: Apple Media Service (what is playing), Current Time Service (the
 * clock) and the Apple Notification Center Service (who is calling). No app on
 * the phone is involved. All three need a bonded, encrypted link. */

/* AMS RemoteCommandID values. */
enum {
    MEDIA_CMD_PLAY = 0,
    MEDIA_CMD_PAUSE = 1,
    MEDIA_CMD_TOGGLE = 2,
    MEDIA_CMD_NEXT = 3,
    MEDIA_CMD_PREVIOUS = 4,
    /* The phone's own volume, so the rider never has to dig it out of a pocket. */
    MEDIA_CMD_VOLUME_UP = 5,
    MEDIA_CMD_VOLUME_DOWN = 6,
};

void apple_link_on_connect(struct bt_conn *conn);

/* Call about once a second: retries discovery if the phone was not ready yet. */
void apple_link_tick(uint32_t now_ms);
void apple_link_on_security_changed(struct bt_conn *conn, uint8_t level, uint8_t err);
void apple_link_on_disconnect(void);

/* Send a play/pause/next command to the phone. Ignored if not connected. */
void apple_link_media_command(uint8_t command);

/* Answer or decline the call that is ringing. Ignored if none is. */
void apple_link_call_action(bool accept);

#endif
