#include "app.h"
#include "platform.h"
#include "gfx.h"
#include "player.h"
#include "library.h"
#include "tags.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

// ---------------------------------------------------------------- palette
#define C_BG        RGB(13, 14, 17)
#define C_SURFACE   RGB(24, 26, 31)
#define C_RAISED    RGB(34, 37, 44)
#define C_LINE      RGBA(255, 255, 255, 18)
#define C_TEXT      RGB(242, 243, 245)
#define C_TEXT2     RGB(156, 160, 170)
#define C_TEXT3     RGB(100, 104, 115)
#define C_ACCENT    RGB(110, 231, 200)
#define C_ACCENT_DK RGB(12, 40, 33)
#define C_GOLD      RGB(240, 196, 110)

#define PS_CROSS    RGB(124, 178, 232)
#define PS_CIRCLE   RGB(255, 112, 112)
#define PS_SQUARE   RGB(240, 160, 205)
#define PS_TRIANGLE RGB(95, 211, 161)

enum { VIEW_BROWSER, VIEW_PLAYING };

static canvas cv;
static dir_view dv;
static int view = VIEW_BROWSER;
static int dirty = 1;
static int screen_off;
static uint64_t last_draw;

static uint32_t prev_buttons;
static uint64_t hold_start[16], hold_last[16];

static uint32_t cur_serial = (uint32_t)-1;
static track_tags cur_tags;
static image cover_big, cover_small;
static uint32_t cover_tint = 0;

static char toast[96];
static uint64_t toast_until;

static char settings_path[512];

#define ROW_H 52
#define LIST_Y 80
#define LIST_ROWS 7

static float list_y;   // browser scroll offset in pixels (touch scrolls smoothly)
static float list_vel; // fling speed, px/s

// ---------------------------------------------------------------- touch
// Every tap ends up as a virtual button press for one tick, so touch and
// buttons share the same code paths. Draw code registers what is tappable
// as it draws, so hit areas always match what is on screen.
enum { Z_BUTTON, Z_LIST, Z_SEEK };
typedef struct { float x, y, w, h; int kind; uint32_t btn; } zone;
static zone zones[24];
static int nzones;

static struct {
    int down, moved, caught, blocked;
    zone z;                 // zone under the finger when it went down
    float x0, y0, x, y;     // start / current position
    float vel;              // list drag speed, px/s
    uint64_t t_move;
} tc;
static float scrub = -1;    // 0..1 while a finger is on the progress bar
static uint64_t last_tick;

#define TAP_SLOP 12

static int in_zone(const zone *z, float x, float y) {
    return x >= z->x && x < z->x + z->w && y >= z->y && y < z->y + z->h;
}

// Register a tappable area. r >= 0 draws a pressed highlight with that
// corner radius while the finger is on it, so call it before drawing the
// control itself.
static void hot(float x, float y, float w, float h, float r, int kind, uint32_t btn) {
    if (nzones < (int)(sizeof zones / sizeof *zones)) zones[nzones++] = (zone){ x, y, w, h, kind, btn };
    if (r >= 0 && tc.down && !tc.blocked && tc.z.kind == kind && tc.z.btn == btn &&
        tc.z.x == x && tc.z.y == y && in_zone(&tc.z, tc.x, tc.y))
        gfx_rrect(&cv, x, y, w, h, r, WITH_ALPHA(C_TEXT, 26));
}

static int list_row_at(float y) {
    int r = (int)floorf((y - LIST_Y + list_y) / ROW_H);
    return r >= 0 && r < dv.n ? r : -1;
}

// list row under a resting finger (-1 once it starts scrolling)
static int pressed_row(void) {
    if (!tc.down || tc.blocked || tc.z.kind != Z_LIST || tc.moved || tc.caught) return -1;
    return list_row_at(tc.y);
}

// ---------------------------------------------------------------- settings
static void save_settings(void) {
    player_status s;
    player_get_status(&s);
    FILE *f = fopen(settings_path, "w");
    if (!f) return;
    fprintf(f, "dir=%s\nshuffle=%d\nrepeat=%d\n", dv.path, s.shuffle, s.repeat);
    fclose(f);
}

static void load_settings(char *dir, size_t n, int *sh, int *rep) {
    FILE *f = fopen(settings_path, "r");
    if (!f) return;
    char line[1100];
    while (fgets(line, sizeof line, f)) {
        line[strcspn(line, "\r\n")] = 0;
        if (!strncmp(line, "dir=", 4)) snprintf(dir, n, "%s", line + 4);
        else if (!strncmp(line, "shuffle=", 8)) *sh = atoi(line + 8);
        else if (!strncmp(line, "repeat=", 7)) *rep = atoi(line + 7);
    }
    fclose(f);
}

static void show_toast(const char *msg) {
    snprintf(toast, sizeof toast, "%s", msg);
    toast_until = plat_time_us() + 1400000;
    dirty = 1;
}

