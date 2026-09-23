#ifndef BLE_NAV_H
#define BLE_NAV_H

#define BIKENAV_DEVICE_NAME "BikeNav"

/* Start the BLE controller + host, register the navigation GATT service and
 * advertise. Runs forever (sends keep-alive indications), so call it as a task. */
void ble_nav_task(void *arg);

#endif
