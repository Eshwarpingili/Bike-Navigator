#ifndef HEALTH_H
#define HEALTH_H

#include <stdint.h>

/* A hardware watchdog that reboots the board if either task stops running.
 *
 * On a handlebar there is no way to notice a hung program and no way to restart
 * it without stopping the ride, and a frozen screen still showing the last turn
 * is worse than a blank one: it looks correct. So both tasks check in, and only
 * a board where both are alive gets to stay running. */

/* Which task is reporting. */
#define HEALTH_TASK_UI  0
#define HEALTH_TASK_BLE 1
#define HEALTH_TASK_COUNT 2

/* Starts the watchdog. Call once, before the tasks start. */
void health_init(void);

/* "I am still running." Call from inside each task's loop. */
void health_alive(int task, uint32_t now_ms);

/* Feeds the watchdog, but only while every task has checked in recently.
 * Call regularly from one task; if any other task has stopped, this stops
 * feeding and the board reboots. */
void health_feed(uint32_t now_ms);

/* True if the last boot was the watchdog rebooting us rather than a power-up. */
int health_rebooted_by_watchdog(void);

#endif
