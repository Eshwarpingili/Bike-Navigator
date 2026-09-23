#include "ble_nav.h"

#include <stdio.h>
#include <string.h>

#include <FreeRTOS.h>
#include <task.h>

#include "bflb_mtimer.h"
#include "bluetooth.h"
#include "btble_lib_api.h"
#include "conn.h"
#include "conn_internal.h"
#include "gatt.h"
#include "hci_core.h"
#include "hci_driver.h"
#include "uuid.h"

#include "apple_link.h"
#include "health.h"
#include "logbuf.h"
#include "nav_state.h"

/* UUIDs shared with Sygic's BLE HUD, see PROTOCOL.md. */
#define UUID_NAV_SVC   BT_UUID_DECLARE_128(BT_UUID_128_ENCODE(0xDD3F0AD1, 0x6239, 0x4E1F, 0x81F1, 0x91F6C9F01D86))
#define UUID_NAV_IND   BT_UUID_DECLARE_128(BT_UUID_128_ENCODE(0xDD3F0AD2, 0x6239, 0x4E1F, 0x81F1, 0x91F6C9F01D86))
#define UUID_NAV_WRITE BT_UUID_DECLARE_128(BT_UUID_128_ENCODE(0xDD3F0AD3, 0x6239, 0x4E1F, 0x81F1, 0x91F6C9F01D86))

#define KEEPALIVE_MS 4000
#define RX_MAX       128

static struct bt_conn *g_conn;
static volatile bool g_ind_enabled;
static volatile bool g_ind_in_flight;
static volatile uint32_t g_last_activity_ms;

static uint8_t g_rx[RX_MAX];

static uint32_t now_ms(void)
{
    return (uint32_t)bflb_mtimer_get_time_ms();
}

static int on_write(struct bt_conn *conn, const struct bt_gatt_attr *attr, const void *buf,
                    u16_t len, u16_t offset, u8_t flags)
{
    (void)conn;
    (void)attr;
    if (flags & BT_GATT_WRITE_FLAG_PREPARE) {
        return 0; /* long write: chunks arrive again with their offsets on execute */
    }
    if ((uint32_t)offset + len > RX_MAX) {
        return BT_GATT_ERR(BT_ATT_ERR_INVALID_OFFSET);
    }
    memcpy(g_rx + offset, buf, len);

    /* For a long write every chunk is applied in turn; the last one completes the
     * packet, and the UI only redraws after all of them have been handled. */
    uint32_t t = now_ms();
    g_last_activity_ms = t;
    if (!nav_state_apply_packet(g_rx, offset + len, t)) {
        printf("[ble] rejected packet type 0x%02x len %u\r\n", g_rx[0], offset + len);
    }
    return len;
}

static void on_ind_ccc_changed(const struct bt_gatt_attr *attr, u16_t value)
{
    (void)attr;
    g_ind_enabled = (value == BT_GATT_CCC_INDICATE);
}

static struct bt_gatt_attr g_attrs[] = {
    BT_GATT_PRIMARY_SERVICE(UUID_NAV_SVC),
    BT_GATT_CHARACTERISTIC(UUID_NAV_IND, BT_GATT_CHRC_INDICATE, 0, NULL, NULL, NULL),
    BT_GATT_CCC(on_ind_ccc_changed, BT_GATT_PERM_READ | BT_GATT_PERM_WRITE),
    BT_GATT_CHARACTERISTIC(UUID_NAV_WRITE, BT_GATT_CHRC_WRITE | BT_GATT_CHRC_WRITE_WITHOUT_RESP,
                           BT_GATT_PERM_WRITE | BT_GATT_PERM_PREPARE_WRITE, NULL, on_write, NULL),
};
#define ATTR_IND_VALUE 2

static struct bt_gatt_service g_service = BT_GATT_SERVICE(g_attrs);

static void on_indicate_done(struct bt_conn *conn, const struct bt_gatt_attr *attr, u8_t err)
{
    (void)conn;
    (void)attr;
    (void)err;
    g_ind_in_flight = false;
}

static void send_keepalive(void)
{
    static struct bt_gatt_indicate_params params; /* must outlive the call */
    static const uint8_t waiting = 0x01;

    if (g_conn == NULL || !g_ind_enabled || g_ind_in_flight) {
        return;
    }
    params.uuid = NULL;
    params.attr = &g_attrs[ATTR_IND_VALUE];
    params.func = on_indicate_done;
    params.data = &waiting;
    params.len = 1;
    g_ind_in_flight = true;
    if (bt_gatt_indicate(g_conn, &params) != 0) {
        g_ind_in_flight = false;
    }
}

