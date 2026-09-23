#include "ui.h"

#include <stdio.h>
#include <string.h>

#include "arrows.h"
#include "backlight.h"
#include "logbuf.h"
#include "lvgl.h"

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

    lv_obj_t *idle;     /* home screen: two cards, route above, music below */
    lv_obj_t *card_nav;
    lv_obj_t *home_arrow;
    lv_obj_t *home_dist;
    lv_obj_t *home_street;
    lv_obj_t *idle_clock;  /* fills the route card when there is no route */
    lv_obj_t *card_media;
    lv_obj_t *idle_msg;
    lv_obj_t *track;
    lv_obj_t *artist;
    lv_obj_t *transport;  /* prev / play-pause / next */

    lv_obj_t *diag;        /* only when the phone link is not working */
    lv_obj_t *link_state;
    lv_obj_t *link_state2;
    lv_obj_t *log_label;   /* firmware log, the board's only console */
    unsigned drawn_log;

    uint32_t drawn_version;
    int drawn_minute;
    bool drawn_stale;
    bool drawn_navigating;
    bool drawn_light;
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

/* Re-colour everything when the day/night palette changes. */
static void apply_theme(void)
{
    lv_obj_t *on_text[] = { ui.speed, ui.clock, ui.dist_num, ui.dist_unit, ui.street,
                            ui.bar_left, ui.bar_mid, ui.idle_clock, ui.track,
                            ui.home_dist, ui.transport };
    lv_obj_t *on_dim[] = { ui.status, ui.then_label, ui.artist, ui.idle_msg, ui.home_street,
                           ui.link_state, ui.link_state2, ui.log_label };

    lv_obj_set_style_bg_color(lv_scr_act(), g_pal.bg, 0);
    lv_obj_set_style_bg_color(ui.card_nav, g_pal.card, 0);
    lv_obj_set_style_bg_color(ui.card_media, g_pal.card, 0);
    lv_obj_set_style_bg_color(ui.bar, g_pal.bar, 0);

    for (unsigned i = 0; i < sizeof(on_text) / sizeof(on_text[0]); i++) {
        lv_obj_set_style_text_color(on_text[i], g_pal.text, 0);
    }
    for (unsigned i = 0; i < sizeof(on_dim) / sizeof(on_dim[0]); i++) {
        lv_obj_set_style_text_color(on_dim[i], g_pal.dim, 0);
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

    /* Home screen: a route card above a music card, like a phone widget. */
    ui.idle = box(scr, W, H - 26);
    lv_obj_align(ui.idle, LV_ALIGN_TOP_LEFT, 0, 26);

    lv_coord_t card_w = W - 16;
    ui.card_nav = card(ui.idle, card_w, 78);
    lv_obj_align(ui.card_nav, LV_ALIGN_TOP_MID, 0, 0);

    ui.home_arrow = tinted_img(ui.card_nav, COL_ACCENT);
    lv_img_set_zoom(ui.home_arrow, 128); /* the nav-screen arrows at half size */
    lv_obj_align(ui.home_arrow, LV_ALIGN_LEFT_MID, -18, 0);

    ui.home_dist = label(ui.card_nav, &lv_font_montserrat_28, COL_TEXT);
    lv_obj_align(ui.home_dist, LV_ALIGN_TOP_LEFT, 64, 6);

    ui.home_street = label(ui.card_nav, &lv_font_montserrat_16, COL_DIM);
    lv_obj_set_width(ui.home_street, card_w - 74);
    lv_label_set_long_mode(ui.home_street, LV_LABEL_LONG_DOT);
    lv_obj_align(ui.home_street, LV_ALIGN_TOP_LEFT, 64, 44);

    /* No route: the card carries the time instead, so it is never dead space. */
    ui.idle_clock = label(ui.card_nav, &lv_font_montserrat_48, COL_TEXT);
    lv_obj_align(ui.idle_clock, LV_ALIGN_CENTER, 0, 0);

    ui.card_media = card(ui.idle, card_w, 118);
    lv_obj_align(ui.card_media, LV_ALIGN_TOP_MID, 0, 86);

    ui.track = label(ui.card_media, &lv_font_montserrat_20, COL_TEXT);
    lv_obj_set_width(ui.track, card_w - 20);
    lv_obj_set_style_text_align(ui.track, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_long_mode(ui.track, LV_LABEL_LONG_SCROLL_CIRCULAR);
    lv_obj_align(ui.track, LV_ALIGN_TOP_MID, 0, 8);

    ui.artist = label(ui.card_media, &lv_font_montserrat_16, COL_DIM);
    lv_obj_set_width(ui.artist, card_w - 20);
    lv_obj_set_style_text_align(ui.artist, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_long_mode(ui.artist, LV_LABEL_LONG_DOT);
    lv_obj_align(ui.artist, LV_ALIGN_TOP_MID, 0, 36);

    /* Lifted clear of the bottom edge, so a thumb does not have to find the rim. */
    ui.transport = label(ui.card_media, &lv_font_montserrat_28, COL_TEXT);
    lv_obj_set_width(ui.transport, card_w);
    lv_obj_set_style_text_align(ui.transport, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(ui.transport, LV_ALIGN_BOTTOM_MID, 0, -14);

    ui.idle_msg = label(ui.card_media, &lv_font_montserrat_16, COL_DIM);
    lv_obj_set_width(ui.idle_msg, card_w - 20);
    lv_obj_set_style_text_align(ui.idle_msg, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_long_mode(ui.idle_msg, LV_LABEL_LONG_WRAP);
    lv_obj_align(ui.idle_msg, LV_ALIGN_CENTER, 0, 0);

    /* Diagnostics take over the whole area, but only when the link is broken. */
    ui.diag = box(ui.idle, W, H - 26);
    lv_obj_align(ui.diag, LV_ALIGN_TOP_LEFT, 0, 0);

    ui.link_state = label(ui.diag, &lv_font_montserrat_12, COL_DIM);
    lv_obj_align(ui.link_state, LV_ALIGN_BOTTOM_LEFT, 6, -4);

    ui.link_state2 = label(ui.diag, &lv_font_montserrat_12, COL_DIM);
    lv_obj_align(ui.link_state2, LV_ALIGN_BOTTOM_LEFT, 6, -20);

    ui.log_label = label(ui.diag, &lv_font_montserrat_12, COL_DIM);
    lv_obj_set_width(ui.log_label, W - 12);
    lv_obj_align(ui.log_label, LV_ALIGN_TOP_LEFT, 6, 14);

    lv_obj_add_flag(ui.nav, LV_OBJ_FLAG_HIDDEN);
    apply_theme();
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
static void draw_home(const nav_state_t *s, int minute, const char *hhmm)
{
    lv_label_set_text(ui.idle_clock, minute < 0 ? "BikeNav" : hhmm);

    bool music = s->music_valid;
    /* Discovering the phone's services takes a few seconds every time it
     * reconnects, and that gap is normal. Only call the link broken once it has
     * stayed that way, or the log flashes up during ordinary reconnects. */
    static uint32_t unhealthy_since;
    if (s->connected && !s->apple_media) {
        if (unhealthy_since == 0) {
            unhealthy_since = ui.now_ms ? ui.now_ms : 1;
        }
    } else {
        unhealthy_since = 0;
    }
    bool broken = unhealthy_since != 0 && ui.now_ms - unhealthy_since > DIAG_AFTER_MS;
    set_hidden(ui.diag, !broken);
    set_hidden(ui.card_nav, broken);
    set_hidden(ui.card_media, broken);

    /* Route card: the turn if there is one, otherwise the time. */
    bool has_route = s->mode != NAV_MODE_IDLE;
    const lv_img_dsc_t *home_img = has_route ? arrow_for_direction(s->direction, false) : NULL;
    set_hidden(ui.home_arrow, home_img == NULL);
    if (home_img) {
        lv_img_set_src(ui.home_arrow, home_img);
    }
    set_hidden(ui.home_dist, !has_route);
    set_hidden(ui.home_street, !has_route);
    set_hidden(ui.idle_clock, has_route);
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
    set_hidden(ui.transport, !music);
    set_hidden(ui.idle_msg, music);

    if (music) {
        lv_label_set_text(ui.track, s->music_title[0] ? s->music_title : "-");
        lv_label_set_text(ui.artist, s->music_artist);
        lv_obj_set_style_text_color(ui.track, s->music_playing ? COL_TEXT : COL_DIM, 0);
        lv_label_set_text_fmt(ui.transport, "%s      %s      %s", LV_SYMBOL_PREV,
                              s->music_playing ? LV_SYMBOL_PAUSE : LV_SYMBOL_PLAY, LV_SYMBOL_NEXT);
    } else if (!s->connected) {
        lv_label_set_text_fmt(ui.idle_msg, "Waiting for phone...\nBluetooth name: %s", ui.device_name);
    } else if (!s->apple_paired) {
        lv_label_set_text(ui.idle_msg, "Connected, not paired.\niPhone Settings > Bluetooth > BikeNav");
    } else if (!s->apple_media) {
        lv_label_set_text(ui.idle_msg, "Paired, but the phone is not\nsharing its media info.");
    } else {
        lv_label_set_text(ui.idle_msg, "Ready. Play something,\nor start a route.");
    }

    /* The board has no serial console, so when the link is down the screen
     * becomes one: the log plus the two state lines, and nothing else. */
    if (!broken) {
        return;
    }
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

int ui_media_zone(lv_coord_t x, lv_coord_t y)
{
    if (lv_obj_has_flag(ui.transport, LV_OBJ_FLAG_HIDDEN) ||
        lv_obj_has_flag(ui.card_media, LV_OBJ_FLAG_HIDDEN)) {
        return -1;
    }
    /* Target the row where the controls really are, padded generously: this is
     * aimed at with a thumb, on a bike, without looking. */
    lv_area_t a;
    lv_obj_get_coords(ui.transport, &a);
    if (y < a.y1 - 22 || y > a.y2 + 22) {
        return -1;
    }
    lv_coord_t W = lv_disp_get_hor_res(NULL);
    return x < W / 3 ? 0 : (x < 2 * W / 3 ? 1 : 2);
}

void ui_update(const nav_state_t *s, uint32_t now_ms)
{
    /* Repeats of the same instruction keep the link alive but do not count as
     * progress, so a finished route eventually gives way to the home screen. */
    uint32_t unchanged_ms = now_ms - s->last_change_ms;
    uint32_t give_up_ms = s->connected ? HOME_AFTER_MS : HOME_AFTER_DISCONNECT_MS;
    bool navigating = s->mode != NAV_MODE_IDLE && unchanged_ms < give_up_ms;
    bool stale = navigating && (now_ms - s->last_packet_ms > STALE_MS);
    int minute = local_minute_of_day(s, now_ms, 0);

    /* Light face by day, dark by night; dark until the phone shares its clock. */
    bool light = minute >= LIGHT_FROM_MIN && minute < LIGHT_TO_MIN;
    if (light != ui.drawn_light) {
        ui.drawn_light = light;
        set_palette(light);
        apply_theme();
    }

    unsigned log_version = logbuf_version();
    if (s->version == ui.drawn_version && stale == ui.drawn_stale && minute == ui.drawn_minute &&
        navigating == ui.drawn_navigating && log_version == ui.drawn_log) {
        return;
    }
    ui.drawn_log = log_version;
    ui.drawn_version = s->version;
    ui.drawn_stale = stale;
    ui.drawn_minute = minute;
    ui.drawn_navigating = navigating;

    ui.now_ms = now_ms;

    char hhmm[8];
    fmt_hhmm(minute, hhmm, sizeof(hhmm));
    lv_label_set_text(ui.clock, hhmm);

    /* Status: the most important problem wins. */
    if (!s->connected) {
        lv_label_set_text(ui.status, LV_SYMBOL_BLUETOOTH " off");
        lv_obj_set_style_text_color(ui.status, COL_RED, 0);
    } else if (stale) {
        lv_label_set_text(ui.status, LV_SYMBOL_WARNING " no data");
        lv_obj_set_style_text_color(ui.status, COL_AMBER, 0);
    } else if (s->flags & NAV_FLAG_GPS_WEAK) {
        lv_label_set_text(ui.status, LV_SYMBOL_GPS " weak");
        lv_obj_set_style_text_color(ui.status, COL_AMBER, 0);
    } else {
        lv_label_set_text(ui.status, LV_SYMBOL_BLUETOOTH);
        lv_obj_set_style_text_color(ui.status, COL_GREEN, 0);
    }

    if (navigating && s->speed_kmh != NAV_UNKNOWN_U8) {
        lv_label_set_text_fmt(ui.speed, "%u km/h", s->speed_kmh);
    } else {
        lv_label_set_text(ui.speed, "");
    }

    set_hidden(ui.nav, !navigating);
    set_hidden(ui.idle, navigating);

    if (navigating) {
        draw_nav(s, now_ms, stale);
    } else {
        draw_home(s, minute, hhmm);
    }
}
