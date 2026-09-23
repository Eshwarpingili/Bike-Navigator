#ifndef BACKLIGHT_H
#define BACKLIGHT_H

#include <stdint.h>

/* Display brightness.
 *
 * The panel's backlight is switched by GPIO14 through a CJ2301 MOSFET (Q2 on
 * the AiPi-DSL schematic, 1K gate resistor). The gate is pulled up, which is
 * why the backlight is already on before any firmware touches the pin — so
 * driving the pin high keeps it on, and PWM on it dims. */

/* Sets up PWM on the backlight pin and goes to full brightness. */
void backlight_init(void);

/* 5..100 percent. Values outside that are clamped, so a bad caller can dim the
 * screen but can never switch it off and strand the rider. */
void backlight_set(uint8_t percent);

#endif