// ---------------------------------------------------------------- icons
static void icon_play(float cx, float cy, float s, uint32_t col) {
    gfx_triangle(&cv, cx - s * 0.38f, cy - s * 0.5f, cx + s * 0.5f, cy, cx - s * 0.38f, cy + s * 0.5f, col);
}
static void icon_pause(float cx, float cy, float s, uint32_t col) {
    float w = s * 0.28f, h = s;
    gfx_rrect(&cv, cx - s * 0.36f, cy - h / 2, w, h, w * 0.3f, col);
    gfx_rrect(&cv, cx + s * 0.08f, cy - h / 2, w, h, w * 0.3f, col);
}
static void icon_skip(float cx, float cy, float s, int fwd, uint32_t col) {
    float d = fwd ? 1 : -1;
    gfx_triangle(&cv, cx - d * s * 0.42f, cy - s * 0.42f, cx + d * s * 0.28f, cy, cx - d * s * 0.42f, cy + s * 0.42f, col);
    gfx_rrect(&cv, cx + d * s * 0.32f - s * 0.08f, cy - s * 0.42f, s * 0.16f, s * 0.84f, s * 0.05f, col);
}
static void icon_shuffle(float cx, float cy, float s, uint32_t col) {
    float t = s * 0.1f, a = s * 0.5f;
    gfx_line(&cv, cx - a, cy - a * 0.55f, cx + a * 0.7f, cy + a * 0.55f, t, col);
    gfx_line(&cv, cx - a, cy + a * 0.55f, cx + a * 0.7f, cy - a * 0.55f, t, col);
    gfx_triangle(&cv, cx + a * 0.55f, cy + a * 0.25f, cx + a * 1.05f, cy + a * 0.55f, cx + a * 0.55f, cy + a * 0.9f, col);
    gfx_triangle(&cv, cx + a * 0.55f, cy - a * 0.25f, cx + a * 1.05f, cy - a * 0.55f, cx + a * 0.55f, cy - a * 0.9f, col);
}
static void icon_repeat(float cx, float cy, float s, int one, uint32_t col) {
    float t = s * 0.1f, w = s * 0.9f, h = s * 0.62f;
    float x0 = cx - w / 2, x1 = cx + w / 2, y0 = cy - h / 2, y1 = cy + h / 2;
    gfx_line(&cv, x0 + h * 0.3f, y0, x1 - h * 0.15f, y0, t, col);
    gfx_line(&cv, x1, y0 + h * 0.2f, x1, y1 - h * 0.3f, t, col);
    gfx_line(&cv, x1 - h * 0.3f, y1, x0 + h * 0.15f, y1, t, col);
    gfx_line(&cv, x0, y1 - h * 0.2f, x0, y0 + h * 0.3f, t, col);
    gfx_triangle(&cv, x1 - h * 0.35f, y0 - h * 0.32f, x1 + h * 0.05f, y0, x1 - h * 0.35f, y0 + h * 0.32f, col);
    gfx_triangle(&cv, x0 + h * 0.35f, y1 - h * 0.32f, x0 - h * 0.05f, y1, x0 + h * 0.35f, y1 + h * 0.32f, col);
    if (one) {
        gfx_circle(&cv, cx, cy, s * 0.26f, C_BG);
        int w1 = text_width(FONT_SEMIBOLD, s * 0.42f, "1");
        text_draw(&cv, FONT_SEMIBOLD, s * 0.42f, cx - w1 / 2.0f, cy + s * 0.15f, "1", col, 0);
    }
}
static void icon_folder(float x, float y, float s, uint32_t col) {
    gfx_rrect(&cv, x, y + s * 0.12f, s * 0.45f, s * 0.2f, s * 0.06f, col);
    gfx_rrect(&cv, x, y + s * 0.22f, s, s * 0.7f, s * 0.1f, col);
}
static void icon_note(float x, float y, float s, uint32_t col) {
    gfx_circle(&cv, x + s * 0.32f, y + s * 0.76f, s * 0.18f, col);
    gfx_line(&cv, x + s * 0.47f, y + s * 0.74f, x + s * 0.47f, y + s * 0.12f, s * 0.09f, col);
    gfx_line(&cv, x + s * 0.47f, y + s * 0.14f, x + s * 0.78f, y + s * 0.3f, s * 0.09f, col);
}
static void icon_device(float x, float y, float s, uint32_t col) {
    gfx_rrect(&cv, x + s * 0.1f, y + s * 0.15f, s * 0.8f, s * 0.7f, s * 0.12f, col);
    gfx_rrect(&cv, x + s * 0.25f, y + s * 0.3f, s * 0.5f, s * 0.1f, s * 0.05f, C_BG);
}

// PlayStation face-button glyphs for the hint bar
static float btn_glyph(float x, float cy, int which) {
    float r = 9;
    switch (which) {
    case BTN_CROSS:
        gfx_line(&cv, x + 3, cy - 6, x + 15, cy + 6, 2.2f, PS_CROSS);
        gfx_line(&cv, x + 3, cy + 6, x + 15, cy - 6, 2.2f, PS_CROSS);
        return 18;
    case BTN_CIRCLE: gfx_ring(&cv, x + r, cy, 7.5f, 2.2f, PS_CIRCLE); return 18;
    case BTN_SQUARE:
        gfx_line(&cv, x + 3, cy - 6, x + 15, cy - 6, 2.2f, PS_SQUARE);
        gfx_line(&cv, x + 15, cy - 6, x + 15, cy + 6, 2.2f, PS_SQUARE);
        gfx_line(&cv, x + 15, cy + 6, x + 3, cy + 6, 2.2f, PS_SQUARE);
        gfx_line(&cv, x + 3, cy + 6, x + 3, cy - 6, 2.2f, PS_SQUARE);
        return 18;
    case BTN_TRIANGLE: gfx_triangle_outline(&cv, x + 9, cy - 7, x + 16.5f, cy + 6, x + 1.5f, cy + 6, 2.2f, PS_TRIANGLE); return 18;
    default: {
        const char *t = which == BTN_START ? "START" : which == BTN_SELECT ? "SELECT" : which == BTN_L ? "L" : which == BTN_R ? "R" : "\xE2\x97\x80\xE2\x96\xB6";
        if (which == (BTN_LEFT | BTN_RIGHT)) t = "\xE2\x86\x90\xE2\x86\x92";
        if (which == (BTN_L | BTN_R)) t = "L/R";
        int w = text_width(FONT_SEMIBOLD, 11, t) + 12;
        gfx_rrect(&cv, x, cy - 9, w, 18, 9, C_RAISED);
        text_draw(&cv, FONT_SEMIBOLD, 11, x + 6, cy + 4, t, C_TEXT2, 0);
        return (float)w;
    }
    }
}

