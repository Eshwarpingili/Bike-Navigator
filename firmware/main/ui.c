#include "ui.h"

#include <stdio.h>
#include <string.h>

#include "apple_link.h"
#include "arrows.h"
#include "backlight.h"
#include "logbuf.h"
#include "lvgl.h"
#include "prefs.h"

#define STALE_MS 8000
/* Sygic never says "route finished", so drop back to the home screen once the
 * instruction has stopped changing (3 min: long enough for a traffic light). */
#define HOME_AFTER_MS 180000
#define HOME_AFTER_DISCONNECT_MS 15000
/* How long the phone link must stay down before the log takes the screen. */
#define DIAG_AFTER_MS 25000

/* Daylight hours, when the screen is fighting the sun and wants a light face. */
#define LIGHT_FROM_MIN (7 * 60)
#define LIGHT_TO_MIN   (19 * 60)

/* Two palettes, chosen by the phone's clock. Both are high contrast: this gets
 * read at a glance, in motion, over a shoulder. */
typedef struct {
    lv_color_t bg, card, text, dim, accent, warn, good, bar;
} palette_t;

static palette_t g_pal;

static void set_palette(bool light)
{
    if (light) {
        g_pal.bg = lv_color_hex(0xEEF1F5);
        g_pal.card = lv_color_hex(0xFFFFFF);
        g_pal.text = lv_color_hex(0x05070A);
        g_pal.dim = lv_color_hex(0x49515C);
        g_pal.accent = lv_color_hex(0x0A5AD0);
        g_pal.warn = lv_color_hex(0xA65200);
        g_pal.good = lv_color_hex(0x0A7A3C);
        g_pal.bar = lv_color_hex(0xDFE4EB);
    } else {
        g_pal.bg = lv_color_hex(0x000000);
        g_pal.card = lv_color_hex(0x161A21);
        g_pal.text = lv_color_hex(0xFFFFFF);
        g_pal.dim = lv_color_hex(0xAAB3BF);
        g_pal.accent = lv_color_hex(0x5AB0FF);
        g_pal.warn = lv_color_hex(0xFFB300);
        g_pal.good = lv_color_hex(0x3DDC84);
        g_pal.bar = lv_color_hex(0x161A21);
    }
}

#define COL_TEXT   (g_pal.text)
#define COL_DIM    (g_pal.dim)
#define COL_AMBER  (g_pal.warn)
#define COL_GREEN  (g_pal.good)
#define COL_RED    lv_color_hex(0xE53935)
#define COL_BAR    (g_pal.bar)
#define COL_ACCENT (g_pal.accent)

/* Which face is showing. A ringing phone and a broken link take the screen on
 * their own account, so they are not choices in here - they override at draw
 * time and hand it back afterwards. */
typedef enum {
    SCR_HOME = 0,
    SCR_NAV,
    SCR_MUSIC,
    SCR_SETTINGS,
    SCR_MAP,
} screen_t;

/* Manual brightness moves in these steps. Nothing below BL_MIN_PERCENT, so the
 * screen can always be read. */
#define BL_STEP 20
#define BL_LOWEST 20
/* Day and night levels when brightness is left on automatic. */
#define BL_DAY 100
#define BL_NIGHT 40

static struct {
    bool landscape;
    const char *device_name;

    lv_obj_t *status;   /* top-left: connection / warnings */
    lv_obj_t *speed;    /* top-centre */
    lv_obj_t *clock;    /* top-right */

    lv_obj_t *nav;      /* container for everything route related */
    lv_obj_t *arrow;
    lv_obj_t *dist_num;
    lv_obj_t *dist_unit;
    lv_obj_t *street;
    lv_obj_t *then_label;
    lv_obj_t *then_arrow;
    lv_obj_t *limit;    /* speed-limit badge */
    lv_obj_t *limit_num;
    lv_obj_t *bar;
    lv_obj_t *bar_left;
    lv_obj_t *bar_mid;
    lv_obj_t *bar_right;

    lv_obj_t *idle;        /* home screen: four tiles */
    lv_obj_t *tile_nav;    /* tap for the full navigation screen */
    lv_obj_t *home_arrow;
    lv_obj_t *home_dist;
    lv_obj_t *home_street;
    lv_obj_t *home_none;   /* "No route", so the tile is never blank */
    lv_obj_t *tile_media;  /* tap for the full music screen */
    lv_obj_t *track;
    lv_obj_t *artist;
    lv_obj_t *idle_msg;    /* on the music tile, when nothing is playing */
    lv_obj_t *tile_clock;
    lv_obj_t *idle_clock;
    lv_obj_t *tile_map;    /* replaces the clock while a route is running */
    lv_obj_t *tile_set;    /* tap for brightness and volume */
    lv_obj_t *tile_set_val;

    lv_obj_t *music;       /* full screen: now playing, transport, volume */
    lv_obj_t *m_back;
    lv_obj_t *m_title;
    lv_obj_t *m_artist;
    lv_obj_t *m_prev;
    lv_obj_t *m_play;
    lv_obj_t *m_next;
    lv_obj_t *m_vol_down;
    lv_obj_t *m_vol_val;
    lv_obj_t *m_vol_up;

    lv_obj_t *settings;    /* full screen: brightness and volume */
    lv_obj_t *s_back;
    lv_obj_t *s_tap;       /* where the last tap landed, for diagnosing touch */
    lv_obj_t *s_bl_down;
    lv_obj_t *s_bl_val;
    lv_obj_t *s_bl_up;
    lv_obj_t *s_vol_down;
    lv_obj_t *s_vol_val;
    lv_obj_t *s_vol_up;
    lv_obj_t *s_auto;

    lv_obj_t *map;         /* the route ahead, drawn heading-up */
    lv_obj_t *map_back;
    lv_obj_t *map_line;
    lv_obj_t *map_me;      /* the rider, always at the same spot: this is a
                            * heading-up view, so the rider does not move */
    lv_obj_t *map_scale;

    lv_obj_t *call;        /* takes the whole screen while the phone rings */
    lv_obj_t *call_who;
    lv_obj_t *call_answer;
    lv_obj_t *call_decline;

    lv_obj_t *diag;        /* only when the phone link is not working */
    lv_obj_t *link_state;
    lv_obj_t *link_state2;
    lv_obj_t *log_label;   /* firmware log, the board's only console */
    unsigned drawn_log;

    screen_t screen;
    bool drawn_ringing;    /* so a call can restore whatever was on screen */
    screen_t before_call;
    uint32_t drawn_version;
    int drawn_minute;
    bool drawn_stale;
    bool drawn_navigating;
    bool drawn_light;
    uint8_t drawn_backlight;
    lv_coord_t last_x, last_y; /* the last tap, shown on the settings screen */
    uint32_t now_ms;
} ui;

static lv_obj_t *label(lv_obj_t *parent, const lv_font_t *font, lv_color_t color)
{
    lv_obj_t *l = lv_label_create(parent);
    lv_obj_set_style_text_font(l, font, 0);
    lv_obj_set_style_text_color(l, color, 0);
    lv_label_set_text(l, "");
    return l;
}

