#include "prefs.h"

#include "easyflash.h"

#define KEY_ROTATION   "bikenav_rot"
#define KEY_BRIGHTNESS "bikenav_bl"

static uint8_t g_rotation;
static uint8_t g_brightness;

static uint8_t load_u8(const char *key, uint8_t fallback)
{
    uint8_t v = 0;
    size_t got = 0;
    ef_get_env_blob(key, &v, sizeof(v), &got);
    return got == sizeof(v) ? v : fallback;
}

void prefs_init(void)
{
    g_rotation = load_u8(KEY_ROTATION, 0);
    g_brightness = load_u8(KEY_BRIGHTNESS, BRIGHTNESS_AUTO);
}

uint8_t prefs_rotation(void)
{
    return g_rotation;
}

void prefs_set_rotation(uint8_t rot)
{
    g_rotation = rot;
    ef_set_env_blob(KEY_ROTATION, &rot, sizeof(rot));
}

uint8_t prefs_brightness(void)
{
    return g_brightness;
}

void prefs_set_brightness(uint8_t percent)
{
    g_brightness = percent;
    ef_set_env_blob(KEY_BRIGHTNESS, &percent, sizeof(percent));
}