typedef struct { int btn; const char *label; } hint;
static void draw_hints(const hint *h, int n) {
    float y = SCREEN_H - 20;
    gfx_rect(&cv, 0, SCREEN_H - 40, SCREEN_W, 1, C_LINE);
    // measure, then right-align
    float total = 0;
    for (int i = 0; i < n; i++) {
        int gw = (h[i].btn == BTN_START || h[i].btn == BTN_SELECT || h[i].btn == (BTN_L | BTN_R) || h[i].btn == (BTN_LEFT | BTN_RIGHT))
                     ? text_width(FONT_SEMIBOLD, 11, h[i].btn == BTN_START ? "START" : h[i].btn == BTN_SELECT ? "SELECT" : h[i].btn == (BTN_L | BTN_R) ? "L/R" : "\xE2\x86\x90\xE2\x86\x92") + 12
                     : 18;
        total += gw + 6 + text_width(FONT_REGULAR, 14, h[i].label) + (i < n - 1 ? 22 : 0);
    }
    float x = SCREEN_W - 32 - total;
    for (int i = 0; i < n; i++) {
        // single-button hints double as touch buttons
        if (!(h[i].btn & (h[i].btn - 1))) {
            int gw = h[i].btn == BTN_START || h[i].btn == BTN_SELECT ? text_width(FONT_SEMIBOLD, 11, h[i].btn == BTN_START ? "START" : "SELECT") + 12 : 18;
            float w = gw + 6 + text_width(FONT_REGULAR, 14, h[i].label);
            hot(x - 10, SCREEN_H - 36, w + 20, 32, 16, Z_BUTTON, h[i].btn);
        }
        x += btn_glyph(x, y, h[i].btn) + 6;
        x += text_draw(&cv, FONT_REGULAR, 14, x, y + 5, h[i].label, C_TEXT2, 0) + 22;
    }
}

static void draw_status(uint32_t fg) {
    char clk[16];
    plat_clock_text(clk, sizeof clk);
    int bat = plat_battery_percent();
    float x = SCREEN_W - 40;
    if (bat >= 0) {
        // battery glyph
        float bw = 26, bh = 13, by = 30;
        x -= bw + 3;
        gfx_rrect(&cv, x, by, bw, bh, 3.5f, WITH_ALPHA(fg, 90));
        gfx_rrect(&cv, x + bw + 1, by + 4, 2.5f, 5, 1, WITH_ALPHA(fg, 90));
        float fill = (bw - 4) * (bat < 0 ? 0 : bat > 100 ? 1 : bat / 100.0f);
        uint32_t bc = plat_battery_charging() ? C_ACCENT : bat <= 15 ? PS_CIRCLE : fg;
        if (fill > 1) gfx_rrect(&cv, x + 2, by + 2, fill, bh - 4, 2, bc);
        char pct[8];
        snprintf(pct, sizeof pct, "%d%%", bat);
        int pw = text_width(FONT_REGULAR, 14, pct);
        x -= pw + 8;
        text_draw(&cv, FONT_REGULAR, 14, x, 42, pct, WITH_ALPHA(fg, 200), 0);
    }
    int cw = text_width(FONT_SEMIBOLD, 15, clk);
    x -= cw + 16;
    text_draw(&cv, FONT_SEMIBOLD, 15, x, 42, clk, fg, 0);
}

static void fmt_time(char *b, size_t n, uint64_t frames, uint32_t rate) {
    uint64_t s = rate ? frames / rate : 0;
    if (s >= 3600) snprintf(b, n, "%d:%02d:%02d", (int)(s / 3600), (int)(s / 60 % 60), (int)(s % 60));
    else snprintf(b, n, "%d:%02d", (int)(s / 60), (int)(s % 60));
}

static void fmt_rate(char *b, size_t n, uint32_t rate) {
    if (rate % 1000 == 0) snprintf(b, n, "%u kHz", rate / 1000);
    else snprintf(b, n, "%.1f kHz", rate / 1000.0);
}

static float chip(float x, float y, const char *t, uint32_t bg, uint32_t fg) {
    int w = text_width(FONT_SEMIBOLD, 13, t) + 20;
    gfx_rrect(&cv, x, y, w, 26, 13, bg);
    text_draw(&cv, FONT_SEMIBOLD, 13, x + 10, y + 18, t, fg, 0);
    return w + 8;
}

// ---------------------------------------------------------------- track info
static void refresh_track(const player_status *s) {
    if (s->serial == cur_serial) return;
    cur_serial = s->serial;
    tags_free(&cur_tags);
    image_free(&cover_big);
    image_free(&cover_small);
    cover_tint = 0;
    if (!s->has_track && !s->path[0]) return;
    tags_read(s->path, &cur_tags, 1);
    if (cur_tags.cover_data) {
        if (image_from_cover(cur_tags.cover_data, cur_tags.cover_size, 300, &cover_big) == 0) {
            image_from_cover(cur_tags.cover_data, cur_tags.cover_size, 48, &cover_small);
            cover_tint = image_average(&cover_small);
        }
        free(cur_tags.cover_data);
        cur_tags.cover_data = NULL;
    }
    dirty = 1;
}

static void placeholder_cover(float x, float y, float s, float r) {
    gfx_rrect(&cv, x, y, s, s, r, C_RAISED);
    icon_note(x + s * 0.3f, y + s * 0.3f, s * 0.4f, C_TEXT3);
}

