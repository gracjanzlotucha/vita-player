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

// Now playing (Figma "Now Playing" frame)
#define NP_TEXT2    RGB(153, 153, 153)
#define NP_LINE     RGBA(255, 255, 255, 26)

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
static image np_bg; // Now playing background, rebuilt per track

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

// PlayStation face-button glyphs for the hint bar. The modern style (Now
// playing) is monochrome and a little smaller; the classic one is coloured.
static const char *btn_text(int which) {
    if (which == (BTN_LEFT | BTN_RIGHT)) return "\xE2\x86\x90\xE2\x86\x92";
    if (which == (BTN_L | BTN_R)) return "L/R";
    return which == BTN_START ? "START" : which == BTN_SELECT ? "SELECT" : which == BTN_L ? "L" : which == BTN_R ? "R" : NULL;
}

static float btn_glyph_w(int which, int modern) {
    const char *t = btn_text(which);
    if (!t) return modern ? 14 : 18;
    return modern ? text_width(FONT_GEIST_MEDIUM, 10, t) + 12 : text_width(FONT_SEMIBOLD, 11, t) + 12;
}

static float btn_glyph(float x, float cy, int which, int modern) {
    float k = modern ? 0.75f : 1, t = modern ? 1.5f : 2.2f;
    uint32_t mono = NP_TEXT2;
    switch (which) {
    case BTN_CROSS:
        gfx_line(&cv, x + 3 * k, cy - 6 * k, x + 15 * k, cy + 6 * k, t, modern ? mono : PS_CROSS);
        gfx_line(&cv, x + 3 * k, cy + 6 * k, x + 15 * k, cy - 6 * k, t, modern ? mono : PS_CROSS);
        break;
    case BTN_CIRCLE: gfx_ring(&cv, x + 9 * k, cy, 7.5f * k, t, modern ? mono : PS_CIRCLE); break;
    case BTN_SQUARE: {
        uint32_t c = modern ? mono : PS_SQUARE;
        float a = 3 * k, b = 15 * k, h = 6 * k;
        gfx_line(&cv, x + a, cy - h, x + b, cy - h, t, c);
        gfx_line(&cv, x + b, cy - h, x + b, cy + h, t, c);
        gfx_line(&cv, x + b, cy + h, x + a, cy + h, t, c);
        gfx_line(&cv, x + a, cy + h, x + a, cy - h, t, c);
        break;
    }
    case BTN_TRIANGLE:
        gfx_triangle_outline(&cv, x + 9 * k, cy - 7 * k, x + 16.5f * k, cy + 6 * k, x + 1.5f * k, cy + 6 * k, t, modern ? mono : PS_TRIANGLE);
        break;
    default: {
        const char *s = btn_text(which);
        float w = btn_glyph_w(which, modern);
        if (modern) {
            gfx_rrect(&cv, x, cy - 8, w, 16, 8, RGBA(255, 255, 255, 22));
            text_draw(&cv, FONT_GEIST_MEDIUM, 10, x + 6, cy + 3.5f, s, mono, 0);
        } else {
            gfx_rrect(&cv, x, cy - 9, w, 18, 9, C_RAISED);
            text_draw(&cv, FONT_SEMIBOLD, 11, x + 6, cy + 4, s, C_TEXT2, 0);
        }
        break;
    }
    }
    return btn_glyph_w(which, modern);
}