static void on_mtu_exchanged(struct bt_conn *conn, u8_t err, struct bt_gatt_exchange_params *params)
{
    (void)params;
    if (!err) {
        logbuf_add("mtu %u", bt_gatt_get_mtu(conn));
    }
}

static void on_connected(struct bt_conn *conn, u8_t err)
{
    static struct bt_gatt_exchange_params mtu_params;

    if (err || conn->type != BT_CONN_TYPE_LE) {
        return;
    }
    logbuf_add("connected");
    g_conn = conn;
    g_ind_in_flight = false;
    g_last_activity_ms = now_ms();
    nav_state_set_connected(true, g_last_activity_ms);

    mtu_params.func = on_mtu_exchanged;
    bt_gatt_exchange_mtu(conn, &mtu_params);

    /* Ask to pair, so the phone will share its clock and what it is playing. */
    apple_link_on_connect(conn);
}

static void on_security_changed(struct bt_conn *conn, bt_security_t level, enum bt_security_err err)
{
    logbuf_add("security level %d err %d", (int)level, (int)err);
    apple_link_on_security_changed(conn, (uint8_t)level, (uint8_t)err);
}

static void on_disconnected(struct bt_conn *conn, u8_t reason)
{
    if (conn->type != BT_CONN_TYPE_LE) {
        return;
    }
    logbuf_add("disconnected 0x%02x", reason);
    g_conn = NULL;
    g_ind_enabled = false;
    g_ind_in_flight = false;
    apple_link_on_disconnect();
    nav_state_set_connected(false, now_ms());
    if (set_adv_enable(true) != 0) {
        printf("[ble] failed to restart advertising\r\n");
    }
}

static struct bt_conn_cb g_conn_callbacks = {
    .connected = on_connected,
    .disconnected = on_disconnected,
    .security_changed = on_security_changed,
};

/* Service UUID in the advertisement so iOS can scan for it; name in the scan response.
 * File scope, because BT_DATA_BYTES uses compound literals. */
static const struct bt_data g_adv_data[] = {
    BT_DATA_BYTES(BT_DATA_FLAGS, BT_LE_AD_GENERAL | BT_LE_AD_NO_BREDR),
    BT_DATA_BYTES(BT_DATA_UUID128_ALL, 0x86, 0x1D, 0xF0, 0xC9, 0xF6, 0x91, 0xF1, 0x81, 0x1F, 0x4E, 0x39, 0x62,
                  0xD1, 0x0A, 0x3F, 0xDD),
};
static const struct bt_data g_scan_rsp[] = {
    BT_DATA(BT_DATA_NAME_COMPLETE, BIKENAV_DEVICE_NAME, sizeof(BIKENAV_DEVICE_NAME) - 1),
};

static void start_advertising(void)
{
    struct bt_le_adv_param param;

    memset(&param, 0, sizeof(param));
    param.interval_min = BT_GAP_ADV_FAST_INT_MIN_2;
    param.interval_max = BT_GAP_ADV_FAST_INT_MAX_2;
    param.options = BT_LE_ADV_OPT_CONNECTABLE | BT_LE_ADV_OPT_ONE_TIME;

    int err = bt_le_adv_start(&param, g_adv_data, ARRAY_SIZE(g_adv_data), g_scan_rsp, ARRAY_SIZE(g_scan_rsp));
    printf("[ble] advertising as %s: %s (%d)\r\n", BIKENAV_DEVICE_NAME, err ? "FAILED" : "ok", err);
}

static void on_bt_ready(int err)
{
    if (err) {
        printf("[ble] bt_enable failed: %d\r\n", err);
        return;
    }
    bt_set_name(BIKENAV_DEVICE_NAME);
    bt_conn_cb_register(&g_conn_callbacks);
    bt_gatt_service_register(&g_service);
    start_advertising();
}

void ble_nav_task(void *arg)
{
    (void)arg;
    btble_controller_init(configMAX_PRIORITIES - 1);
    hci_driver_init();
    bt_enable(on_bt_ready);

    while (1) {
        vTaskDelay(pdMS_TO_TICKS(1000));
        health_alive(HEALTH_TASK_BLE, now_ms());
        apple_link_tick(now_ms());
        if (g_conn != NULL && now_ms() - g_last_activity_ms > KEEPALIVE_MS) {
            g_last_activity_ms = now_ms();
            send_keepalive();
        }
    }
}