// ---------------------------------------------------------------- browser view
static void draw_browser(const player_status *s) {
    gfx_clear(&cv, C_BG);

    // header
    const char *title = dv.path[0] ? lib_basename(dv.path) : "Storage";
    if (!title[0]) title = dv.path; // device root like "ux0:"
    text_draw(&cv, FONT_SEMIBOLD, 26, 40, 46, title, C_TEXT, 520);
    if (dv.path[0]) {
        int tw = text_width(FONT_SEMIBOLD, 26, title);
        if (tw > 520) tw = 520;
        text_draw(&cv, FONT_REGULAR, 14, 40 + tw + 14, 45, dv.path, C_TEXT3, 760 - 40 - tw - 14 - 60);
    }
    draw_status(C_TEXT);

    // list
    int list_bottom = LIST_Y + ROW_H * LIST_ROWS;
    if (dv.n == 0) {
        const char *msg = dv.path[0] ? "No folders or audio files here" : "No storage found";
        int w = text_width(FONT_REGULAR, 18, msg);
        text_draw(&cv, FONT_REGULAR, 18, (SCREEN_W - w) / 2.0f, LIST_Y + 150, msg, C_TEXT3, 0);
    }
    if (dv.n) hot(0, LIST_Y, SCREEN_W, list_bottom - LIST_Y, -1, Z_LIST, 0);
    gfx_clip(&cv, 0, LIST_Y, SCREEN_W, list_bottom - LIST_Y);
    int first = (int)(list_y / ROW_H);
    float off = list_y - first * ROW_H;
    for (int i = 0; i <= LIST_ROWS && first + i < dv.n; i++) {
        int idx = first + i;
        lib_entry *e = &dv.items[idx];
        float y = LIST_Y + i * ROW_H - off;
        int sel = idx == dv.sel;
        if (sel) {
            gfx_rrect(&cv, 24, y + 3, SCREEN_W - 48, ROW_H - 6, 12, C_RAISED);
            gfx_rrect(&cv, 24, y + 15, 4, ROW_H - 30, 2, C_ACCENT);
        } else if (idx == pressed_row()) {
            gfx_rrect(&cv, 24, y + 3, SCREEN_W - 48, ROW_H - 6, 12, WITH_ALPHA(C_TEXT, 14));
        }
        uint32_t ic = sel ? C_ACCENT : C_TEXT3;
        if (!dv.path[0]) icon_device(44, y + 14, 24, ic);
        else if (e->is_dir) icon_folder(44, y + 14, 24, ic);
        else icon_note(44, y + 14, 24, ic);

        // highlight the file that's currently playing
        int playing_this = 0;
        if (!e->is_dir && s->has_track) {
            char full[1024];
            lib_join(full, sizeof full, dv.path, e->name);
            playing_this = !strcmp(full, s->path);
        }
        char name[256];
        snprintf(name, sizeof name, "%s", e->name);
        if (!e->is_dir) { char *dot = strrchr(name, '.'); if (dot) *dot = 0; }
        uint32_t tc = playing_this ? C_ACCENT : sel ? C_TEXT : col_mix(C_TEXT, C_TEXT2, 0.35f);
        text_draw(&cv, sel ? FONT_SEMIBOLD : FONT_REGULAR, 19, 84, y + ROW_H / 2 + 7, name, tc, 700);

        if (!e->is_dir) {
            const char *lab = format_label(e->fmt);
            int w = text_width(FONT_SEMIBOLD, 11, lab) + 14;
            float bx = SCREEN_W - 48 - w;
            gfx_rrect(&cv, bx, y + ROW_H / 2 - 10, w, 20, 6, e->fmt == FMT_FLAC || e->fmt == FMT_WAV ? WITH_ALPHA(C_ACCENT, 34) : C_SURFACE);
            text_draw(&cv, FONT_SEMIBOLD, 11, bx + 7, y + ROW_H / 2 + 4, lab, e->fmt == FMT_FLAC || e->fmt == FMT_WAV ? C_ACCENT : C_TEXT2, 0);
            if (playing_this) {
                // tiny equalizer bars
                float bxx = bx - 26;
                uint64_t t = plat_time_us() / 1000;
                for (int k = 0; k < 3; k++) {
                    float hh = s->paused ? 4 : 5 + 7 * (0.5f + 0.5f * sinf((float)t / 140.0f + k * 1.7f));
                    gfx_rrect(&cv, bxx + k * 6, y + ROW_H / 2 + 7 - hh, 4, hh, 1.5f, C_ACCENT);
                }
            }
        } else if (dv.path[0]) {
            // chevron
            float cx = SCREEN_W - 56, cy = y + ROW_H / 2;
            gfx_line(&cv, cx - 3, cy - 6, cx + 3, cy, 2, C_TEXT3);
            gfx_line(&cv, cx + 3, cy, cx - 3, cy + 6, 2, C_TEXT3);
        }
    }
    gfx_noclip(&cv);

    // scrollbar
    if (dv.n > LIST_ROWS) {
        float track_h = ROW_H * LIST_ROWS - 12;
        float th = track_h * LIST_ROWS / dv.n;
        if (th < 24) th = 24;
        float ty = LIST_Y + 6 + (track_h - th) * list_y / ((dv.n - LIST_ROWS) * ROW_H);
        gfx_rrect(&cv, SCREEN_W - 14, ty, 4, th, 2, WITH_ALPHA(C_TEXT, 50));
    }

    // mini player
    float my = SCREEN_H - 40 - 70;
    gfx_rrect(&cv, 24, my, SCREEN_W - 48, 62, 14, C_SURFACE);
    hot(24, my, SCREEN_W - 48, 62, 14, Z_BUTTON, BTN_TRIANGLE);
    if (s->has_track) {
        if (cover_small.px) gfx_image(&cv, &cover_small, 31, (int)my + 7, 8, 255);
        else placeholder_cover(31, my + 7, 48, 8);
        text_draw(&cv, FONT_SEMIBOLD, 17, 94, my + 28, cur_tags.title, C_TEXT, 560);
        const char *sub = cur_tags.artist[0] ? cur_tags.artist : "Unknown artist";
        text_draw(&cv, FONT_REGULAR, 14, 94, my + 48, sub, C_TEXT2, 560);
        char a[16], b[16], t[40];
        fmt_time(a, sizeof a, s->pos, s->rate);
        fmt_time(b, sizeof b, s->total, s->rate);
        snprintf(t, sizeof t, "%s / %s", a, b);
        int tw = text_width(FONT_REGULAR, 14, t);
        text_draw(&cv, FONT_REGULAR, 14, SCREEN_W - 100 - tw, my + 38, t, C_TEXT2, 0);
        hot(SCREEN_W - 94, my, 60, 62, 30, Z_BUTTON, BTN_START);
        gfx_circle(&cv, SCREEN_W - 64, my + 31, 18, C_ACCENT);
        if (s->paused) icon_play(SCREEN_W - 62, my + 31, 14, C_ACCENT_DK);
        else icon_pause(SCREEN_W - 64, my + 31, 13, C_ACCENT_DK);
        float pw = SCREEN_W - 48 - 28;
        float frac = s->total ? (float)s->pos / s->total : 0;
        gfx_rrect(&cv, 38, my + 58, pw, 2, 1, C_LINE);
        if (frac > 0) gfx_rrect(&cv, 38, my + 58, pw * frac, 2, 1, C_ACCENT);
    } else {
        placeholder_cover(31, my + 7, 48, 8);
        text_draw(&cv, FONT_REGULAR, 16, 94, my + 37, "Nothing playing \xE2\x80\x94 pick a track or press \xE2\x96\xA1 to play a folder", C_TEXT3, 780);
    }

    hint h[] = {
        { BTN_CROSS, dv.path[0] ? "Open / Play" : "Open" },
        { BTN_CIRCLE, "Back" },
        { BTN_SQUARE, "Play all" },
        { BTN_TRIANGLE, "Now playing" },
        { BTN_SELECT, "Screen off" },
    };
    draw_hints(h, dv.path[0] ? 5 : 2);
}