typedef struct { int btn; const char *label; } hint;
// Right-aligned hint row. Classic: below a divider at the bottom of the
// screen. Modern: inside the Now playing bottom panel (508..544).
static void draw_hints(const hint *h, int n, int modern) {
    int face = modern ? FONT_GEIST : FONT_REGULAR;
    float size = modern ? 12 : 14, gap = modern ? 20 : 22, sp = modern ? 5 : 6;
    float y = modern ? 526 : SCREEN_H - 20;
    uint32_t col = modern ? NP_TEXT2 : C_TEXT2;
    if (!modern) gfx_rect(&cv, 0, SCREEN_H - 40, SCREEN_W, 1, C_LINE);
    // measure, then right-align
    float total = 0;
    for (int i = 0; i < n; i++)
        total += btn_glyph_w(h[i].btn, modern) + sp + text_width(face, size, h[i].label) + (i < n - 1 ? gap : 0);
    float x = (modern ? SCREEN_W - 36 : SCREEN_W - 32) - total;
    float base = modern ? y + 4 : y + 5;
    for (int i = 0; i < n; i++) {
        // single-button hints double as touch buttons
        if (!(h[i].btn & (h[i].btn - 1))) {
            float w = btn_glyph_w(h[i].btn, modern) + sp + text_width(face, size, h[i].label);
            hot(x - 10, modern ? 510 : SCREEN_H - 36, w + 20, modern ? 32 : 32, 16, Z_BUTTON, h[i].btn);
        }
        x += btn_glyph(x, y, h[i].btn, modern) + sp;
        x += text_draw(&cv, face, size, x, base, h[i].label, col, 0) + gap;
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


// ---------------------------------------------------------------- track info
// Now playing background, per the Figma "Background" layer: the cover in a
// 1051x1050 frame at (-46,-253), saturation +29%, 128 px layer blur, a faint
// noise, at 20% opacity over black. Built once per track into a full-screen
// image, so each frame is a plain copy. The blur runs on a 64x64 grid (the
// cover is that soft anyway) with transparent padding, so the edges fade out
// like a Figma layer blur does.
static void build_np_bg(const image *cov) {
    enum { G = 64, PAD = 14, N = G + 2 * PAD };
    const float fx = -46, fy = -253, fw = 1051, fh = 1050;
    image_free(&np_bg);
    np_bg.px = malloc(SCREEN_W * SCREEN_H * 4);
    if (!np_bg.px) return;
    np_bg.w = SCREEN_W; np_bg.h = SCREEN_H;
    if (!cov->px) {
        for (int i = 0; i < SCREEN_W * SCREEN_H; i++) np_bg.px[i] = RGB(0, 0, 0);
        return;
    }
    float *g = calloc(N * N * 4, sizeof *g), *t = calloc(N * N * 4, sizeof *t);
    if (!g || !t) { free(g); free(t); image_free(&np_bg); return; }
    // area-average the cover into the grid; premultiplied rgb + alpha
    for (int gy = 0; gy < G; gy++)
        for (int gx = 0; gx < G; gx++) {
            int x0 = gx * cov->w / G, x1 = (gx + 1) * cov->w / G, y0 = gy * cov->h / G, y1 = (gy + 1) * cov->h / G;
            float r = 0, gg = 0, b = 0;
            int n = 0;
            for (int y = y0; y < y1; y++)
                for (int x = x0; x < x1; x++, n++) {
                    uint32_t p = cov->px[y * cov->w + x];
                    r += p & 255; gg += (p >> 8) & 255; b += (p >> 16) & 255;
                }
            if (!n) continue;
            r /= n; gg /= n; b /= n;
            float l = 0.299f * r + 0.587f * gg + 0.114f * b; // saturation +29%
            float *o = &g[((gy + PAD) * N + gx + PAD) * 4];
            o[0] = l + (r - l) * 1.29f; o[1] = l + (gg - l) * 1.29f; o[2] = l + (b - l) * 1.29f; o[3] = 1;
            for (int k = 0; k < 3; k++) o[k] = o[k] < 0 ? 0 : o[k] > 255 ? 255 : o[k];
        }
    // separable gaussian; Figma's blur radius is about two sigmas
    float sigma = 64.0f / (fw / G), kw[2 * PAD + 1], ksum = 0;
    for (int i = -PAD; i <= PAD; i++) ksum += kw[i + PAD] = expf(-(float)(i * i) / (2 * sigma * sigma));
    for (int i = 0; i <= 2 * PAD; i++) kw[i] /= ksum;
    for (int pass = 0; pass < 2; pass++) {
        float *src = pass ? t : g, *dst = pass ? g : t;
        for (int y = 0; y < N; y++)
            for (int x = 0; x < N; x++)
                for (int c = 0; c < 4; c++) {
                    float acc = 0;
                    for (int k = -PAD; k <= PAD; k++) {
                        int xx = pass ? x : x + k, yy = pass ? y + k : y;
                        if (xx < 0 || yy < 0 || xx >= N || yy >= N) continue;
                        acc += src[(yy * N + xx) * 4 + c] * kw[k + PAD];
                    }
                    dst[(y * N + x) * 4 + c] = acc;
                }
    }
    // upscale bilinearly to the screen, 20% over black, plus noise that also
    // breaks up banding in such a dark gradient
    uint32_t seed = 0x9E3779B9u;
    for (int y = 0; y < SCREEN_H; y++) {
        float v = (y - fy) / fh * G + PAD - 0.5f;
        int iy = (int)floorf(v);
        float ty = v - iy;
        for (int x = 0; x < SCREEN_W; x++) {
            float u = (x - fx) / fw * G + PAD - 0.5f;
            int ix = (int)floorf(u);
            float tx = u - ix;
            const float *p00 = &g[(iy * N + ix) * 4], *p01 = p00 + 4, *p10 = p00 + N * 4, *p11 = p10 + 4;
            seed = seed * 1664525u + 1013904223u;
            float n1 = (seed >> 8) * (1.0f / 16777216.0f);
            seed = seed * 1664525u + 1013904223u;
            float n2 = (seed >> 8) * (1.0f / 16777216.0f);
            uint32_t out = 0xFF000000u;
            for (int c = 0; c < 3; c++) {
                float a = p00[c] + (p01[c] - p00[c]) * tx, b = p10[c] + (p11[c] - p10[c]) * tx;
                float val = (a + (b - a) * ty) * 0.2f * (0.95f + 0.05f * n1) + n2 - 0.5f;
                int q = (int)(val + 0.5f);
                out |= (uint32_t)(q < 0 ? 0 : q > 255 ? 255 : q) << (c * 8);
            }
            np_bg.px[y * SCREEN_W + x] = out;
        }
    }
    free(g); free(t);
}

static void refresh_track(const player_status *s) {
    if (s->serial == cur_serial) return;
    cur_serial = s->serial;
    tags_free(&cur_tags);
    image_free(&cover_big);
    image_free(&cover_small);
    image_free(&np_bg);
    if (!s->has_track && !s->path[0]) return;
    tags_read(s->path, &cur_tags, 1);
    if (cur_tags.cover_data) {
        if (image_from_cover(cur_tags.cover_data, cur_tags.cover_size, 400, &cover_big) == 0) {
            image_from_cover(cur_tags.cover_data, cur_tags.cover_size, 48, &cover_small);
        }
        free(cur_tags.cover_data);
        cur_tags.cover_data = NULL;
    }
    build_np_bg(&cover_big);
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
    draw_hints(h, dv.path[0] ? 5 : 2, 0);
}

// ---------------------------------------------------------------- now playing view
// Layout, colours and icons follow the Figma "Now Playing" frame (960x544).
// Icon paths are exported from it; pause has no Figma counterpart and is drawn
// in the same style (a 60 px disc with the symbol cut out).
static const char *PATH_PLAY = "M36 66C52.5684 66 66 52.5684 66 36C66 19.4314 52.5684 6 36 6C19.4314 6 6 19.4314 6 36C6 52.5684 19.4314 66 36 66ZM32.3451 26.3489C31.3494 25.67 30 26.3831 30 27.5882V44.4117C30 45.6168 31.3494 46.3299 32.3451 45.651L44.6823 37.2393C45.5556 36.6438 45.5556 35.3562 44.6823 34.7607L32.3451 26.3489Z";
static const char *PATH_PAUSE = "M36 66C52.5684 66 66 52.5684 66 36C66 19.4314 52.5684 6 36 6C19.4314 6 6 19.4314 6 36C6 52.5684 19.4314 66 36 66ZM30 26H32C33.1046 26 34 26.8954 34 28V44C34 45.1046 33.1046 46 32 46H30C28.8954 46 28 45.1046 28 44V28C28 26.8954 28.8954 26 30 26ZM40 26H42C43.1046 26 44 26.8954 44 28V44C44 45.1046 43.1046 46 42 46H40C38.8954 46 38 45.1046 38 44V28C38 26.8954 38.8954 26 40 26Z";
static const char *PATH_NEXT = "M9.44211 5.77893C7.68771 4.61058 5.33331 5.86509 5.33331 7.97738V24.0228C5.33331 26.1351 7.68771 27.3896 9.44211 26.2212L21.489 18.1985C23.0592 17.1529 23.0592 14.8472 21.489 13.8016L9.44211 5.77893Z"
                               "M26.6667 6.66674C26.6667 5.93035 26.0697 5.3334 25.3333 5.3334C24.5969 5.3334 24 5.93035 24 6.66674V25.3333C24 26.0697 24.5969 26.6667 25.3333 26.6667C26.0697 26.6667 26.6667 26.0697 26.6667 25.3333V6.66674Z";
static const char *PATH_PREV = "M22.5579 26.2211C24.3123 27.3894 26.6667 26.1349 26.6667 24.0226L26.6667 7.97719C26.6667 5.86493 24.3123 4.61039 22.5579 5.77879L10.511 13.8015C8.94082 14.8471 8.94082 17.1528 10.511 18.1984L22.5579 26.2211Z"
                               "M5.33333 25.3333C5.33333 26.0697 5.93027 26.6666 6.66667 26.6666C7.40307 26.6666 8 26.0697 8 25.3333L8 6.66666C8 5.93026 7.40307 5.33333 6.66667 5.33333C5.93027 5.33333 5.33333 5.93026 5.33333 6.66666L5.33333 25.3333Z";
static const char *PATH_SHUFFLE = "M17.2929 3.29289C17.6834 2.90237 18.3166 2.90237 18.7071 3.29289L21.7071 6.29289C22.0976 6.68342 22.0976 7.31658 21.7071 7.70711L18.7071 10.7071C18.3166 11.0976 17.6834 11.0976 17.2929 10.7071C16.9024 10.3166 16.9024 9.68342 17.2929 9.29289L18.5858 8H16.8284C16.5632 8 16.3089 8.10536 16.1213 8.29289L6.29289 18.1213C5.73028 18.6839 4.96722 19 4.17157 19H3C2.44772 19 2 18.5523 2 18C2 17.4477 2.44772 17 3 17H4.17157C4.43679 17 4.69114 16.8946 4.87868 16.7071L14.7071 6.87868C15.2697 6.31607 16.0328 6 16.8284 6H18.5858L17.2929 4.70711C16.9024 4.31658 16.9024 3.68342 17.2929 3.29289ZM2 6C2 5.44772 2.44772 5 3 5H4.17157C4.96722 5 5.73028 5.31607 6.29289 5.87868L8.70711 8.29289C9.09763 8.68342 9.09763 9.31658 8.70711 9.70711C8.31658 10.0976 7.68342 10.0976 7.29289 9.70711L4.87868 7.29289C4.69114 7.10536 4.43679 7 4.17157 7H3C2.44772 7 2 6.55228 2 6ZM17.2929 13.2929C17.6834 12.9024 18.3166 12.9024 18.7071 13.2929L21.7071 16.2929C22.0976 16.6834 22.0976 17.3166 21.7071 17.7071L18.7071 20.7071C18.3166 21.0976 17.6834 21.0976 17.2929 20.7071C16.9024 20.3166 16.9024 19.6834 17.2929 19.2929L18.5858 18H16.8284C16.0328 18 15.2697 17.6839 14.7071 17.1213L13.2929 15.7071C12.9024 15.3166 12.9024 14.6834 13.2929 14.2929C13.6834 13.9024 14.3166 13.9024 14.7071 14.2929L16.1213 15.7071C16.3089 15.8946 16.5632 16 16.8284 16H18.5858L17.2929 14.7071C16.9024 14.3166 16.9024 13.6834 17.2929 13.2929Z";
static const char *PATH_REPEAT = "M17.9571 2.29289C17.5666 1.90237 16.9334 1.90237 16.5429 2.29289C16.1524 2.68342 16.1524 3.31658 16.5429 3.70711L17.8358 5H6C4.34315 5 3 6.34315 3 8V11C3 11.5523 3.44772 12 4 12C4.55228 12 5 11.5523 5 11V8C5 7.44772 5.44772 7 6 7H17.8358L16.5429 8.29289C16.1524 8.68342 16.1524 9.31658 16.5429 9.70711C16.9334 10.0976 17.5666 10.0976 17.9571 9.70711L20.4268 7.23744C21.1102 6.55402 21.1102 5.44598 20.4268 4.76256L17.9571 2.29289Z"
                                 "M20 12C20.5523 12 21 12.4477 21 13V16C21 17.6569 19.6569 19 18 19H6.1641L7.45699 20.2929C7.84752 20.6834 7.84752 21.3166 7.45699 21.7071C7.06647 22.0976 6.4333 22.0976 6.04278 21.7071L3.57311 19.2374C2.88969 18.554 2.88969 17.446 3.57311 16.7626L6.04278 14.2929C6.4333 13.9024 7.06647 13.9024 7.45699 14.2929C7.84752 14.6834 7.84752 15.3166 7.45699 15.7071L6.1641 17H18C18.5523 17 19 16.5523 19 16V13C19 12.4477 19.4477 12 20 12Z";
static mask ic_play, ic_pause, ic_prev, ic_next, ic_shuffle, ic_repeat;

static void np_icons_init(void) {
    mask_from_path(&ic_play, 72, 72, 1, PATH_PLAY, 1);
    mask_from_path(&ic_pause, 72, 72, 1, PATH_PAUSE, 1);
    mask_from_path(&ic_prev, 32, 32, 1, PATH_PREV, 0);
    mask_from_path(&ic_next, 32, 32, 1, PATH_NEXT, 0);
    mask_from_path(&ic_shuffle, 24, 24, 1, PATH_SHUFFLE, 1);
    mask_from_path(&ic_repeat, 24, 24, 1, PATH_REPEAT, 0);
}

// Text placed by its line box top, as Figma does (line_h <= 0: "auto").
static int np_text(int face, float size, float line_h, float x, float top, const char *t, uint32_t col, int max_w) {
    return text_draw(&cv, face, size, x, top + font_baseline(face, size, line_h), t, col, max_w);
}

// "03:52", or "1:02:03" past an hour
static void np_time(char *b, size_t n, uint64_t frames, uint32_t rate) {
    uint64_t s = rate ? frames / rate : 0;
    if (s >= 3600) snprintf(b, n, "%d:%02d:%02d", (int)(s / 3600), (int)(s / 60 % 60), (int)(s % 60));
    else snprintf(b, n, "%02d:%02d", (int)(s / 60), (int)(s % 60));
}

// Pill tag: 24 px high, 8 px padding, Geist Medium 12. Returns the x after it.
static float np_tag(float x, float y, const char *t, uint32_t bg, uint32_t fg) {
    int w = text_width(FONT_GEIST_MEDIUM, 12, t) + 16;
    gfx_rrect(&cv, x, y, w, 24, 12, bg);
    np_text(FONT_GEIST_MEDIUM, 12, 16, x + 8, y + 4, t, fg, 0);
    return x + w + 8;
}

static void draw_playing(const player_status *s) {
    if (np_bg.px) gfx_blit(&cv, &np_bg, 0, 0);
    else gfx_clear(&cv, RGB(0, 0, 0));

    // top bar: 36 high, 36 px side padding, hairline below
    gfx_rect(&cv, 0, 35, SCREEN_W, 1, NP_LINE);
    char top[48], clk[16], right[48];
    if (s->count > 0 && s->index >= 0) snprintf(top, sizeof top, "Now Playing %d/%d", s->index + 1, s->count);
    else snprintf(top, sizeof top, "Now Playing");
    np_text(FONT_GEIST, 12, 0, 36, 9.5f, top, C_TEXT, 0);
    plat_clock_text(clk, sizeof clk);
    int bat = plat_battery_percent();
    if (bat >= 0) snprintf(right, sizeof right, "%s  \xE2\x80\xA2  %d%%%s", clk, bat, plat_battery_charging() ? " \xE2\x9A\xA1" : "");
    else snprintf(right, sizeof right, "%s", clk);
    np_text(FONT_GEIST, 12, 0, SCREEN_W - 36 - text_width(FONT_GEIST, 12, right), 9.5f, right, C_TEXT, 0);

    // artwork 400x400, radius 20
    if (cover_big.px) gfx_image(&cv, &cover_big, 36, 72, 20, 255);
    else {
        gfx_rrect(&cv, 36, 72, 400, 400, 20, RGB(28, 28, 30));
        icon_note(36 + 130, 72 + 130, 140, RGB(72, 72, 76));
    }

    // details: title / artist / album line, 7 px apart
    const float x = 472, w = 452;
    char line[600];
    if (!s->has_track) {
        np_text(FONT_GEIST, 32, 36, x, 92, "Not Playing", C_TEXT, (int)w);
        np_text(FONT_GEIST, 20, 0, x, 135, "Pick something in the library", NP_TEXT2, (int)w);
    } else {
        np_text(FONT_GEIST, 32, 36, x, 92, cur_tags.title, C_TEXT, (int)w);
        np_text(FONT_GEIST, 20, 0, x, 135, cur_tags.artist[0] ? cur_tags.artist : "Unknown Artist", NP_TEXT2, (int)w);
        line[0] = 0;
        if (cur_tags.album[0] && cur_tags.year[0]) snprintf(line, sizeof line, "%s \xE2\x80\xA2 %s", cur_tags.album, cur_tags.year);
        else snprintf(line, sizeof line, "%s", cur_tags.album[0] ? cur_tags.album : cur_tags.year);
        np_text(FONT_GEIST, 14, 0, x, 168, line, NP_TEXT2, (int)w);

        // tags: [Hi-Res] Lossless, format, bit depth / rate
        float tx = x;
        int lossless = s->fmt == FMT_FLAC || s->fmt == FMT_WAV;
        int hires = s->bits > 16 || s->rate > 48000;
        if (lossless && hires) tx = np_tag(tx, 206, "Hi-Res Lossless", RGBA(255, 140, 64, 26), RGB(255, 181, 96));
        else if (lossless) tx = np_tag(tx, 206, "Lossless", RGBA(255, 255, 255, 26), C_TEXT);
        tx = np_tag(tx, 206, format_label(s->fmt), RGBA(255, 255, 255, 26), C_TEXT);
        char rate[16], depth[40];
        if (s->rate % 1000 == 0) snprintf(rate, sizeof rate, "%u kHz", s->rate / 1000);
        else snprintf(rate, sizeof rate, "%.1f kHz", s->rate / 1000.0);
        if (lossless) snprintf(depth, sizeof depth, "%u bit \xE2\x80\xA2 %s", s->bits, rate);
        else snprintf(depth, sizeof depth, "%s", rate);
        np_tag(tx, 206, depth, RGBA(255, 255, 255, 26), C_TEXT);
    }

    // timeline: 8 px bar (12 px while scrubbing), times 10 px below
    float frac = s->total ? (float)s->pos / s->total : 0;
    uint64_t pos = s->pos;
    if (scrub >= 0 && s->total) { frac = scrub; pos = (uint64_t)(scrub * s->total); }
    if (frac > 1) frac = 1;
    float bh = scrub >= 0 ? 12 : 8, by = 330 - bh / 2;
    if (s->has_track) hot(x - 16, 306, w + 32, 52, -1, Z_SEEK, 0);
    gfx_rrect(&cv, x, by, w, bh, bh / 2, RGBA(255, 255, 255, 90)); // Figma: 30% additive
    if (s->has_track && frac > 0) gfx_rrect(&cv, x, by, w * frac < bh ? bh : w * frac, bh, bh / 2, RGB(255, 255, 255)); // 70% additive saturates to white
    if (s->has_track) {
        char a[16], b[16];
        np_time(a, sizeof a, pos, s->rate);
        np_time(b, sizeof b, s->total, s->rate);
        np_text(FONT_GEIST_MEDIUM, 12, 0, x, 344, a, NP_TEXT2, 0);
        np_text(FONT_GEIST_MEDIUM, 12, 0, x + w - text_width(FONT_GEIST_MEDIUM, 12, b), 344, b, NP_TEXT2, 0);
    }

    // controls row (top 380, 72 high): shuffle | prev play next | repeat
    // Touch zones are larger than the icons; active shuffle/repeat get a pill.
    hot(456, 388, 56, 56, 28, Z_BUTTON, BTN_SQUARE);
    hot(578, 384, 64, 64, 32, Z_BUTTON, BTN_L);
    hot(658, 376, 80, 80, 40, Z_BUTTON, BTN_CROSS);
    hot(754, 384, 64, 64, 32, Z_BUTTON, BTN_R);
    hot(884, 388, 56, 56, 28, Z_BUTTON, BTN_TRIANGLE);
    if (s->shuffle) gfx_rrect(&cv, 466, 398, 36, 36, 10, RGBA(255, 255, 255, 30));
    gfx_mask(&cv, &ic_shuffle, 472, 404, s->shuffle ? RGB(255, 255, 255) : RGBA(255, 255, 255, 128));
    gfx_mask(&cv, &ic_prev, 594, 400, RGBA(255, 255, 255, 179));
    gfx_mask(&cv, s->paused || !s->has_track ? &ic_play : &ic_pause, 662, 380, RGBA(255, 255, 255, 230));
    gfx_mask(&cv, &ic_next, 770, 400, RGBA(255, 255, 255, 179));
    if (s->repeat) gfx_rrect(&cv, 894, 398, 36, 36, 10, RGBA(255, 255, 255, 30));
    gfx_mask(&cv, &ic_repeat, 900, 404, s->repeat ? RGB(255, 255, 255) : RGBA(255, 255, 255, 128));
    if (s->repeat == REPEAT_ONE) { // "1" badge on the pill's corner
        gfx_circle(&cv, 927, 401, 7, RGB(255, 255, 255));
        int ow = text_width(FONT_GEIST_MEDIUM, 10, "1");
        np_text(FONT_GEIST_MEDIUM, 10, 14, 927 - ow / 2.0f, 394, "1", RGB(20, 20, 22), 0);
    }

    // bottom panel: hairline on top, button hints inside
    gfx_rect(&cv, 0, 508, SCREEN_W, 1, NP_LINE);
    hint h[] = {
        { BTN_CROSS, "Play/Pause" },
        { BTN_LEFT | BTN_RIGHT, "Seek" },
        { BTN_L | BTN_R, "Prev/Next" },
        { BTN_SQUARE, "Shuffle" },
        { BTN_TRIANGLE, "Repeat" },
        { BTN_CIRCLE, "Library" },
        { BTN_SELECT, "Screen off" },
    };
    draw_hints(h, 7, 1);
}

static void draw_toast(void) {
    if (!toast[0] || plat_time_us() > toast_until) { toast[0] = 0; return; }
    if (view == VIEW_PLAYING) { // under the controls, centred on the right column
        int w = text_width(FONT_GEIST_MEDIUM, 13, toast) + 28;
        float x = 698 - w / 2.0f, y = 466;
        gfx_rrect(&cv, x, y, w, 30, 15, RGBA(48, 48, 52, 235));
        np_text(FONT_GEIST_MEDIUM, 13, 30, x + 14, y, toast, C_TEXT, 0);
        return;
    }
    int w = text_width(FONT_SEMIBOLD, 15, toast) + 36;
    float x = (SCREEN_W - w) / 2.0f, y = SCREEN_H - 40 - 70 - 48;
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
    if (font_load(FONT_REGULAR, p, 0)) return -1;
    snprintf(p, sizeof p, "%s/Inter-SemiBold.otf", asset_dir);
    if (font_load(FONT_SEMIBOLD, p, 0)) return -1;
    snprintf(p, sizeof p, "%s/Geist-Regular.ttf", asset_dir);
    if (font_load(FONT_GEIST, p, 1)) return -1;
    snprintf(p, sizeof p, "%s/Geist-Medium.ttf", asset_dir);
    if (font_load(FONT_GEIST_MEDIUM, p, 1)) return -1;
    np_icons_init();

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