static lv_obj_t *box(lv_obj_t *parent, lv_coord_t w, lv_coord_t h)
{
    lv_obj_t *o = lv_obj_create(parent);
    lv_obj_remove_style_all(o);
    lv_obj_set_size(o, w, h);
    lv_obj_clear_flag(o, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    return o;
}

static lv_obj_t *tinted_img(lv_obj_t *parent, lv_color_t color)
{
    lv_obj_t *img = lv_img_create(parent);
    lv_obj_set_style_img_recolor(img, color, 0);
    lv_obj_set_style_img_recolor_opa(img, LV_OPA_COVER, 0);
    return img;
}

/* A rounded panel that lifts its contents off the background. */
static lv_obj_t *card(lv_obj_t *parent, lv_coord_t w, lv_coord_t h)
{
    lv_obj_t *o = box(parent, w, h);
    lv_obj_set_style_bg_color(o, g_pal.card, 0);
    lv_obj_set_style_bg_opa(o, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(o, 12, 0);
    return o;
}

/* A card with one symbol centred in it, which is what every control here is.
 * Hit testing reads the card's own coordinates, so nothing is hard-coded. */
static lv_obj_t *button(lv_obj_t *parent, lv_coord_t w, lv_coord_t h,
                        const char *symbol, const lv_font_t *font)
{
    lv_obj_t *b = card(parent, w, h);
    lv_obj_t *l = label(b, font, g_pal.text);
    lv_label_set_text(l, symbol);
    lv_obj_center(l);
    return b;
}

static lv_obj_t *button_label(lv_obj_t *b)
{
    return b ? lv_obj_get_child(b, 0) : NULL;
}

/* Every control's hit area is this much larger than the control, because this
 * is aimed at with a thumb, on a bike, without looking. Two controls must
 * therefore sit at least twice this far apart, or their hit areas overlap and
 * a tap in between lands on whichever happens to be tested first. */
#define HIT_PAD 6
/* Two pixels more than twice the pad, not exactly twice: at exactly twice, the
 * two padded areas share a boundary and a tap on that line is still ambiguous. */
#define MIN_GAP (2 * HIT_PAD + 2)

static void padded_area(lv_obj_t *o, lv_area_t *a)
{
    lv_obj_get_coords(o, a);
    a->x1 -= HIT_PAD;
    a->y1 -= HIT_PAD;
    a->x2 += HIT_PAD;
    a->y2 += HIT_PAD;
}

static bool hit(lv_obj_t *o, lv_coord_t x, lv_coord_t y)
{
    if (o == NULL || lv_obj_has_flag(o, LV_OBJ_FLAG_HIDDEN)) {
        return false;
    }
    lv_area_t a;
    padded_area(o, &a);
    return x >= a.x1 && x <= a.x2 && y >= a.y1 && y <= a.y2;
}

/* Says so on the log screen if any two controls on a screen have hit areas that
 * overlap. Spacing worked out by hand is exactly the kind of thing that drifts
 * the next time a layout is touched, and the symptom - a button that sometimes
 * does its neighbour's job - is miserable to diagnose from the saddle. */
static void check_spacing(const char *screen, lv_obj_t **controls, int count)
{
    for (int i = 0; i < count; i++) {
        for (int j = i + 1; j < count; j++) {
            lv_area_t a, b;
            padded_area(controls[i], &a);
            padded_area(controls[j], &b);
            if (a.x1 <= b.x2 && b.x1 <= a.x2 && a.y1 <= b.y2 && b.y1 <= a.y2) {
                logbuf_add("LAYOUT %s %d/%d overlap", screen, i, j);
            }
        }
    }
}

/* Backlight: the rider's setting if there is one, otherwise bright by day and
 * dim at night. Applied whenever either of those could have changed. */
static void apply_backlight(bool light)
{
    uint8_t want = prefs_brightness();
    if (want == BRIGHTNESS_AUTO) {
        want = light ? BL_DAY : BL_NIGHT;
    }
    if (want != ui.drawn_backlight) {
        ui.drawn_backlight = want;
        backlight_set(want);
    }
}

/* Re-colour everything when the day/night palette changes. */
static void apply_theme(void)
{
    lv_obj_t *on_text[] = { ui.speed, ui.clock, ui.dist_num, ui.dist_unit, ui.street,
                            ui.bar_left, ui.bar_mid, ui.idle_clock, ui.track,
                            ui.home_dist, ui.m_title, ui.s_bl_val, ui.call_who,
                            button_label(ui.m_prev), button_label(ui.m_play),
                            button_label(ui.m_next), button_label(ui.m_vol_down),
                            button_label(ui.m_vol_up), button_label(ui.s_bl_down),
                            button_label(ui.s_bl_up), button_label(ui.s_vol_down),
                            button_label(ui.s_vol_up), button_label(ui.m_back),
                            button_label(ui.s_back), button_label(ui.s_auto),
                            ui.m_vol_val, ui.s_vol_val, button_label(ui.map_back) };
    lv_obj_t *on_dim[] = { ui.status, ui.then_label, ui.artist, ui.idle_msg, ui.home_street,
                           ui.home_none, ui.m_artist, ui.tile_set_val, ui.s_tap,
                           ui.map_scale, ui.link_state, ui.link_state2, ui.log_label };
    /* The tiles and every control share the card colour. The call buttons do
     * not: green and red are the whole point of them. */
    lv_obj_t *on_card[] = { ui.tile_nav, ui.tile_media, ui.tile_clock, ui.tile_set,
                            ui.m_prev, ui.m_play, ui.m_next, ui.m_vol_down, ui.m_vol_up,
                            ui.s_bl_down, ui.s_bl_up, ui.s_vol_down, ui.s_vol_up,
                            ui.m_back, ui.s_back, ui.s_auto, ui.map_back };

    lv_obj_set_style_bg_color(lv_scr_act(), g_pal.bg, 0);
    for (unsigned i = 0; i < sizeof(on_card) / sizeof(on_card[0]); i++) {
        if (on_card[i]) {
            lv_obj_set_style_bg_color(on_card[i], g_pal.card, 0);
        }
    }
    lv_obj_set_style_bg_color(ui.bar, g_pal.bar, 0);
    /* The call buttons follow the palette's green and red, and keep white
     * glyphs on both: the daytime green is dark enough that the ordinary text
     * colour would be unreadable on it. */
    lv_obj_set_style_bg_color(ui.call_answer, g_pal.good, 0);
    lv_obj_set_style_bg_color(ui.call_decline, COL_RED, 0);
    lv_obj_set_style_text_color(button_label(ui.call_answer), lv_color_white(), 0);
    lv_obj_set_style_text_color(button_label(ui.call_decline), lv_color_white(), 0);

    for (unsigned i = 0; i < sizeof(on_text) / sizeof(on_text[0]); i++) {
        if (on_text[i]) {
            lv_obj_set_style_text_color(on_text[i], g_pal.text, 0);
        }
    }
    for (unsigned i = 0; i < sizeof(on_dim) / sizeof(on_dim[0]); i++) {
        if (on_dim[i]) {
            lv_obj_set_style_text_color(on_dim[i], g_pal.dim, 0);
        }
    }
    lv_obj_set_style_text_color(ui.bar_right, g_pal.good, 0);
    lv_obj_set_style_img_recolor(ui.home_arrow, g_pal.accent, 0);
    lv_obj_set_style_img_recolor(ui.then_arrow, g_pal.dim, 0);
}

void ui_init(const char *device_name)
{
    lv_obj_t *scr = lv_scr_act();
    lv_coord_t W = lv_disp_get_hor_res(NULL);
    lv_coord_t H = lv_disp_get_ver_res(NULL);

    memset(&ui, 0, sizeof(ui));
    ui.device_name = device_name;
    ui.landscape = W > H;
    ui.drawn_version = UINT32_MAX;
    ui.drawn_minute = -1;
    ui.drawn_light = false;
    set_palette(false); /* dark until the phone tells us the time */

    lv_obj_set_style_bg_color(scr, g_pal.bg, 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
    lv_obj_clear_flag(scr, LV_OBJ_FLAG_SCROLLABLE);

    /* Top bar */
    ui.status = label(scr, &lv_font_montserrat_16, COL_DIM);
    lv_obj_align(ui.status, LV_ALIGN_TOP_LEFT, 8, 4);
    ui.speed = label(scr, &lv_font_montserrat_16, COL_TEXT);
    lv_obj_align(ui.speed, LV_ALIGN_TOP_MID, ui.landscape ? 20 : 0, 4);
    ui.clock = label(scr, &lv_font_montserrat_16, COL_TEXT);
    lv_obj_align(ui.clock, LV_ALIGN_TOP_RIGHT, -8, 4);

    /* Navigation view */
    ui.nav = box(scr, W, H - 26);
    lv_obj_align(ui.nav, LV_ALIGN_TOP_LEFT, 0, 26);

    ui.arrow = tinted_img(ui.nav, COL_TEXT);
    ui.dist_num = label(ui.nav, &lv_font_montserrat_48, COL_TEXT);
    ui.dist_unit = label(ui.nav, &lv_font_montserrat_20, COL_TEXT);
    ui.street = label(ui.nav, &lv_font_montserrat_20, COL_TEXT);
    lv_label_set_long_mode(ui.street, LV_LABEL_LONG_DOT);
    ui.then_label = label(ui.nav, &lv_font_montserrat_16, COL_DIM);
    lv_label_set_text(ui.then_label, "then");
    ui.then_arrow = tinted_img(ui.nav, COL_DIM);

    if (ui.landscape) {
        lv_obj_align(ui.arrow, LV_ALIGN_TOP_LEFT, 6, 8);
        lv_obj_align(ui.dist_num, LV_ALIGN_TOP_LEFT, 148, 8);
        lv_obj_set_size(ui.street, W - 156, 50);
        lv_obj_align(ui.street, LV_ALIGN_TOP_LEFT, 150, 68);
        lv_obj_align(ui.then_label, LV_ALIGN_TOP_LEFT, 150, 132);
    } else {
        lv_obj_align(ui.arrow, LV_ALIGN_TOP_MID, 0, 4);
        lv_obj_align(ui.dist_num, LV_ALIGN_TOP_MID, -14, 140);
        lv_obj_set_size(ui.street, W - 16, 50);
        lv_obj_set_style_text_align(ui.street, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_align(ui.street, LV_ALIGN_TOP_MID, 0, 196);
        lv_obj_add_flag(ui.then_label, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(ui.then_arrow, LV_OBJ_FLAG_HIDDEN);
    }

    /* Speed-limit badge: red ring with the number inside. */
    ui.limit = lv_obj_create(ui.nav);
    lv_obj_remove_style_all(ui.limit);
    lv_obj_set_size(ui.limit, 40, 40);
    lv_obj_clear_flag(ui.limit, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_radius(ui.limit, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(ui.limit, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(ui.limit, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(ui.limit, COL_RED, 0);
    lv_obj_set_style_border_width(ui.limit, 5, 0);
    lv_obj_align(ui.limit, ui.landscape ? LV_ALIGN_TOP_RIGHT : LV_ALIGN_TOP_LEFT, ui.landscape ? -6 : 8, ui.landscape ? 128 : 8);
    ui.limit_num = label(ui.limit, &lv_font_montserrat_16, lv_color_black());
    lv_obj_center(ui.limit_num);

    /* Bottom bar: remaining distance, time left, arrival time. */
    ui.bar = box(ui.nav, W, 42);
    lv_obj_set_style_bg_color(ui.bar, COL_BAR, 0);
    lv_obj_set_style_bg_opa(ui.bar, LV_OPA_COVER, 0);
    lv_obj_align(ui.bar, LV_ALIGN_BOTTOM_MID, 0, 0);
    ui.bar_left = label(ui.bar, &lv_font_montserrat_20, COL_TEXT);
    lv_obj_align(ui.bar_left, LV_ALIGN_LEFT_MID, 8, 0);
    ui.bar_mid = label(ui.bar, &lv_font_montserrat_20, COL_TEXT);
    lv_obj_align(ui.bar_mid, LV_ALIGN_CENTER, 0, 0);
    ui.bar_right = label(ui.bar, &lv_font_montserrat_20, COL_GREEN);
    lv_obj_align(ui.bar_right, LV_ALIGN_RIGHT_MID, -8, 0);

    /* Home screen: four tiles. Route and music are the two things looked at
     * while moving, the clock is what gets glanced at at a light, and settings
     * is the one that can afford to be hunted for. Tapping a tile opens it. */
    ui.idle = box(scr, W, H - 26);
    lv_obj_align(ui.idle, LV_ALIGN_TOP_LEFT, 0, 26);

    /* Gaps of MIN_GAP, so no two tiles' hit areas can reach each other. */
    lv_coord_t tw = (W - 3 * MIN_GAP) / 2;
    lv_coord_t th = (H - 26 - 3 * MIN_GAP) / 2;
    lv_coord_t col2 = MIN_GAP + tw + MIN_GAP;
    lv_coord_t row2 = MIN_GAP + th + MIN_GAP;

    ui.tile_nav = card(ui.idle, tw, th);
    lv_obj_align(ui.tile_nav, LV_ALIGN_TOP_LEFT, MIN_GAP, MIN_GAP);

    ui.home_arrow = tinted_img(ui.tile_nav, COL_ACCENT);
    lv_img_set_zoom(ui.home_arrow, 128); /* the nav-screen arrows at half size */
    lv_obj_align(ui.home_arrow, LV_ALIGN_LEFT_MID, -20, -8);

    ui.home_dist = label(ui.tile_nav, &lv_font_montserrat_28, COL_TEXT);
    lv_obj_align(ui.home_dist, LV_ALIGN_TOP_RIGHT, -8, 10);

    ui.home_street = label(ui.tile_nav, &lv_font_montserrat_16, COL_DIM);
    lv_obj_set_width(ui.home_street, tw - 12);
    lv_obj_set_style_text_align(ui.home_street, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_long_mode(ui.home_street, LV_LABEL_LONG_DOT);
    lv_obj_align(ui.home_street, LV_ALIGN_BOTTOM_MID, 0, -8);

    ui.home_none = label(ui.tile_nav, &lv_font_montserrat_20, COL_DIM);
    lv_label_set_text(ui.home_none, "No route");
    lv_obj_center(ui.home_none);

    ui.tile_media = card(ui.idle, tw, th);
    lv_obj_align(ui.tile_media, LV_ALIGN_TOP_LEFT, col2, MIN_GAP);

    ui.track = label(ui.tile_media, &lv_font_montserrat_16, COL_TEXT);
    lv_obj_set_width(ui.track, tw - 12);
    lv_obj_set_style_text_align(ui.track, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_long_mode(ui.track, LV_LABEL_LONG_SCROLL_CIRCULAR);
    lv_obj_align(ui.track, LV_ALIGN_TOP_MID, 0, 14);

    ui.artist = label(ui.tile_media, &lv_font_montserrat_16, COL_DIM);
    lv_obj_set_width(ui.artist, tw - 12);
    lv_obj_set_style_text_align(ui.artist, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_long_mode(ui.artist, LV_LABEL_LONG_DOT);
    lv_obj_align(ui.artist, LV_ALIGN_TOP_MID, 0, 40);

    /* Short text only: the tile has no room for advice, and the diagnostics
     * screen already exists for that. */
    ui.idle_msg = label(ui.tile_media, &lv_font_montserrat_16, COL_DIM);
    lv_obj_set_width(ui.idle_msg, tw - 12);
    lv_obj_set_style_text_align(ui.idle_msg, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_long_mode(ui.idle_msg, LV_LABEL_LONG_WRAP);
    lv_obj_center(ui.idle_msg);

    /* Clock when there is nothing to navigate, the way in to the map when there
     * is. The time is already in the top bar, so this tile is better spent on
     * whichever of the two is actually useful at the time. */
    ui.tile_clock = card(ui.idle, tw, th);
    lv_obj_align(ui.tile_clock, LV_ALIGN_TOP_LEFT, MIN_GAP, row2);
    ui.idle_clock = label(ui.tile_clock, &lv_font_montserrat_48, COL_TEXT);
    lv_obj_center(ui.idle_clock);
    ui.tile_map = label(ui.tile_clock, &lv_font_montserrat_20, COL_ACCENT);
    lv_label_set_text(ui.tile_map, LV_SYMBOL_GPS " Map");
    lv_obj_center(ui.tile_map);

    ui.tile_set = card(ui.idle, tw, th);
    lv_obj_align(ui.tile_set, LV_ALIGN_TOP_LEFT, col2, row2);
    lv_obj_t *set_icon = label(ui.tile_set, &lv_font_montserrat_28, COL_DIM);
    lv_label_set_text(set_icon, LV_SYMBOL_SETTINGS);
    lv_obj_align(set_icon, LV_ALIGN_CENTER, 0, -14);
    ui.tile_set_val = label(ui.tile_set, &lv_font_montserrat_16, COL_DIM);
    lv_obj_align(ui.tile_set_val, LV_ALIGN_CENTER, 0, 22);

    /* Full-screen music: the same controls, big enough for a gloved thumb,
     * plus the phone's volume so it never has to come out of a pocket. */
    ui.music = box(scr, W, H - 26);
    lv_obj_align(ui.music, LV_ALIGN_TOP_LEFT, 0, 26);

    ui.m_back = button(ui.music, 58, 34, LV_SYMBOL_LEFT, &lv_font_montserrat_20);
    lv_obj_align(ui.m_back, LV_ALIGN_TOP_LEFT, MIN_GAP, 2);

    /* Narrowed to clear the back button: a long title would otherwise run
     * underneath it and make it look like part of the text. */
    ui.m_title = label(ui.music, &lv_font_montserrat_28, COL_TEXT);
    lv_obj_set_width(ui.m_title, W - 150);
    lv_obj_set_style_text_align(ui.m_title, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_long_mode(ui.m_title, LV_LABEL_LONG_SCROLL_CIRCULAR);
    lv_obj_align(ui.m_title, LV_ALIGN_TOP_MID, 20, 4);

    ui.m_artist = label(ui.music, &lv_font_montserrat_20, COL_DIM);
    lv_obj_set_width(ui.m_artist, W - 20);
    lv_obj_set_style_text_align(ui.m_artist, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_long_mode(ui.m_artist, LV_LABEL_LONG_DOT);
    lv_obj_align(ui.m_artist, LV_ALIGN_TOP_MID, 0, 40);

    /* The two rows are 20 px apart. They were 8, which is less than twice the
     * margin hit testing adds, so the bottom of "next" and the top of "volume"
     * overlapped and a tap between them could land on either. */
    lv_coord_t bw = (W - 56) / 3;
    ui.m_prev = button(ui.music, bw, 58, LV_SYMBOL_PREV, &lv_font_montserrat_28);
    lv_obj_align(ui.m_prev, LV_ALIGN_TOP_LEFT, 10, 84);
    ui.m_play = button(ui.music, bw, 58, LV_SYMBOL_PLAY, &lv_font_montserrat_28);
    lv_obj_align(ui.m_play, LV_ALIGN_TOP_LEFT, 10 + bw + 18, 84);
    ui.m_next = button(ui.music, bw, 58, LV_SYMBOL_NEXT, &lv_font_montserrat_28);
    lv_obj_align(ui.m_next, LV_ALIGN_TOP_LEFT, 10 + 2 * (bw + 18), 84);

    /* The level sits between its own two buttons. It used to be a card that
     * floated in from the bottom - which landed exactly on top of the volume
     * buttons, so the feedback covered the thing it was reporting on. */
    ui.m_vol_down = button(ui.music, 92, 52, LV_SYMBOL_VOLUME_MID, &lv_font_montserrat_20);
    lv_obj_align(ui.m_vol_down, LV_ALIGN_TOP_LEFT, 12, 156);
    ui.m_vol_val = label(ui.music, &lv_font_montserrat_20, COL_TEXT);
    lv_obj_set_width(ui.m_vol_val, 84);
    lv_obj_set_style_text_align(ui.m_vol_val, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(ui.m_vol_val, LV_ALIGN_TOP_MID, 0, 170);
    ui.m_vol_up = button(ui.music, 92, 52, LV_SYMBOL_VOLUME_MAX, &lv_font_montserrat_20);
    lv_obj_align(ui.m_vol_up, LV_ALIGN_TOP_RIGHT, -12, 156);

    /* Settings: brightness and volume, one row each. */
    ui.settings = box(scr, W, H - 26);
    lv_obj_align(ui.settings, LV_ALIGN_TOP_LEFT, 0, 26);

    ui.s_back = button(ui.settings, 58, 34, LV_SYMBOL_LEFT, &lv_font_montserrat_20);
    lv_obj_align(ui.s_back, LV_ALIGN_TOP_LEFT, MIN_GAP, 2);

    lv_obj_t *bl_head = label(ui.settings, &lv_font_montserrat_16, COL_DIM);
    lv_label_set_text(bl_head, "Brightness");
    lv_obj_align(bl_head, LV_ALIGN_TOP_LEFT, 82, 10);

    ui.s_bl_down = button(ui.settings, 72, 52, LV_SYMBOL_MINUS, &lv_font_montserrat_28);
    lv_obj_align(ui.s_bl_down, LV_ALIGN_TOP_LEFT, 12, 50);
    /* Tapping the reading itself switches automatic on and off, so minus always
     * dims and plus always brightens - a cycle that wrapped from Auto round to
     * full brightness on a press of minus would just be a lie about direction. */
    /* A reading, not a button. It used to toggle automatic when tapped, which
     * put a control that dims the screen right next to the one that brightens
     * it - and made "plus made it dimmer" a thing that could happen from a tap
     * landing slightly left. Automatic has its own button now. */
    ui.s_bl_val = label(ui.settings, &lv_font_montserrat_28, COL_TEXT);
    lv_obj_set_width(ui.s_bl_val, 120);
    lv_obj_set_style_text_align(ui.s_bl_val, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(ui.s_bl_val, LV_ALIGN_TOP_MID, 0, 62);
    ui.s_bl_up = button(ui.settings, 72, 52, LV_SYMBOL_PLUS, &lv_font_montserrat_28);
    lv_obj_align(ui.s_bl_up, LV_ALIGN_TOP_RIGHT, -12, 50);

    ui.s_vol_down = button(ui.settings, 72, 52, LV_SYMBOL_VOLUME_MID, &lv_font_montserrat_28);
    lv_obj_align(ui.s_vol_down, LV_ALIGN_TOP_LEFT, 12, 116);
    ui.s_vol_val = label(ui.settings, &lv_font_montserrat_28, COL_TEXT);
    lv_obj_set_width(ui.s_vol_val, 120);
    lv_obj_set_style_text_align(ui.s_vol_val, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(ui.s_vol_val, LV_ALIGN_TOP_MID, 0, 128);
    ui.s_vol_up = button(ui.settings, 72, 52, LV_SYMBOL_VOLUME_MAX, &lv_font_montserrat_28);
    lv_obj_align(ui.s_vol_up, LV_ALIGN_TOP_RIGHT, -12, 116);

    ui.s_auto = button(ui.settings, 150, 30, "Auto", &lv_font_montserrat_16);
    lv_obj_align(ui.s_auto, LV_ALIGN_TOP_LEFT, 12, 182);

    /* What the touch panel actually reported, and what the backlight is
     * actually set to. The board has no other way to say either, and guessing
     * at them has cost enough. */
    ui.s_tap = label(ui.settings, &lv_font_montserrat_16, COL_DIM);
    lv_obj_align(ui.s_tap, LV_ALIGN_TOP_RIGHT, -12, 188);

    /* The route ahead. The phone sends it already turned so that forward is up
     * and measured from where the rider is, so there is no trigonometry here -
     * only a scale factor, chosen so the whole of what was sent fits. */
    ui.map = box(scr, W, H - 26);
    lv_obj_align(ui.map, LV_ALIGN_TOP_LEFT, 0, 26);

    ui.map_line = lv_line_create(ui.map);
    lv_obj_set_style_line_color(ui.map_line, COL_ACCENT, 0);
    lv_obj_set_style_line_width(ui.map_line, 5, 0);
    lv_obj_set_style_line_rounded(ui.map_line, true, 0);
    lv_obj_set_pos(ui.map_line, 0, 0);

    ui.map_me = box(ui.map, 14, 14);
    lv_obj_set_style_bg_color(ui.map_me, COL_TEXT, 0);
    lv_obj_set_style_bg_opa(ui.map_me, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(ui.map_me, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_border_color(ui.map_me, g_pal.bg, 0);
    lv_obj_set_style_border_width(ui.map_me, 2, 0);

    ui.map_scale = label(ui.map, &lv_font_montserrat_16, COL_DIM);
    lv_obj_align(ui.map_scale, LV_ALIGN_BOTTOM_RIGHT, -10, -6);

    ui.map_back = button(ui.map, 58, 34, LV_SYMBOL_LEFT, &lv_font_montserrat_20);
    lv_obj_align(ui.map_back, LV_ALIGN_TOP_LEFT, MIN_GAP, 2);

    /* An incoming call: nothing else on the screen, two large targets. */
    ui.call = box(scr, W, H - 26);
    lv_obj_align(ui.call, LV_ALIGN_TOP_LEFT, 0, 26);

    lv_obj_t *call_head = label(ui.call, &lv_font_montserrat_20, COL_DIM);
    lv_label_set_text(call_head, "Incoming call");
    lv_obj_align(call_head, LV_ALIGN_TOP_MID, 0, 8);

    ui.call_who = label(ui.call, &lv_font_montserrat_28, COL_TEXT);
    lv_obj_set_width(ui.call_who, W - 24);
    lv_obj_set_style_text_align(ui.call_who, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_long_mode(ui.call_who, LV_LABEL_LONG_DOT);
    lv_obj_align(ui.call_who, LV_ALIGN_TOP_MID, 0, 44);

    lv_coord_t cw = (W - 24 - MIN_GAP) / 2;
    ui.call_answer = button(ui.call, cw, 68, LV_SYMBOL_CALL, &lv_font_montserrat_28);
    lv_obj_set_style_bg_color(ui.call_answer, COL_GREEN, 0);
    lv_obj_align(ui.call_answer, LV_ALIGN_TOP_LEFT, 12, 112);
    ui.call_decline = button(ui.call, cw, 68, LV_SYMBOL_CLOSE, &lv_font_montserrat_28);
    lv_obj_set_style_bg_color(ui.call_decline, COL_RED, 0);
    lv_obj_align(ui.call_decline, LV_ALIGN_TOP_RIGHT, -12, 112);

    /* Diagnostics take over the whole area, but only when the link is broken.
     * Parented to the screen rather than to the home view, because it has to
     * be able to cover any of them. */
    ui.diag = box(scr, W, H - 26);
    lv_obj_align(ui.diag, LV_ALIGN_TOP_LEFT, 0, 26);

    ui.link_state = label(ui.diag, &lv_font_montserrat_12, COL_DIM);
    lv_obj_align(ui.link_state, LV_ALIGN_BOTTOM_LEFT, 6, -4);

    ui.link_state2 = label(ui.diag, &lv_font_montserrat_12, COL_DIM);
    lv_obj_align(ui.link_state2, LV_ALIGN_BOTTOM_LEFT, 6, -20);

    ui.log_label = label(ui.diag, &lv_font_montserrat_12, COL_DIM);
    lv_obj_set_width(ui.log_label, W - 12);
    lv_obj_align(ui.log_label, LV_ALIGN_TOP_LEFT, 6, 14);

    ui.screen = SCR_HOME;
    ui.before_call = SCR_HOME;
    lv_obj_add_flag(ui.nav, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(ui.music, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(ui.settings, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(ui.call, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(ui.map, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(ui.diag, LV_OBJ_FLAG_HIDDEN);
    apply_theme();

    /* Lay everything out now, so the coordinates the check reads are the real
     * ones rather than whatever they were before the first refresh. */
    lv_obj_update_layout(scr);
    lv_obj_t *home_controls[] = { ui.tile_nav, ui.tile_media, ui.tile_clock, ui.tile_set };
    lv_obj_t *music_controls[] = { ui.m_back, ui.m_prev, ui.m_play, ui.m_next,
                                   ui.m_vol_down, ui.m_vol_up };
    lv_obj_t *set_controls[] = { ui.s_back, ui.s_bl_down, ui.s_bl_up,
                                 ui.s_vol_down, ui.s_vol_up, ui.s_auto };
    lv_obj_t *call_controls[] = { ui.call_answer, ui.call_decline };
    check_spacing("home", home_controls, 4);
    check_spacing("music", music_controls, 6);
    check_spacing("set", set_controls, 6);
    check_spacing("call", call_controls, 2);
}

static void fmt_distance(uint32_t m, char *num, size_t num_cap, const char **unit)
{
    if (m < 20) {
        snprintf(num, num_cap, "Now");
        *unit = "";
    } else if (m < 1000) {
        snprintf(num, num_cap, "%lu", (unsigned long)((m + 5) / 10 * 10));
        *unit = "m";
    } else if (m < 10000) {
        unsigned long tenths = (m + 50) / 100;
        snprintf(num, num_cap, "%lu.%lu", tenths / 10, tenths % 10);
        *unit = "km";
    } else {
        snprintf(num, num_cap, "%lu", (unsigned long)((m + 500) / 1000));
        *unit = "km";
    }
}

static void fmt_remaining(uint32_t m, char *out, size_t cap)
{
    if (m == NAV_UNKNOWN_U32) {
        out[0] = '\0';
    } else if (m < 1000) {
        snprintf(out, cap, "%lu m", (unsigned long)m);
    } else {
        unsigned long tenths = (m + 50) / 100;
        snprintf(out, cap, "%lu.%lu km", tenths / 10, tenths % 10);
    }
}

/* Minutes since local midnight, or -1 if the phone has not sent the time yet. */
static int local_minute_of_day(const nav_state_t *s, uint32_t now_ms, uint32_t add_s)
{
    if (!s->clock_valid) {
        return -1;
    }
    int64_t t = (int64_t)(now_ms / 1000) + s->clock_offset_s + add_s;
    int64_t day_s = t % 86400;
    if (day_s < 0) {
        day_s += 86400;
    }
    return (int)(day_s / 60);
}

static void fmt_hhmm(int minute_of_day, char *out, size_t cap)
{
    if (minute_of_day < 0) {
        out[0] = '\0';
    } else {
        snprintf(out, cap, "%02d:%02d", minute_of_day / 60, minute_of_day % 60);
    }
}

static void set_hidden(lv_obj_t *o, bool hidden)
{
    if (o == NULL) {
        return;
    }
    if (hidden) {
        lv_obj_add_flag(o, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_clear_flag(o, LV_OBJ_FLAG_HIDDEN);
    }
}

static void draw_nav(const nav_state_t *s, uint32_t now_ms, bool stale)
{
    char num[16], buf[32];
    const char *unit = "";
    bool arrived = s->flags & NAV_FLAG_ARRIVED;
    bool rerouting = s->flags & NAV_FLAG_REROUTING;
    lv_color_t main_col = stale ? COL_DIM : COL_TEXT;

    uint8_t dir = arrived ? DIR_DESTINATION : s->direction;
    const lv_img_dsc_t *img = arrow_for_direction(dir, false);
    set_hidden(ui.arrow, img == NULL);
    if (img) {
        lv_img_set_src(ui.arrow, img);
        lv_obj_set_style_img_recolor(ui.arrow, rerouting ? COL_AMBER : main_col, 0);
    }

    if (arrived) {
        snprintf(num, sizeof(num), "Arrived");
    } else if (s->mode == NAV_MODE_BASIC) {
        snprintf(num, sizeof(num), "%s", s->distance_text);
    } else {
        fmt_distance(s->distance_m, num, sizeof(num), &unit);
    }
    lv_obj_set_style_text_font(ui.dist_num, strlen(num) > 5 ? &lv_font_montserrat_20 : &lv_font_montserrat_48, 0);
    lv_label_set_text(ui.dist_num, num);
    lv_obj_set_style_text_color(ui.dist_num, main_col, 0);
    lv_label_set_text(ui.dist_unit, unit);
    lv_obj_set_style_text_color(ui.dist_unit, main_col, 0);
    lv_obj_update_layout(ui.dist_num); /* align_to needs the new text width */
    lv_obj_align_to(ui.dist_unit, ui.dist_num, LV_ALIGN_OUT_RIGHT_BOTTOM, 4, -8);

    if (rerouting) {
        lv_label_set_text(ui.street, "Rerouting...");
        lv_obj_set_style_text_color(ui.street, COL_AMBER, 0);
    } else {
        lv_label_set_text(ui.street, s->street);
        lv_obj_set_style_text_color(ui.street, main_col, 0);
    }

    const lv_img_dsc_t *then_img = arrow_for_direction(s->then_direction, true);
    bool show_then = ui.landscape && then_img && !arrived && !rerouting;
    set_hidden(ui.then_label, !show_then);
    set_hidden(ui.then_arrow, !show_then);
    if (show_then) {
        lv_img_set_src(ui.then_arrow, then_img);
        lv_obj_align_to(ui.then_arrow, ui.then_label, LV_ALIGN_OUT_RIGHT_MID, 6, 0);
    }

    set_hidden(ui.limit, s->limit_kmh == 0);
    if (s->limit_kmh) {
        lv_label_set_text_fmt(ui.limit_num, "%u", s->limit_kmh);
    }

    bool full = s->mode == NAV_MODE_FULL;
    set_hidden(ui.bar, !full);
    if (full) {
        fmt_remaining(s->remaining_m, buf, sizeof(buf));
        lv_label_set_text(ui.bar_left, buf);
        if (s->minutes_left == NAV_UNKNOWN_U16) {
            lv_label_set_text(ui.bar_mid, "");
        } else if (s->minutes_left < 60) {
            lv_label_set_text_fmt(ui.bar_mid, "%u min", s->minutes_left);
        } else {
            lv_label_set_text_fmt(ui.bar_mid, "%u h %02u", s->minutes_left / 60, s->minutes_left % 60);
        }
        int eta = s->minutes_left == NAV_UNKNOWN_U16 ? -1
                                                     : local_minute_of_day(s, now_ms, (uint32_t)s->minutes_left * 60);
        fmt_hhmm(eta, buf, sizeof(buf));
        lv_label_set_text(ui.bar_right, buf);
    }
}

/* Home screen: the clock, and whatever the phone is playing. */
/* Discovering the phone's services takes a few seconds every time it
 * reconnects, and that gap is normal. Only call the link broken once it has
 * stayed that way, or the log flashes up during ordinary reconnects. */
static bool link_broken(const nav_state_t *s)
{
    static uint32_t unhealthy_since;
    if (s->connected && !s->apple_media) {
        if (unhealthy_since == 0) {
            unhealthy_since = ui.now_ms ? ui.now_ms : 1;
        }
    } else {
        unhealthy_since = 0;
    }
    return unhealthy_since != 0 && ui.now_ms - unhealthy_since > DIAG_AFTER_MS;
}

static void draw_brightness(lv_obj_t *target)
{
    uint8_t percent = prefs_brightness();
    if (percent == BRIGHTNESS_AUTO) {
        /* Say what automatic currently amounts to, or the reading means nothing. */
        lv_label_set_text_fmt(target, "Auto %u%%", (unsigned)ui.drawn_backlight);
    } else {
        lv_label_set_text_fmt(target, "%u%%", (unsigned)percent);
    }
}

static void draw_home(const nav_state_t *s, int minute, const char *hhmm)
{
    bool has_map = s->mode != NAV_MODE_IDLE;
    set_hidden(ui.idle_clock, has_map);
    set_hidden(ui.tile_map, !has_map);
    lv_label_set_text(ui.idle_clock, minute < 0 ? "--:--" : hhmm);

    bool music = s->music_valid;

    /* Route tile: the turn if there is one, otherwise it says so. */
    bool has_route = s->mode != NAV_MODE_IDLE;
    const lv_img_dsc_t *home_img = has_route ? arrow_for_direction(s->direction, false) : NULL;
    set_hidden(ui.home_arrow, home_img == NULL);
    if (home_img) {
        lv_img_set_src(ui.home_arrow, home_img);
    }
    set_hidden(ui.home_dist, !has_route);
    set_hidden(ui.home_street, !has_route);
    set_hidden(ui.home_none, has_route);
    if (has_route) {
        char num[16];
        const char *unit = "";
        if (s->mode == NAV_MODE_BASIC) {
            snprintf(num, sizeof(num), "%s", s->distance_text);
        } else {
            fmt_distance(s->distance_m, num, sizeof(num), &unit);
        }
        lv_label_set_text_fmt(ui.home_dist, "%s%s", num, unit);
        lv_label_set_text(ui.home_street, s->street);
    }

    set_hidden(ui.track, !music);
    set_hidden(ui.artist, !music);
    set_hidden(ui.idle_msg, music);

    if (music) {
        lv_label_set_text(ui.track, s->music_title[0] ? s->music_title : "-");
        lv_label_set_text(ui.artist, s->music_artist);
        lv_obj_set_style_text_color(ui.track, s->music_playing ? COL_TEXT : COL_DIM, 0);
    } else if (!s->connected) {
        /* The name is what the rider has to look for in the phone's Bluetooth
         * list, and this is the only screen shown before there is a phone. */
        lv_label_set_text_fmt(ui.idle_msg, "No phone\n%s", ui.device_name);
    } else if (!s->apple_paired) {
        lv_label_set_text(ui.idle_msg, "Not paired");
    } else if (!s->apple_media) {
        lv_label_set_text(ui.idle_msg, "No media");
    } else {
        lv_label_set_text(ui.idle_msg, "Nothing playing");
    }

    draw_brightness(ui.tile_set_val);
}

/* The phone's own level, not what the board last asked for. A dash rather than
 * a number when the phone has not said: an invented figure would be worse than
 * admitting we do not know. */
static void draw_volume(lv_obj_t *target, const nav_state_t *s)
{
    if (s->volume_valid) {
        lv_label_set_text_fmt(target, "%u%%", (unsigned)s->volume_percent);
    } else {
        lv_label_set_text(target, "--");
    }
}

/* LVGL keeps the pointer it is given rather than copying, so the points have
 * to outlive the call. */
static lv_point_t g_map_points[NAV_ROUTE_MAX];

static void draw_map(const nav_state_t *s)
{
    lv_coord_t w = lv_obj_get_width(ui.map);
    lv_coord_t h = lv_obj_get_height(ui.map);
    /* The rider sits low and centred, because almost all of what matters is
     * ahead; the little behind is worth seeing to know the turn just taken. */
    lv_coord_t cx = w / 2;
    lv_coord_t cy = h - 46;

    lv_obj_align(ui.map_me, LV_ALIGN_TOP_LEFT, cx - 7, cy - 7);

    if (s->route_points < 2) {
        set_hidden(ui.map_line, true);
        lv_label_set_text(ui.map_scale, s->mode == NAV_MODE_IDLE ? "No route" : "Waiting for the route");
        return;
    }
    set_hidden(ui.map_line, false);

    /* One scale for both axes, or the road bends in ways it does not. */
    int32_t max_x = 1, max_y = 1;
    for (uint8_t i = 0; i < s->route_points; i++) {
        int32_t ax = s->route_x[i] < 0 ? -s->route_x[i] : s->route_x[i];
        int32_t ay = s->route_y[i] < 0 ? -s->route_y[i] : s->route_y[i];
        if (ax > max_x) max_x = ax;
        if (ay > max_y) max_y = ay;
    }
    int32_t sx = ((int32_t)(cx - 14) * 256) / max_x;
    int32_t sy = ((int32_t)(cy - 14) * 256) / max_y;
    int32_t sc = sx < sy ? sx : sy;
    if (sc < 1) {
        sc = 1;
    }

    for (uint8_t i = 0; i < s->route_points; i++) {
        g_map_points[i].x = (lv_coord_t)(cx + ((int32_t)s->route_x[i] * sc) / 256);
        g_map_points[i].y = (lv_coord_t)(cy - ((int32_t)s->route_y[i] * sc) / 256);
    }
    lv_line_set_points(ui.map_line, g_map_points, s->route_points);

    /* How far the view reaches, so the drawing has a sense of distance. */
    uint32_t reach = (uint32_t)max_y * s->route_unit_m;
    if (reach >= 1000) {
        lv_label_set_text_fmt(ui.map_scale, "%u.%u km ahead", (unsigned)(reach / 1000),
                              (unsigned)((reach % 1000) / 100));
    } else {
        lv_label_set_text_fmt(ui.map_scale, "%u m ahead", (unsigned)reach);
    }
}

static void draw_music(const nav_state_t *s)
{
    draw_volume(ui.m_vol_val, s);
    bool music = s->music_valid && s->music_title[0] != '\0';
    lv_label_set_text(ui.m_title, music ? s->music_title : "Nothing playing");
    lv_label_set_text(ui.m_artist, s->music_valid ? s->music_artist : "");
    lv_label_set_text(button_label(ui.m_play),
                      s->music_playing ? LV_SYMBOL_PAUSE : LV_SYMBOL_PLAY);
}

static void draw_call(const nav_state_t *s)
{
    lv_label_set_text(ui.call_who, s->call_name[0] ? s->call_name : "Unknown caller");
    /* The phone says which actions the notification allows. Showing a button
     * the phone would refuse is worse than showing none. */
    set_hidden(ui.call_answer, !s->call_can_answer);
    set_hidden(ui.call_decline, !s->call_can_decline);
}

/* The board has no serial console, so when the link is down the screen becomes
 * one: the log plus the two state lines, and nothing else. */
static void draw_diag(const nav_state_t *s)
{
    char lines[LOGBUF_LINES][LOGBUF_WIDTH + 1];
    int n = logbuf_snapshot(lines);
    char text[LOGBUF_LINES * (LOGBUF_WIDTH + 2)];
    text[0] = '\0';
    for (int i = 0; i < n; i++) {
        strcat(text, lines[i]);
        if (i + 1 < n) {
            strcat(text, "\n");
        }
    }
    lv_label_set_text(ui.log_label, text);
    lv_label_set_text_fmt(ui.link_state2, "ams ccc=%u c=%02x w=%02x n=%u sec=%u%s",
                          (unsigned)s->apple_ccc, (unsigned)s->apple_ccc_err,
                          (unsigned)s->apple_write_err, (unsigned)s->apple_notifs,
                          (unsigned)s->apple_sec, s->apple_sub_lost ? " lost" : "");
    lv_label_set_text_fmt(ui.link_state, "link: %s %s svc=%u att=%02x %s%s",
                          s->connected ? "conn" : "-",
                          s->apple_paired ? "paired" : "unpaired",
                          (unsigned)s->apple_services,
                          (unsigned)s->apple_att_err,
                          s->apple_media ? (s->apple_subscribed ? "sub" : "media") : "no-media",
                          s->apple_clock ? "+clock" : "");
}

/* Minus always dims and plus always brightens, with no wrap: a cycle that went
 * from Auto round to full brightness on a press of minus was lying about the
 * direction. Automatic is a separate choice, made by tapping the reading. */
static void step_brightness(int direction)
{
    int now = prefs_brightness();
    if (now == BRIGHTNESS_AUTO) {
        /* Carry on from what it looks like now, not from an end of the scale. */
        now = ui.drawn_backlight ? ui.drawn_backlight : BL_DAY;
    }
    int next = now + direction * BL_STEP;
    next = (next / BL_STEP) * BL_STEP; /* back onto round numbers */
    if (next < BL_LOWEST) {
        next = BL_LOWEST;
    } else if (next > 100) {
        next = 100;
    }
    prefs_set_brightness((uint8_t)next);
    /* At once: a brightness control that waits for the next redraw feels broken. */
    ui.drawn_backlight = 0;
    apply_backlight(ui.drawn_light);
}

static void toggle_auto_brightness(void)
{
    if (prefs_brightness() == BRIGHTNESS_AUTO) {
        prefs_set_brightness(ui.drawn_backlight ? ui.drawn_backlight : BL_DAY);
    } else {
        prefs_set_brightness(BRIGHTNESS_AUTO);
    }
    ui.drawn_backlight = 0;
    apply_backlight(ui.drawn_light);
}

/* Force the next ui_update to redraw, after a tap has changed what is showing. */
static void invalidate(void)
{
    ui.drawn_version = UINT32_MAX;
}

static void go_home(void)
{
    if (ui.screen != SCR_HOME) {
        ui.screen = SCR_HOME;
    }
    invalidate(); /* the tap readout moved, even if the screen did not */
}

ui_action_t ui_tap(lv_coord_t x, lv_coord_t y)
{
    ui_action_t act = { UI_ACT_NONE, 0 };

    /* A ringing phone owns the screen: answering or declining are the only two
     * things a tap can mean, and neither should need aiming for. */
    if (!lv_obj_has_flag(ui.call, LV_OBJ_FLAG_HIDDEN)) {
        if (hit(ui.call_answer, x, y)) {
            act.kind = UI_ACT_CALL_ANSWER;
        } else if (hit(ui.call_decline, x, y)) {
            act.kind = UI_ACT_CALL_DECLINE;
        }
        return act;
    }
    /* Diagnostics are a read-out, not a control surface. */
    if (!lv_obj_has_flag(ui.diag, LV_OBJ_FLAG_HIDDEN)) {
        return act;
    }

    ui.last_x = x;
    ui.last_y = y;

    /* The top bar is one way back, but only one. A 26 px strip along the edge
     * of the glass was the sole way out, and it did not work: thin, and right
     * where a digitizer is least reliable. The back button below is the real
     * answer, and on the screens where nothing is at stake, so is empty space. */
    if (y < 26) {
        if (ui.screen != SCR_HOME) {
            ui.screen = SCR_HOME;
            invalidate();
        }
        return act;
    }

    switch (ui.screen) {
        case SCR_HOME:
            if (hit(ui.tile_nav, x, y)) {
                ui.screen = SCR_NAV;
            } else if (hit(ui.tile_media, x, y)) {
                ui.screen = SCR_MUSIC;
            } else if (hit(ui.tile_set, x, y)) {
                ui.screen = SCR_SETTINGS;
            } else if (hit(ui.tile_clock, x, y) && !lv_obj_has_flag(ui.tile_map, LV_OBJ_FLAG_HIDDEN)) {
                ui.screen = SCR_MAP;
            } else {
                break; /* the clock is a read-out, not a button */
            }
            invalidate();
            break;

        case SCR_MUSIC:
            act.kind = UI_ACT_MEDIA;
            if (hit(ui.m_prev, x, y)) {
                act.arg = MEDIA_CMD_PREVIOUS;
            } else if (hit(ui.m_play, x, y)) {
                act.arg = MEDIA_CMD_TOGGLE;
            } else if (hit(ui.m_next, x, y)) {
                act.arg = MEDIA_CMD_NEXT;
            } else if (hit(ui.m_vol_down, x, y)) {
                act.arg = MEDIA_CMD_VOLUME_DOWN;
            } else {
                act.kind = UI_ACT_NONE;
                if (hit(ui.m_vol_up, x, y)) {
                    act.kind = UI_ACT_MEDIA;
                    act.arg = MEDIA_CMD_VOLUME_UP;
                } else {
                    go_home(); /* the back button, or anywhere that is not a control */
                }
            }
            break;

        case SCR_SETTINGS:
            if (hit(ui.s_bl_down, x, y)) {
                step_brightness(-1);
                invalidate();
            } else if (hit(ui.s_bl_up, x, y)) {
                step_brightness(1);
                invalidate();
            } else if (hit(ui.s_auto, x, y)) {
                toggle_auto_brightness();
                invalidate();
            } else if (hit(ui.s_vol_down, x, y)) {
                act.kind = UI_ACT_MEDIA;
                act.arg = MEDIA_CMD_VOLUME_DOWN;
            } else if (hit(ui.s_vol_up, x, y)) {
                act.kind = UI_ACT_MEDIA;
                act.arg = MEDIA_CMD_VOLUME_UP;
            } else {
                go_home();
            }
            break;

        case SCR_MAP:
            go_home();
            break;

        case SCR_NAV:
            /* Not anywhere, here: the turn arrow is the one thing that must not
             * vanish because a glove brushed the glass. A corner is still easy
             * to find without looking. */
            if (x < lv_disp_get_hor_res(NULL) / 3 && y < 26 + (lv_disp_get_ver_res(NULL) - 26) / 2) {
                go_home();
            }
            break;

        default:
            break;
    }
    return act;
}

void ui_update(const nav_state_t *s, uint32_t now_ms)
{
    ui.now_ms = now_ms; /* link_broken() times from this */

    /* Repeats of the same instruction keep the link alive but do not count as
     * progress, so a finished route eventually gives way to the home screen. */
    uint32_t unchanged_ms = now_ms - s->last_change_ms;
    uint32_t give_up_ms = s->connected ? HOME_AFTER_MS : HOME_AFTER_DISCONNECT_MS;
    bool navigating = s->mode != NAV_MODE_IDLE && unchanged_ms < give_up_ms;
    bool stale = navigating && (now_ms - s->last_packet_ms > STALE_MS);
    int minute = local_minute_of_day(s, now_ms, 0);
    bool ringing = s->call_ringing;
    bool broken = link_broken(s);

    /* Light face by day, dark by night; dark until the phone shares its clock. */
    bool light = minute >= LIGHT_FROM_MIN && minute < LIGHT_TO_MIN;
    if (light != ui.drawn_light) {
        ui.drawn_light = light;
        set_palette(light);
        apply_theme();
    }
    apply_backlight(light);

    unsigned log_version = logbuf_version();
    if (s->version == ui.drawn_version && stale == ui.drawn_stale && minute == ui.drawn_minute &&
        navigating == ui.drawn_navigating && log_version == ui.drawn_log &&
        ringing == ui.drawn_ringing) {
        return;
    }
    bool was_navigating = ui.drawn_navigating;
    ui.drawn_log = log_version;
    ui.drawn_version = s->version;
    ui.drawn_stale = stale;
    ui.drawn_minute = minute;
    ui.drawn_navigating = navigating;

    /* A ringing phone borrows the screen and gives back whatever was there.
     * Handed back first, so that if the route ended while they were talking,
     * the check below still catches it - otherwise the call would restore a
     * turn-by-turn screen that has nothing left to show. */
    if (ringing && !ui.drawn_ringing) {
        ui.before_call = ui.screen;
    } else if (!ringing && ui.drawn_ringing) {
        ui.screen = ui.before_call;
    }
    ui.drawn_ringing = ringing;

    /* A route starting takes the rider to the turn-by-turn screen, and a route
     * ending hands it back. Only on the change: otherwise a deliberate tap back
     * to the tiles would be undone a tenth of a second later. */
    if (navigating && !was_navigating && ui.screen == SCR_HOME) {
        ui.screen = SCR_NAV;
    } else if (!navigating && (ui.screen == SCR_NAV || ui.screen == SCR_MAP)) {
        ui.screen = SCR_HOME;
    }

    /* A call outranks a broken link, which outranks whatever was chosen. */
    bool show_call = ringing;
    bool show_diag = !ringing && broken;
    bool normal = !show_call && !show_diag;
    set_hidden(ui.call, !show_call);
    set_hidden(ui.diag, !show_diag);
    set_hidden(ui.nav, !(normal && ui.screen == SCR_NAV));
    set_hidden(ui.idle, !(normal && ui.screen == SCR_HOME));
    set_hidden(ui.music, !(normal && ui.screen == SCR_MUSIC));
    set_hidden(ui.settings, !(normal && ui.screen == SCR_SETTINGS));
    set_hidden(ui.map, !(normal && ui.screen == SCR_MAP));

    char hhmm[8];
    fmt_hhmm(minute, hhmm, sizeof(hhmm));
    lv_label_set_text(ui.clock, hhmm);

    /* Status: the most important problem wins. */
    const char *status_text;
    lv_color_t status_col;
    if (!s->connected) {
        status_text = LV_SYMBOL_BLUETOOTH " off";
        status_col = COL_RED;
    } else if (stale) {
        status_text = LV_SYMBOL_WARNING " no data";
        status_col = COL_AMBER;
    } else if (s->flags & NAV_FLAG_GPS_WEAK) {
        status_text = LV_SYMBOL_GPS " weak";
        status_col = COL_AMBER;
    } else {
        status_text = LV_SYMBOL_BLUETOOTH;
        status_col = COL_GREEN;
    }
    /* Off the home screen the top bar is also the way back, and nothing else
     * on the screen would say so. */
    if (normal && ui.screen != SCR_HOME) {
        lv_label_set_text_fmt(ui.status, "%s %s", LV_SYMBOL_LEFT, status_text);
    } else {
        lv_label_set_text(ui.status, status_text);
    }
    lv_obj_set_style_text_color(ui.status, status_col, 0);

    if (navigating && s->speed_kmh != NAV_UNKNOWN_U8) {
        lv_label_set_text_fmt(ui.speed, "%u km/h", s->speed_kmh);
    } else {
        lv_label_set_text(ui.speed, "");
    }

    if (show_call) {
        draw_call(s);
        return;
    }
    if (show_diag) {
        draw_diag(s);
        return;
    }
    switch (ui.screen) {
        case SCR_NAV:
            draw_nav(s, now_ms, stale);
            break;
        case SCR_MUSIC:
            draw_music(s);
            break;
        case SCR_MAP:
            draw_map(s);
            break;
        case SCR_SETTINGS:
            draw_brightness(ui.s_bl_val);
            draw_volume(ui.s_vol_val, s);
            lv_label_set_text_fmt(ui.s_tap, "tap %d,%d  duty %u", (int)ui.last_x,
                                  (int)ui.last_y, (unsigned)backlight_duty());
            break;
        case SCR_HOME:
        default:
            draw_home(s, minute, hhmm);
            break;
    }
}