// ---------------------------------------------------------------- now playing view
static void draw_playing(const player_status *s) {
    uint32_t top = cover_tint ? col_mix(C_BG, cover_tint, 0.38f) : RGB(22, 26, 30);
    top = col_mix(top, RGB(0, 0, 0), 0.25f);
    gfx_vgradient(&cv, 0, 0, SCREEN_W, SCREEN_H, top, C_BG);

    text_draw(&cv, FONT_SEMIBOLD, 13, 60, 44, "NOW PLAYING", WITH_ALPHA(C_TEXT, 150), 0);
    if (s->count > 0 && s->index >= 0) {
        char q[32];
        snprintf(q, sizeof q, "%d / %d", s->index + 1, s->count);
        text_draw(&cv, FONT_REGULAR, 13, 60 + text_width(FONT_SEMIBOLD, 13, "NOW PLAYING") + 12, 44, q, WITH_ALPHA(C_TEXT, 110), 0);
    }
    draw_status(C_TEXT);

    // cover with soft shadow
    float cx = 60, cy = 82, cs = 300;
    for (int i = 6; i >= 1; i--) gfx_rrect(&cv, cx - i * 2, cy - i * 2 + 10, cs + i * 4, cs + i * 4, 16 + i * 2, RGBA(0, 0, 0, 14));
    if (cover_big.px) gfx_image(&cv, &cover_big, (int)cx, (int)cy, 16, 255);
    else placeholder_cover(cx, cy, cs, 16);

    float x = 410, w = SCREEN_W - x - 60;
    if (!s->has_track) {
        text_draw(&cv, FONT_SEMIBOLD, 30, x, 200, "Nothing playing", C_TEXT, (int)w);
        text_draw(&cv, FONT_REGULAR, 18, x, 236, "Choose music in the browser", C_TEXT2, (int)w);
    } else {
        if (cur_tags.album[0]) text_draw(&cv, FONT_REGULAR, 16, x, 118, cur_tags.album, WITH_ALPHA(C_TEXT, 150), (int)w);
        text_draw(&cv, FONT_SEMIBOLD, 32, x, 162, cur_tags.title, C_TEXT, (int)w);
        text_draw(&cv, FONT_REGULAR, 21, x, 196, cur_tags.artist[0] ? cur_tags.artist : "Unknown artist", WITH_ALPHA(C_TEXT, 200), (int)w);

        // format chips
        float chx = x, chy = 222;
        char buf[64], r1[16];
        chx += chip(chx, chy, format_label(s->fmt), WITH_ALPHA(C_TEXT, 26), C_TEXT);
        fmt_rate(r1, sizeof r1, s->rate);
        if (s->fmt == FMT_FLAC || s->fmt == FMT_WAV) snprintf(buf, sizeof buf, "%u-bit \xC2\xB7 %s", s->bits, r1);
        else snprintf(buf, sizeof buf, "%s", r1);
        chx += chip(chx, chy, buf, WITH_ALPHA(C_TEXT, 26), C_TEXT);
        int hires = s->bits > 16 || s->rate > 48000;
        if (hires) chx += chip(chx, chy, "HI-RES", WITH_ALPHA(C_GOLD, 40), C_GOLD);
        int lossless = s->fmt == FMT_FLAC || s->fmt == FMT_WAV;
        if (lossless && s->bits <= 16 && s->rate == s->out_rate) chip(chx, chy, "BIT-PERFECT", WITH_ALPHA(C_ACCENT, 40), C_ACCENT);
        else if (s->rate != s->out_rate) {
            char o[16], t[40];
            fmt_rate(o, sizeof o, s->out_rate);
            snprintf(t, sizeof t, "OUT %s / 16-bit", o);
            chip(chx, chy, t, WITH_ALPHA(C_TEXT, 16), C_TEXT2);
        } else if (lossless) chip(chx, chy, "OUT 16-bit", WITH_ALPHA(C_TEXT, 16), C_TEXT2);
    }

    // progress
    float py = 300;
    float frac = s->total ? (float)s->pos / s->total : 0;
    uint64_t pos = s->pos;
    if (scrub >= 0 && s->total) { frac = scrub; pos = (uint64_t)(scrub * s->total); }
    if (frac > 1) frac = 1;
    gfx_rrect(&cv, x, py, w, 6, 3, WITH_ALPHA(C_TEXT, 36));
    if (s->has_track) {
        hot(x - 16, py - 22, w + 32, 50, -1, Z_SEEK, 0);
        if (frac > 0) gfx_rrect(&cv, x, py, w * frac, 6, 3, C_ACCENT);
        gfx_circle(&cv, x + w * frac, py + 3, scrub >= 0 ? 11 : 8, C_TEXT);
        char a[16], b[16];
        fmt_time(a, sizeof a, pos, s->rate);
        uint64_t rem = s->total > pos ? s->total - pos : 0;
        b[0] = '-';
        fmt_time(b + 1, sizeof b - 1, rem, s->rate);
        text_draw(&cv, FONT_REGULAR, 14, x, py + 30, a, C_TEXT2, 0);
        int bw = text_width(FONT_REGULAR, 14, b);
        text_draw(&cv, FONT_REGULAR, 14, x + w - bw, py + 30, b, C_TEXT2, 0);
    }

    // transport
    float ty = 400, mid = x + w / 2;
    static const uint32_t tbtn[5] = { BTN_SQUARE, BTN_L, BTN_CROSS, BTN_R, BTN_TRIANGLE };
    for (int i = 0; i < 5; i++) hot(mid + (i - 2) * 95 - 42, ty - 42, 84, 84, 42, Z_BUTTON, tbtn[i]);
    uint32_t sh = s->shuffle ? C_ACCENT : WITH_ALPHA(C_TEXT, 120);
    icon_shuffle(mid - 190, ty, 22, sh);
    if (s->shuffle) gfx_circle(&cv, mid - 190, ty + 22, 2.5f, C_ACCENT);
    icon_skip(mid - 95, ty, 26, 0, C_TEXT);
    gfx_circle(&cv, mid, ty, 36, C_ACCENT);
    if (s->paused || !s->has_track) icon_play(mid + 3, ty, 28, C_ACCENT_DK);
    else icon_pause(mid, ty, 26, C_ACCENT_DK);
    icon_skip(mid + 95, ty, 26, 1, C_TEXT);
    uint32_t rc = s->repeat ? C_ACCENT : WITH_ALPHA(C_TEXT, 120);
    icon_repeat(mid + 190, ty, 24, s->repeat == REPEAT_ONE, rc);
    if (s->repeat) gfx_circle(&cv, mid + 190, ty + 22, 2.5f, C_ACCENT);

    hint h[] = {
        { BTN_CROSS, "Play/Pause" },
        { BTN_LEFT | BTN_RIGHT, "Seek" },
        { BTN_L | BTN_R, "Prev/Next" },
        { BTN_SQUARE, "Shuffle" },
        { BTN_TRIANGLE, "Repeat" },
        { BTN_CIRCLE, "Library" },
        { BTN_SELECT, "Screen off" },
    };
    draw_hints(h, 7);
}

