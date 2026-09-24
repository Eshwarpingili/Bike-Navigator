#include "backlight.h"

#include "bflb_clock.h"
#include "bflb_gpio.h"
#include "bflb_pwm_v2.h"

/* Backlight gate, from the AiPi-DSL schematic (page 1, Q2/R28). */
#define BL_PIN     GPIO_PIN_14
/* On this chip a pin's PWM channel is its number modulo four. */
#define BL_CHANNEL (14 % 4)

/* 40 MHz / 40 / 1000 = 1 kHz: well above anything the eye can see, and well
 * below the frequency where the MOSFET's switching losses would matter. */
#define PWM_DIV    40
#define PWM_PERIOD 1000

#define BL_MIN_PERCENT 5

static struct bflb_device_s *g_pwm;
static uint8_t g_percent;

void backlight_init(void)
{
    struct bflb_pwm_v2_config_s cfg = {
        .clk_source = BFLB_SYSTEM_XCLK,
        .clk_div = PWM_DIV,
        .period = PWM_PERIOD,
    };
    struct bflb_device_s *gpio = bflb_device_get_by_name("gpio");

    if (gpio == NULL) {
        return;
    }
    /* Pull up as well as drive: if the PWM is ever stopped the gate floats back
     * to on rather than leaving the rider with a black screen. */
    bflb_gpio_init(gpio, BL_PIN, GPIO_FUNC_PWM0 | GPIO_ALTERNATE | GPIO_PULLUP | GPIO_SMT_EN | GPIO_DRV_1);

    g_pwm = bflb_device_get_by_name("pwm_v2_0");
    if (g_pwm == NULL) {
        return;
    }
    bflb_pwm_v2_init(g_pwm, &cfg);
    bflb_pwm_v2_channel_positive_start(g_pwm, BL_CHANNEL);
    bflb_pwm_v2_start(g_pwm);
    backlight_set(100);
}

/* Perceived brightness to PWM duty, in thousandths.
 *
 * Duty is linear in light output; the eye is not. Driving the backlight at 20%
 * duty looks about half brightness rather than a fifth, so a percentage that
 * means duty tells the rider something untrue. This is the CIE 1931 lightness
 * curve, which is what "50%" has to mean for it to look like half.
 *
 * Integer throughout: (100+16)^3 * 1000 is about 1.56e9, which still fits in
 * 32 bits, and the PWM period is 1000, so the result is the threshold itself. */
static uint16_t duty_for(uint8_t perceived)
{
    if (perceived <= 8) {
        /* The curve's linear foot, below which the cube rounds away to nothing. */
        return (uint16_t)(((uint32_t)perceived * PWM_PERIOD) / 903u);
    }
    uint32_t l = (uint32_t)perceived + 16u;
    return (uint16_t)((l * l * l * PWM_PERIOD) / 1560896u); /* 1560896 = 116^3 */
}

void backlight_set(uint8_t percent)
{
    if (g_pwm == NULL) {
        return;
    }
    if (percent < BL_MIN_PERCENT) {
        percent = BL_MIN_PERCENT;
    } else if (percent > 100) {
        percent = 100;
    }
    if (percent == g_percent) {
        return;
    }
    g_percent = percent;
    bflb_pwm_v2_channel_set_threshold(g_pwm, BL_CHANNEL, 0, duty_for(percent));
}