static void draw_toast(void) {
    if (!toast[0] || plat_time_us() > toast_until) { toast[0] = 0; return; }
    int w = text_width(FONT_SEMIBOLD, 15, toast) + 36;
    float x = (SCREEN_W - w) / 2.0f, y = view == VIEW_PLAYING ? 452 : SCREEN_H - 40 - 70 - 48;
    gfx_rrect(&cv, x, y, w, 36, 18, RGBA(44, 48, 56, 240));
    text_draw(&cv, FONT_SEMIBOLD, 15, x + 18, y + 24, toast, C_TEXT, 0);
}

static void render(const player_status *s) {
    int stride;
    uint32_t *px = plat_backbuffer(&stride);
    gfx_begin(&cv, px, SCREEN_W, SCREEN_H, stride);
    nzones = 0;
    if (view == VIEW_BROWSER) draw_browser(s);
    else draw_playing(s);
    draw_toast();
    plat_present();
}

// ---------------------------------------------------------------- input
static int bit_index(uint32_t b) { int i = 0; while (b > 1) { b >>= 1; i++; } return i; }

// pressed now (edge) or auto-repeating while held
static int pressed(uint32_t b, uint32_t now, int repeat) {
    int i = bit_index(b);
    uint64_t t = plat_time_us();
    if ((now & b) && !(prev_buttons & b)) { hold_start[i] = hold_last[i] = t; return 1; }
    if (repeat && (now & b) && t - hold_start[i] > 380000 && t - hold_last[i] > 70000) { hold_last[i] = t; return 1; }
    return 0;
}

static float max_scroll(void) { return dv.n > LIST_ROWS ? (float)(dv.n - LIST_ROWS) * ROW_H : 0; }

static int clamp_list_y(void) {
    float m = max_scroll();
    if (list_y < 0) { list_y = 0; return 1; }
    if (list_y > m) { list_y = m; return 1; }
    return 0;
}

// scroll so the selection is on screen (buttons)
static void clamp_scroll(void) {
    if (dv.sel < 0) dv.sel = 0;
    if (dv.sel >= dv.n) dv.sel = dv.n ? dv.n - 1 : 0;
    if (dv.sel * ROW_H < list_y) list_y = (float)dv.sel * ROW_H;
    if ((dv.sel + 1) * ROW_H > list_y + LIST_ROWS * ROW_H) list_y = (float)(dv.sel + 1 - LIST_ROWS) * ROW_H;
    clamp_list_y();
    list_vel = 0;
}

// Touch scrolling leaves the selection where it was, even off screen.
// The first button press afterwards moves it to the nearest visible row.
static int reveal_selection(void) {
    if (!dv.n) return 0;
    int top = (int)ceilf(list_y / ROW_H - 0.01f);
    int bottom = (int)floorf((list_y + LIST_ROWS * ROW_H) / ROW_H + 0.01f) - 1;
    int old = dv.sel;
    if (dv.sel < top) dv.sel = top;
    if (dv.sel > bottom) dv.sel = bottom;
    return dv.sel != old;
}

static void open_dir(const char *path, const char *select_name) {
    if (lib_open(&dv, path) != 0) {
        show_toast("Can't open folder");
        lib_open(&dv, "");
    }
    if (select_name)
        for (int i = 0; i < dv.n; i++) if (!strcmp(dv.items[i].name, select_name)) { dv.sel = i; break; }
    list_y = (float)(dv.sel - LIST_ROWS / 2) * ROW_H;
    clamp_scroll();
    save_settings();
    dirty = 1;
}

static void play_folder(const char *dir, const char *start_file, int recursive) {
    char **list;
    int n = lib_collect(dir, recursive, &list);
    if (n == 0) { lib_free_list(list, n); show_toast("No playable files"); return; }
    int start = 0;
    if (start_file) for (int i = 0; i < n; i++) if (!strcmp(list[i], start_file)) { start = i; break; }
    player_set_queue(list, n, start);
    lib_free_list(list, n);
    if (recursive && n > 1) {
        char m[48];
        snprintf(m, sizeof m, "Playing %d tracks", n);
        show_toast(m);
    }
}

static const zone *zone_at(float x, float y) {
    for (int i = nzones - 1; i >= 0; i--) if (in_zone(&zones[i], x, y)) return &zones[i];
    return NULL;
}

static float seek_frac(float x) {
    float f = (x - (tc.z.x + 16)) / (tc.z.w - 32);
    return f < 0 ? 0 : f > 1 ? 1 : f;
}

// Returns buttons to inject this tick.
static uint32_t handle_touch(const player_status *s) {
    int x, y;
    int down = plat_touch(&x, &y);
    uint64_t now = plat_time_us();
    uint32_t out = 0;

    const zone *z = down && !tc.down && !tc.blocked ? zone_at(x, y) : NULL;
    if (down && !tc.down && !tc.blocked && !z) { // dead area: ignore until lifted
        tc.blocked = 1;
        list_vel = 0;
    }

    if (tc.blocked) { // also a finger left over from screen-off / app switch
        if (!down) tc.blocked = 0;
        tc.down = 0;
    } else if (down && !tc.down) {
        memset(&tc, 0, sizeof tc);
        tc.down = 1;
        tc.z = *z;
        tc.x0 = tc.x = x; tc.y0 = tc.y = y;
        tc.t_move = now;
        tc.caught = fabsf(list_vel) > 60; // finger stops a fling; that's not a tap
        list_vel = 0;
        if (z->kind == Z_SEEK) scrub = seek_frac(x);
        dirty = 1;
    } else if (down) {
        float dy = y - tc.y;
        if (fabsf(x - tc.x0) > TAP_SLOP || fabsf(y - tc.y0) > TAP_SLOP) tc.moved = 1;
        if (tc.z.kind == Z_LIST && tc.moved && dy != 0) {
            float dt = (now - tc.t_move) / 1e6f;
            list_y -= dy;
            clamp_list_y();
            if (dt > 0) tc.vel = tc.vel * 0.6f + (-dy / dt) * 0.4f;
            tc.t_move = now;
        }
        if (tc.z.kind == Z_SEEK) scrub = seek_frac(x);
        if (x != tc.x || y != tc.y) dirty = 1;
        tc.x = x; tc.y = y;
    } else if (tc.down) {
        tc.down = 0;
        dirty = 1;
        switch (tc.z.kind) {
        case Z_SEEK:
            if (s->has_track && s->total) player_seek_to((uint64_t)(scrub * s->total));
            scrub = -1;
            break;
        case Z_LIST:
            if (tc.moved) {
                // fling, unless the finger rested before lifting
                if (now - tc.t_move < 80000 && fabsf(tc.vel) > 150) list_vel = tc.vel;
            } else if (!tc.caught && list_row_at(tc.y) >= 0) {
                dv.sel = list_row_at(tc.y);
                out = BTN_CROSS;
            }
            break;
        case Z_BUTTON:
            if (in_zone(&tc.z, tc.x, tc.y)) out = tc.z.btn;
            break;
        }
    }

    // fling momentum
    if (!tc.down && list_vel != 0) {
        float dt = (now - last_tick) / 1e6f;
        if (dt > 0.05f) dt = 0.05f;
        list_y += list_vel * dt;
        list_vel *= expf(-4.0f * dt);
        if (clamp_list_y() || fabsf(list_vel) < 30) list_vel = 0;
        dirty = 1;
    }
    last_tick = now;
    return out;
}

static void handle_input(uint32_t b, player_status *s) {
    // global
    if (pressed(BTN_START, b, 0)) { player_toggle_pause(); dirty = 1; }
    if (pressed(BTN_L, b, 0)) { player_prev(); dirty = 1; }
    if (pressed(BTN_R, b, 0)) { player_next(); dirty = 1; }

    if (view == VIEW_BROWSER) {
        int up = pressed(BTN_UP, b, 1), down = pressed(BTN_DOWN, b, 1);
        int left = pressed(BTN_LEFT, b, 1), right = pressed(BTN_RIGHT, b, 1);
        int cross = pressed(BTN_CROSS, b, 0);
        if ((up || down || left || right || cross) && reveal_selection()) {
            up = down = left = right = cross = 0;
            dirty = 1;
        }
        if (up) { dv.sel--; if (dv.sel < 0) dv.sel = dv.n - 1; clamp_scroll(); dirty = 1; }
        if (down) { dv.sel++; if (dv.sel >= dv.n) dv.sel = 0; clamp_scroll(); dirty = 1; }
        if (left) { dv.sel -= LIST_ROWS; clamp_scroll(); dirty = 1; }
        if (right) { dv.sel += LIST_ROWS; clamp_scroll(); dirty = 1; }
        if (cross && dv.n) {
            lib_entry *e = &dv.items[dv.sel];
            char full[1024];
            if (!dv.path[0]) snprintf(full, sizeof full, "%s", e->name);
            else lib_join(full, sizeof full, dv.path, e->name);
            if (e->is_dir) open_dir(full, NULL);
            else { play_folder(dv.path, full, 0); view = VIEW_PLAYING; dirty = 1; }
        }
        if (pressed(BTN_CIRCLE, b, 0) && dv.path[0]) {
            char parent[1024], child[256];
            snprintf(child, sizeof child, "%s", lib_basename(dv.path));
            if (!child[0]) snprintf(child, sizeof child, "%s", dv.path);
            lib_parent(dv.path, parent, sizeof parent);
            open_dir(parent, child);
        }
        if (pressed(BTN_SQUARE, b, 0) && dv.path[0]) {
            if (dv.n && dv.items[dv.sel].is_dir) {
                char full[1024];
                lib_join(full, sizeof full, dv.path, dv.items[dv.sel].name);
                play_folder(full, NULL, 1);
            } else play_folder(dv.path, NULL, 1);
            dirty = 1;
        }
        if (pressed(BTN_TRIANGLE, b, 0)) { view = VIEW_PLAYING; dirty = 1; }
    } else {
        if (pressed(BTN_CROSS, b, 0)) { player_toggle_pause(); dirty = 1; }
        if (pressed(BTN_LEFT, b, 1)) { player_seek_rel(-10); dirty = 1; }
        if (pressed(BTN_RIGHT, b, 1)) { player_seek_rel(10); dirty = 1; }
        if (pressed(BTN_SQUARE, b, 0)) {
            player_set_shuffle(!s->shuffle);
            show_toast(!s->shuffle ? "Shuffle on" : "Shuffle off");
            save_settings();
        }
        if (pressed(BTN_TRIANGLE, b, 0)) {
            player_cycle_repeat();
            int r = (s->repeat + 1) % 3;
            show_toast(r == REPEAT_OFF ? "Repeat off" : r == REPEAT_ALL ? "Repeat all" : "Repeat one");
            save_settings();
        }
        if (pressed(BTN_CIRCLE, b, 0)) { view = VIEW_BROWSER; dirty = 1; }
    }
}

// ---------------------------------------------------------------- lifecycle
int app_init(const char *asset_dir) {
    char p[512];
    snprintf(p, sizeof p, "%s/Inter-Regular.otf", asset_dir);
    if (font_load(FONT_REGULAR, p)) return -1;
    snprintf(p, sizeof p, "%s/Inter-SemiBold.otf", asset_dir);
    if (font_load(FONT_SEMIBOLD, p)) return -1;

    plat_mkdir(plat_data_dir());
    snprintf(settings_path, sizeof settings_path, "%s/settings.txt", plat_data_dir());

    char logp[512];
    snprintf(logp, sizeof logp, "%s/log.txt", plat_data_dir());
    remove(logp); // fresh log each launch
    plat_log("--- Fidelity start");
    player_init();
    char dir[1024];
    snprintf(dir, sizeof dir, "%s", plat_root_dir());
    int sh = 0, rep = 0;
    load_settings(dir, sizeof dir, &sh, &rep);
    player_set_modes(sh, rep);
    if (lib_open(&dv, dir) != 0) lib_open(&dv, plat_root_dir());
    return 0;
}

void app_force_redraw(void) { dirty = 1; }

int app_step(uint32_t b) {
    player_status s;
    player_get_status(&s);

    if ((s.has_track && !s.paused) || screen_off) plat_keep_awake();

    // Never leave the backlight at 0 if the user jumps out (PS button)
    // or the console wakes from sleep.
    if (plat_focus_event()) {
        tc.blocked = 1;
        scrub = -1;
        if (screen_off) {
            screen_off = 0;
            plat_display_on();
            dirty = 1;
        }
    }

    if (screen_off) {
        // pocket mode: only transport controls, SELECT wakes the screen
        if (pressed(BTN_START, b, 0)) player_toggle_pause();
        if (pressed(BTN_L, b, 0)) player_prev();
        if (pressed(BTN_R, b, 0)) player_next();
        if (pressed(BTN_SELECT, b, 0)) {
            screen_off = 0;
            plat_display_on();
            tc.blocked = 1;
            dirty = 1;
        }
        prev_buttons = b;
        plat_sleep_us(20000);
        return 0;
    }

    b |= handle_touch(&s);

    if (pressed(BTN_SELECT, b, 0)) {
        screen_off = 1;
        tc.down = 0;
        tc.blocked = 1;
        scrub = -1;
        list_vel = 0;
        // black frame first so nothing flashes when it comes back
        int stride;
        uint32_t *px = plat_backbuffer(&stride);
        gfx_begin(&cv, px, SCREEN_W, SCREEN_H, stride);
        gfx_clear(&cv, RGB(0, 0, 0));
        plat_present();
        plat_display_off();
        prev_buttons = b;
        return 0;
    }

    handle_input(b, &s);
    prev_buttons = b;
    player_get_status(&s);
    refresh_track(&s);

    uint64_t now = plat_time_us();
    // redraw on change, while a toast is up, ~4x/s for the clock and progress,
    // and faster in the browser while the equalizer bars animate
    int animating = view == VIEW_BROWSER && s.has_track && !s.paused;
    uint64_t interval = animating ? 66000 : 250000;
    if (dirty || toast[0] || now - last_draw > interval) {
        render(&s);
        last_draw = now;
        dirty = 0;
    } else {
        plat_sleep_us(16000);
    }
    return 0;
}
