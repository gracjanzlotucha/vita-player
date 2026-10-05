#include "app.h"
#include "platform.h"
#include "gfx.h"
#include "player.h"
#include "catalog.h"
#include "tags.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

// Layout, colours and type follow Gracjan's Figma file (frames are 960x544,
// so coordinates map 1:1): "Now Playing" (1:2) and "Library" (14:292).
// The album and artist pages, the Tracks/Artists/Settings tabs and the
// empty states aren't in Figma yet and are designed to match.

// ---------------------------------------------------------------- palette
#define C_WHITE     RGB(255, 255, 255)
#define C_TEXT      RGB(255, 255, 255)
#define C_TEXT2     RGB(153, 153, 153)          // #999
#define C_DOT       RGBA(153, 153, 153, 153)    // separators: #999 at 60%
#define C_LINE      RGBA(255, 255, 255, 26)     // hairlines: 3% + 7% dodge
#define C_PILL      RGBA(255, 255, 255, 26)
#define C_ROW_SEL   RGB(20, 20, 20)             // #141414
#define C_PLACEH    RGB(28, 28, 30)

// ---------------------------------------------------------------- state
enum { V_LIBRARY, V_ALBUM, V_ARTIST, V_PLAYING };
enum { TAB_ALBUMS, TAB_TRACKS, TAB_ARTISTS, TAB_SETTINGS, NTABS };
static const char *tab_names[NTABS] = { "Albums", "Tracks", "Artists", "Settings" };

// Virtual buttons for touch targets that have no physical button of their
// own in the current view (bits above the real ones).
#define VB_PREV  (1u << 12)
#define VB_NEXT  (1u << 13)
#define VB_TAB0  (1u << 14) // ... VB_TAB0 << 3

static canvas cv;
static int view = V_LIBRARY, tab = TAB_ALBUMS;
static int np_back = V_LIBRARY;              // where O goes from Now playing
static int page_album = -1, page_artist = -1; // open album / artist page
static uint32_t page_album_key;
static int album_from_artist;                // album page opened from an artist
static int dirty = 1, screen_off;
static uint64_t last_draw;

static uint32_t prev_buttons;
static uint64_t hold_start[32], hold_last[32];

static catalog *cat;
static int cur_track = -1;                   // catalog index of the playing file

static uint32_t cur_serial = (uint32_t)-1;
static track_tags cur_tags;
static image cover_big, cover_small;
static image np_bg, mini_bg;                 // blurred cover backgrounds, per track
static int np_static_dirty = 1, mini_dirty = 1;

static char toast[96];
static uint64_t toast_until;
static char settings_path[512];

#define MINI_X 132
#define MINI_Y 428
#define MINI_W 696
#define MINI_H 64

// ---------------------------------------------------------------- lists
// Every list (tabs, album page, artist page) scrolls in pixels; touch drags
// and flings it, buttons move the selection and keep it on screen.
typedef struct { int sel; float y, vel; } list_t;
static list_t lst_tab[NTABS], lst_album, lst_artist;

static struct {
    list_t *l;
    int n;
    float top, row, bottom; // drawn area (rows pass under the mini player)
    float view_bottom;      // rows above this count as visible
} L;

static int mini_visible(const player_status *s) { return s->has_track && view != V_PLAYING; }

static void list_geom(const player_status *s) {
    memset(&L, 0, sizeof L);
    int ntr = cat ? cat->ntracks : 0, nal = cat ? cat->nalbums : 0, nar = cat ? cat->nartists : 0;
    L.top = 52; L.row = 52; L.bottom = 508;
    if (view == V_LIBRARY) {
        L.l = &lst_tab[tab];
        L.n = tab == TAB_ALBUMS ? nal : tab == TAB_TRACKS ? ntr : tab == TAB_ARTISTS ? nar : 3;
    } else if (view == V_ALBUM && cat && page_album >= 0) {
        L.l = &lst_album; L.n = cat->albums[page_album].ntracks;
        L.top = 188; L.row = 44;
    } else if (view == V_ARTIST && cat && page_artist >= 0) {
        L.l = &lst_artist; L.n = cat->artists[page_artist].nalbums;
    }
    L.view_bottom = mini_visible(s) ? MINI_Y - 8 : L.bottom;
}

static float max_scroll(void) {
    float content = L.n * L.row + (L.bottom - L.view_bottom) + 8;
    float m = content - (L.bottom - L.top);
    return m > 0 ? m : 0;
}

static int clamp_list_y(void) {
    if (!L.l) return 0;
    float m = max_scroll();
    if (L.l->y < 0) { L.l->y = 0; return 1; }
    if (L.l->y > m) { L.l->y = m; return 1; }
    return 0;
}

// scroll so the selection is fully visible (buttons)
static void clamp_scroll(void) {
    if (!L.l) return;
    list_t *l = L.l;
    if (l->sel >= L.n) l->sel = L.n - 1;
    if (l->sel < 0) l->sel = 0;
    float vis = L.view_bottom - L.top;
    if (l->sel * L.row < l->y) l->y = l->sel * L.row;
    if ((l->sel + 1) * L.row > l->y + vis) l->y = (l->sel + 1) * L.row - vis;
    clamp_list_y();
    l->vel = 0;
}

// Touch scrolling leaves the selection where it was, even off screen.
// The first button press afterwards moves it to the nearest visible row.
static int reveal_selection(void) {
    if (!L.l || !L.n) return 0;
    list_t *l = L.l;
    int top = (int)ceilf(l->y / L.row - 0.01f);
    int bottom = (int)floorf((l->y + L.view_bottom - L.top) / L.row + 0.01f) - 1;
    if (bottom >= L.n) bottom = L.n - 1;
    int old = l->sel;
    if (l->sel < top) l->sel = top;
    if (l->sel > bottom) l->sel = bottom;
    return l->sel != old;
}

// ---------------------------------------------------------------- touch
// Every tap ends up as a (virtual) button press for one tick, so touch and
// buttons share the same code paths. Draw code registers what is tappable
// as it draws, so hit areas always match what is on screen.
enum { Z_BUTTON, Z_LIST, Z_SEEK };
typedef struct { float x, y, w, h; int kind; uint32_t btn; } zone;
static zone zones[48];
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
        gfx_rrect(&cv, x, y, w, h, r, WITH_ALPHA(C_WHITE, 26));
}

static int list_row_at(float y) {
    if (!L.l || y < L.top || y >= L.bottom) return -1;
    int r = (int)floorf((y - L.top + L.l->y) / L.row);
    return r >= 0 && r < L.n ? r : -1;
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
    fprintf(f, "tab=%d\nshuffle=%d\nrepeat=%d\n", tab, s.shuffle, s.repeat);
    fclose(f);
}

static void load_settings(int *sh, int *rep) {
    FILE *f = fopen(settings_path, "r");
    if (!f) return;
    char line[256];
    while (fgets(line, sizeof line, f)) {
        if (!strncmp(line, "tab=", 4)) { int t = atoi(line + 4); if (t >= 0 && t < NTABS) tab = t; }
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

// ---------------------------------------------------------------- text
// Text placed by its line box top, as Figma does (line_h <= 0: "auto").
static int text_at(int face, float size, float line_h, float x, float top, const char *t, uint32_t col, int max_w) {
    return text_draw(&cv, face, size, x, top + font_baseline(face, size, line_h), t, col, max_w);
}

// A run of differently coloured pieces in the pixel font (bars, hints).
typedef struct { const char *t; uint32_t col; } seg;
static float segs_width(const seg *s, int n) {
    float w = 0;
    for (int i = 0; i < n; i++) w += text_width(FONT_PIXEL, 12, s[i].t);
    return w;
}
static float segs_draw(float x, float top, const seg *s, int n) {
    for (int i = 0; i < n; i++) x += text_at(FONT_PIXEL, 12, 0, x, top, s[i].t, s[i].col, 0);
    return x;
}

// "03:52", or "1:02:03" past an hour
static void fmt_secs(char *b, size_t n, uint64_t s) {
    if (s >= 3600) snprintf(b, n, "%d:%02d:%02d", (int)(s / 3600), (int)(s / 60 % 60), (int)(s % 60));
    else snprintf(b, n, "%02d:%02d", (int)(s / 60), (int)(s % 60));
}

static void fmt_quality(char *b, size_t n, audio_format fmt, uint32_t bits, uint32_t rate) {
    char r[16];
    if (rate % 1000 == 0) snprintf(r, sizeof r, "%u kHz", rate / 1000);
    else snprintf(r, sizeof r, "%.1f kHz", rate / 1000.0);
    if ((fmt == FMT_FLAC || fmt == FMT_WAV) && bits) snprintf(b, n, "%u bit \xE2\x80\xA2 %s", bits, r);
    else snprintf(b, n, "%s", rate ? r : "");
}

// Pill: Geist Pixel 12 / 12, 8 px padding, 20 high.
static float pill_w(const char *t) { return text_width(FONT_PIXEL, 12, t) + 16; }
static float pill(float x, float y, const char *t, uint32_t bg, uint32_t fg) {
    float w = pill_w(t);
    gfx_rrect(&cv, x, y, w, 20, 10, bg);
    text_at(FONT_PIXEL, 12, 12, x + 8, y + 4, t, fg, 0);
    return w;
}
// right-aligned row of pills ending at x_right; returns the left edge
static float pills_right(float x_right, float y, const char **t, int n) {
    float w = 0;
    for (int i = 0; i < n; i++) if (t[i] && t[i][0]) w += pill_w(t[i]) + (w > 0 ? 8 : 0);
    float x = x_right - w;
    for (int i = 0; i < n; i++) if (t[i] && t[i][0]) x += pill(x, y, t[i], C_PILL, C_TEXT) + 8;
    return x_right - w;
}

// ---------------------------------------------------------------- bars
static void draw_clock(void) {
    char clk[16], pct[16];
    plat_clock_text(clk, sizeof clk);
    int bat = plat_battery_percent();
    seg s[3] = { { clk, C_TEXT }, { " \xE2\x80\xA2 ", C_DOT }, { pct, C_TEXT } };
    int n = 1;
    if (bat >= 0) { snprintf(pct, sizeof pct, "%d%%%s", bat, plat_battery_charging() ? "+" : ""); n = 3; }
    segs_draw(SCREEN_W - 36 - segs_width(s, n), 9.5f, s, n);
}

// Top bar on library views: tabs with the selected one underlined, or a
// breadcrumb on album/artist pages.
static void draw_top_tabs(void) {
    gfx_rect(&cv, 0, 35, SCREEN_W, 1, C_LINE);
    float x = 36;
    if (view == V_LIBRARY) {
        for (int i = 0; i < NTABS; i++) {
            float w = text_width(FONT_PIXEL, 12, tab_names[i]);
            hot(x - 12, 0, w + 24, 36, 8, Z_BUTTON, VB_TAB0 << i);
            text_at(FONT_PIXEL, 12, 0, x, 9.5f, tab_names[i], i == tab ? C_TEXT : C_TEXT2, 0);
            if (i == tab) gfx_rect(&cv, x, 35, w, 1, C_WHITE);
            x += w + 24;
        }
    } else {
        const char *root = view == V_ARTIST || album_from_artist ? "Artists" : "Albums";
        const char *name = view == V_ALBUM ? cat->albums[page_album].title : cat->artists[page_artist].name;
        seg s[3] = { { root, C_TEXT2 }, { " / ", C_DOT }, { "", C_TEXT } };
        float w = segs_draw(x, 9.5f, s, 2);
        text_at(FONT_PIXEL, 12, 0, w, 9.5f, name, C_TEXT, 700);
    }
    draw_clock();
}

// Bottom bar hints: the key in grey (pixel-font letters, with small drawn
// shapes for square and triangle, which the font doesn't have), the label
// in white. Hints with a single button are touch targets too.
typedef struct { const char *key; const char *label; uint32_t btn; } hint;

static float key_w(const char *k) {
    if (!strcmp(k, "SQ") || !strcmp(k, "TRI")) return 9;
    return text_width(FONT_PIXEL, 12, k);
}
static void key_draw(float x, const char *k) {
    float base = 518.5f + font_baseline(FONT_PIXEL, 12, 0);
    if (!strcmp(k, "SQ")) { // 1 px square outline, cap height
        gfx_rect(&cv, (int)x, (int)base - 8, 8, 1, C_TEXT2);
        gfx_rect(&cv, (int)x, (int)base - 1, 8, 1, C_TEXT2);
        gfx_rect(&cv, (int)x, (int)base - 8, 1, 8, C_TEXT2);
        gfx_rect(&cv, (int)x + 7, (int)base - 8, 1, 8, C_TEXT2);
    } else if (!strcmp(k, "TRI")) {
        gfx_triangle_outline(&cv, x + 4.5f, base - 8.5f, x + 8.5f, base - 0.5f, x + 0.5f, base - 0.5f, 1.1f, C_TEXT2);
    } else {
        text_at(FONT_PIXEL, 12, 0, x, 518.5f, k, C_TEXT2, 0);
    }
}
static float hint_w(const hint *h) { return key_w(h->key) + text_width(FONT_PIXEL, 12, " ") + text_width(FONT_PIXEL, 12, h->label); }
static void hint_draw(float x, const hint *h) {
    float w = hint_w(h);
    if (h->btn && !(h->btn & (h->btn - 1))) hot(x - 8, 510, w + 16, 32, 10, Z_BUTTON, h->btn);
    key_draw(x, h->key);
    text_at(FONT_PIXEL, 12, 0, x + key_w(h->key) + text_width(FONT_PIXEL, 12, " "), 518.5f, h->label, C_TEXT, 0);
}

// Bottom bar: 36 high, hairline on top, one hint on the left, the rest
// right-aligned 16 px apart. Library views fill it black (rows pass under).
static void draw_bottom(const hint *left, const hint *right, int nr, int fill) {
    if (fill) gfx_rect(&cv, 0, 508, SCREEN_W, 36, RGB(0, 0, 0));
    gfx_rect(&cv, 0, 508, SCREEN_W, 1, C_LINE);
    if (left) hint_draw(36, left);
    float w = 0;
    for (int i = 0; i < nr; i++) w += hint_w(&right[i]) + (i ? 16 : 0);
    float x = SCREEN_W - 36 - w;
    for (int i = 0; i < nr; i++) { hint_draw(x, &right[i]); x += hint_w(&right[i]) + 16; }
}

static void icon_note(float x, float y, float s, uint32_t col) {
    gfx_circle(&cv, x + s * 0.32f, y + s * 0.76f, s * 0.18f, col);
    gfx_line(&cv, x + s * 0.47f, y + s * 0.74f, x + s * 0.47f, y + s * 0.12f, s * 0.09f, col);
    gfx_line(&cv, x + s * 0.47f, y + s * 0.14f, x + s * 0.78f, y + s * 0.3f, s * 0.09f, col);
}

static void art_or_placeholder(const image *img, float x, float y, int size, float r) {
    if (img && img->px && img->w == size) gfx_image(&cv, img, (int)x, (int)y, r, 255);
    else {
        gfx_rrect(&cv, x, y, size, size, r, C_PLACEH);
        icon_note(x + size * 0.28f, y + size * 0.28f, size * 0.44f, RGB(72, 72, 76));
    }
}

// three little bars marking what's playing
static void playing_mark(float x, float cy) {
    static const float h[3] = { 6, 10, 8 };
    for (int i = 0; i < 3; i++) gfx_rect(&cv, (int)x + i * 4, (int)(cy + 5 - h[i]), 2, (int)h[i], C_WHITE);
}

// ---------------------------------------------------------------- track info
static void find_current_track(const player_status *s) {
    cur_track = -1;
    if (!cat || !s->path[0]) return;
    for (int i = 0; i < cat->ntracks; i++)
        if (!strcmp(cat->tracks[i].path, s->path)) { cur_track = i; break; }
}

// Tags, cover art and the Now playing backgrounds are prepared on a worker
// thread (its own core on the Vita), so a track change never stalls the UI:
// it keeps the previous info for the few ms until the new tags arrive, and
// the art follows when decoded. Art is identified by a hash of the picture
// bytes, so tracks sharing a cover (an album) reuse it without decoding.

// Blurred cover backgrounds, per the Figma "Background" layers: the cover
// drawn into the rect (fx, fy, fw, fh) relative to the output, saturation
// +29%, 128 px layer blur, a faint noise, at `opacity` over black. Now
// playing: 960x544 with the cover at (-46,-253) 1051x1050; mini player:
// 696x64 with a 1156 px cover centred. The blur runs on a 64x64 grid (the
// cover is that soft anyway) with transparent padding, so the edges fade
// out like a Figma layer blur does.
static int build_bg(const image *cov, image *out, int ow, int oh, float fx, float fy, float fw, float fh, float opacity) {
    enum { G = 64, PAD = 14, N = G + 2 * PAD };
    out->px = malloc((size_t)ow * oh * 4);
    if (!out->px) return -1;
    out->w = ow; out->h = oh;
    float *g = calloc(N * N * 3, sizeof *g), *t = calloc(N * N * 3, sizeof *t);
    if (!g || !t) { free(g); free(t); image_free(out); return -1; }
    // area-average the cover into the grid (premultiplied: padding is 0)
    for (int gy = 0; gy < G; gy++)
        for (int gx = 0; gx < G; gx++) {
            int x0 = gx * cov->w / G, x1 = (gx + 1) * cov->w / G, y0 = gy * cov->h / G, y1 = (gy + 1) * cov->h / G;
            uint32_t r = 0, gg = 0, b = 0, n = 0;
            for (int y = y0; y < y1; y++)
                for (int x = x0; x < x1; x++, n++) {
                    uint32_t p = cov->px[y * cov->w + x];
                    r += p & 255; gg += (p >> 8) & 255; b += (p >> 16) & 255;
                }
            if (!n) continue;
            float fr = (float)r / n, fg = (float)gg / n, fb = (float)b / n;
            float l = 0.299f * fr + 0.587f * fg + 0.114f * fb; // saturation +29%
            float *o = &g[((gy + PAD) * N + gx + PAD) * 3];
            o[0] = l + (fr - l) * 1.29f; o[1] = l + (fg - l) * 1.29f; o[2] = l + (fb - l) * 1.29f;
            for (int k = 0; k < 3; k++) o[k] = o[k] < 0 ? 0 : o[k] > 255 ? 255 : o[k];
        }
    // separable gaussian; Figma's blur radius is about two sigmas
    float sigma = 64.0f / (fw / G), kw[2 * PAD + 1], ksum = 0;
    for (int i = -PAD; i <= PAD; i++) ksum += kw[i + PAD] = expf(-(float)(i * i) / (2 * sigma * sigma));
    for (int i = 0; i <= 2 * PAD; i++) kw[i] /= ksum;
    for (int pass = 0; pass < 2; pass++) {
        float *src = pass ? t : g, *dst = pass ? g : t;
        int step = pass ? N * 3 : 3;
        for (int y = 0; y < N; y++)
            for (int x = 0; x < N; x++) {
                int c0 = pass ? y : x, lo = c0 - PAD < 0 ? -c0 : -PAD, hi = c0 + PAD >= N ? N - 1 - c0 : PAD;
                const float *p = &src[(y * N + x) * 3];
                float a0 = 0, a1 = 0, a2 = 0;
                for (int k = lo; k <= hi; k++) {
                    const float *q = p + k * step;
                    float w = kw[k + PAD];
                    a0 += q[0] * w; a1 += q[1] * w; a2 += q[2] * w;
                }
                float *o = &dst[(y * N + x) * 3];
                o[0] = a0; o[1] = a1; o[2] = a2;
            }
    }
    // upscale bilinearly: interpolate each screen row once across the grid,
    // then along it. The noise also breaks up banding in a gradient this dark.
    static int col_ix[SCREEN_W];
    static float col_t[SCREEN_W];
    for (int x = 0; x < ow; x++) {
        float u = (x - fx) / fw * G + PAD - 0.5f;
        col_ix[x] = (int)floorf(u);
        col_t[x] = u - col_ix[x];
    }
    float row[N * 3];
    uint32_t seed = 0x9E3779B9u;
    for (int y = 0; y < oh; y++) {
        float v = (y - fy) / fh * G + PAD - 0.5f;
        int iy = (int)floorf(v);
        float ty = v - iy;
        const float *r0 = &g[iy * N * 3], *r1 = r0 + N * 3;
        for (int i = 0; i < N * 3; i++) row[i] = (r0[i] + (r1[i] - r0[i]) * ty) * opacity;
        uint32_t *o = &out->px[y * ow];
        for (int x = 0; x < ow; x++) {
            const float *a = &row[col_ix[x] * 3];
            float tx = col_t[x];
            seed = seed * 1664525u + 1013904223u;
            float grain = 0.95f + (seed >> 24) * (0.05f / 255.0f), dith = ((seed >> 8) & 255) * (1.0f / 255.0f);
            uint32_t px = 0xFF000000u;
            for (int c = 0; c < 3; c++) {
                int q = (int)((a[c] + (a[c + 3] - a[c]) * tx) * grain + dith);
                px |= (uint32_t)(q > 255 ? 255 : q) << (c * 8);
            }
            o[x] = px;
        }
    }
    free(g); free(t);
    return 0;
}

static uint32_t art_hash(const uint8_t *p, size_t n) {
    uint32_t h = 2166136261u ^ (uint32_t)n;
    size_t step = n > 65536 ? n / 65536 : 1; // sample big pictures
    for (size_t i = 0; i < n; i += step) h = (h ^ p[i]) * 16777619u;
    return h ? h : 1;
}

static plat_mutex *ld_m;
// request (UI -> worker)
static char ld_path[1024];
static int ld_req;
static uint32_t ld_req_id, ld_shown_art;
// results (worker -> UI)
static track_tags ld_tags;
static uint32_t ld_tags_id, ld_tags_art; // ld_tags_id 0 = nothing new
static image ld_big, ld_small, ld_bg, ld_mini;
static uint32_t ld_art;                 // art id of ld_big & co, 0 = none

static int loader_thread(void *arg) {
    (void)arg;
    char path[1024];
    for (;;) {
        plat_mutex_lock(ld_m);
        int have = ld_req;
        uint32_t id = ld_req_id, shown = ld_shown_art, ready = ld_art;
        if (have) snprintf(path, sizeof path, "%s", ld_path);
        ld_req = 0;
        plat_mutex_unlock(ld_m);
        if (!have) { plat_sleep_us(15000); continue; }

        track_tags t;
        tags_read(path, &t, 1);
        uint8_t *cover = t.cover_data;
        size_t cover_n = t.cover_size;
        t.cover_data = NULL;
        uint32_t art = cover ? art_hash(cover, cover_n) : 0;

        plat_mutex_lock(ld_m);
        ld_tags = t; ld_tags_art = art; ld_tags_id = id;
        int newer = ld_req;
        plat_mutex_unlock(ld_m);

        if (art && art != shown && art != ready && !newer) {
            image big = { 0 }, small = { 0 }, bg = { 0 }, mini = { 0 };
            plat_cpu_boost(1);
            if (image_from_cover(cover, cover_n, 400, &big) == 0) {
                image_scale(&big, 48, &small);
                build_bg(&big, &bg, SCREEN_W, SCREEN_H, -46, -253, 1051, 1050, 0.25f);
                build_bg(&big, &mini, MINI_W, MINI_H, -229, -546, 1156, 1156, 0.25f);
            }
            plat_cpu_boost(0);
            plat_mutex_lock(ld_m);
            image_free(&ld_big); image_free(&ld_small); image_free(&ld_bg); image_free(&ld_mini); // never collected
            ld_big = big; ld_small = small; ld_bg = bg; ld_mini = mini;
            ld_art = big.px ? art : 0;
            plat_mutex_unlock(ld_m);
        }
        free(cover);
    }
    return 0;
}

static char cur_path[1024];
static uint32_t cur_req_id, want_art, shown_art;
static int last_has_track = -1;

static void show_art(image *big, image *small, image *bg, image *mini, uint32_t id) {
    image_free(&cover_big); image_free(&cover_small); image_free(&np_bg); image_free(&mini_bg);
    cover_big = *big; cover_small = *small; np_bg = *bg; mini_bg = *mini;
    memset(big, 0, sizeof *big); memset(small, 0, sizeof *small); memset(bg, 0, sizeof *bg); memset(mini, 0, sizeof *mini);
    shown_art = id;
    np_static_dirty = mini_dirty = 1;
    dirty = 1;
}

static void clear_art(void) {
    image a = { 0 }, b2 = { 0 }, c = { 0 }, d = { 0 };
    show_art(&a, &b2, &c, &d, 0);
}

// Called every tick: asks the loader for a new track, collects its results.
static void refresh_track(const player_status *s) {
    if (s->serial != cur_serial) {
        cur_serial = s->serial;
        np_static_dirty = mini_dirty = 1; // format, rate, bits come with the status
        find_current_track(s);
        if (s->path[0] && strcmp(s->path, cur_path)) {
            snprintf(cur_path, sizeof cur_path, "%s", s->path);
            plat_mutex_lock(ld_m);
            snprintf(ld_path, sizeof ld_path, "%s", s->path);
            ld_req = 1;
            ld_req_id = ++cur_req_id;
            plat_mutex_unlock(ld_m);
        }
    }
    if (s->has_track != last_has_track) { last_has_track = s->has_track; np_static_dirty = mini_dirty = 1; }

    plat_mutex_lock(ld_m);
    if (ld_tags_id) {
        if (ld_tags_id == cur_req_id) {
            tags_free(&cur_tags);
            cur_tags = ld_tags;
            want_art = ld_tags_art;
            np_static_dirty = mini_dirty = 1;
            dirty = 1;
            if (!want_art && shown_art) clear_art();
            else if (want_art != shown_art && ld_art != want_art) clear_art(); // new art still decoding
        } else {
            tags_free(&ld_tags);
        }
        ld_tags_id = 0;
    }
    if (ld_art && ld_art == want_art && want_art != shown_art) {
        show_art(&ld_big, &ld_small, &ld_bg, &ld_mini, ld_art);
        ld_art = 0;
    }
    ld_shown_art = shown_art;
    plat_mutex_unlock(ld_m);
}

// ---------------------------------------------------------------- library
// One list row, per the Figma "Album Item": 888 x 52, art 36 (r8) inset 8,
// title Geist 14/18, subtitle 12/12 #999, pills on the right (16 px in),
// selected / pressed rows get a #141414 background (r16).
static void draw_row(float y, int hl, int has_art, const image *art, int round_art,
                     const char *title, const char *sub, const char **pills, int npills, int playing) {
    if (hl) gfx_rrect(&cv, 36, y, 888, 52, 16, C_ROW_SEL);
    float tx = 52;
    if (has_art) { art_or_placeholder(art, 44, y + 8, 36, round_art ? 18 : 8); tx = 92; }
    float pl = pills_right(908, y + 16, pills, npills);
    if (playing) { pl -= 22; playing_mark(pl, y + 26); }
    int maxw = (int)(pl - 16 - tx);
    if (sub) {
        text_at(FONT_GEIST, 14, 18, tx, y + 8, title, C_TEXT, maxw);
        text_at(FONT_GEIST, 12, 12, tx, y + 32, sub, C_TEXT2, maxw);
    } else {
        text_at(FONT_GEIST, 14, 18, tx, y + 17, title, C_TEXT, maxw);
    }
}

static void songs_text(char *b, size_t n, int count) { snprintf(b, n, "%d %s", count, count == 1 ? "Song" : "Songs"); }

static void album_row(int a, float y, int hl) {
    const cat_album *al = &cat->albums[a];
    const cat_track *t = &cat->tracks[al->tracks[0]];
    char songs[24], q[40];
    songs_text(songs, sizeof songs, al->ntracks);
    fmt_quality(q, sizeof q, t->fmt, t->bits, t->rate);
    const char *p[3] = { songs, format_label(t->fmt), q };
    int playing = cur_track >= 0 && cat->tracks[cur_track].album_idx == a;
    draw_row(y, hl, 1, cat_thumb(cat, a, 36), 0, al->title, al->artist, p, 3, playing);
}

static void album_row_of_artist(int i, float y, int hl) { album_row(cat->artists[page_artist].albums[i], y, hl); }

static void track_row(int i, float y, int hl) {
    int ti = cat->by_title[i];
    const cat_track *t = &cat->tracks[ti];
    char sub[600], q[40], d[16];
    snprintf(sub, sizeof sub, "%s \xE2\x80\xA2 %s", t->artist, t->album);
    fmt_quality(q, sizeof q, t->fmt, t->bits, t->rate);
    if (t->secs) fmt_secs(d, sizeof d, t->secs); else d[0] = 0;
    const char *p[3] = { format_label(t->fmt), q, d };
    draw_row(y, hl, 1, cat_thumb(cat, t->album_idx, 36), 0, t->title, sub, p, 3, ti == cur_track);
}

static void artist_row(int i, float y, int hl) {
    const cat_artist *ar = &cat->artists[i];
    char albums[24], songs[24];
    snprintf(albums, sizeof albums, "%d %s", ar->nalbums, ar->nalbums == 1 ? "Album" : "Albums");
    songs_text(songs, sizeof songs, ar->ntracks);
    const char *p[2] = { albums, songs };
    int playing = cur_track >= 0 && cat->albums[cat->tracks[cur_track].album_idx].artist_idx == i;
    draw_row(y, hl, 1, cat_thumb(cat, ar->albums[0], 36), 1, ar->name, NULL, p, 2, playing);
}

static void settings_row(int i, float y, int hl) {
    char a[64], b[64];
    if (i == 0) {
        int done, total;
        if (cat_scanning(&done, &total)) snprintf(a, sizeof a, total ? "Scanning %d / %d" : "Scanning", done, total);
        else snprintf(a, sizeof a, "Up to date");
        const char *p[1] = { a };
        draw_row(y, hl, 0, NULL, 0, "Rescan Library", "Look for new and changed music", p, 1, 0);
    } else if (i == 1) {
        songs_text(a, sizeof a, cat ? cat->ntracks : 0);
        snprintf(b, sizeof b, "%d %s", cat ? cat->nalbums : 0, cat && cat->nalbums == 1 ? "Album" : "Albums");
        const char *p[2] = { b, a };
        draw_row(y, hl, 0, NULL, 0, "Music Folder", plat_music_dir(), p, 2, 0);
    } else {
        const char *p[1] = { "v1.1" };
        draw_row(y, hl, 0, NULL, 0, "Fidelity", "Lossless music player for PS Vita", p, 1, 0);
    }
}

// Album page track row (designed to match): 44 high, number / title /
// track artist if different, duration on the right.
static void album_track_row(int i, float y, int hl) {
    const cat_album *al = &cat->albums[page_album];
    int ti = al->tracks[i];
    const cat_track *t = &cat->tracks[ti];
    if (hl) gfx_rrect(&cv, 36, y, 888, 44, 14, C_ROW_SEL);
    if (ti == cur_track) playing_mark(52, y + 22);
    else {
        char n[8];
        snprintf(n, sizeof n, "%d", t->track_no ? t->track_no : i + 1);
        text_at(FONT_PIXEL, 12, 12, 52, y + 16, n, C_TEXT2, 0);
    }
    char d[16] = "";
    if (t->secs) fmt_secs(d, sizeof d, t->secs);
    float dw = text_width(FONT_PIXEL, 12, d);
    text_at(FONT_PIXEL, 12, 12, 908 - dw, y + 16, d, C_TEXT2, 0);
    int maxw = (int)(908 - dw - 24 - 88);
    int w = text_at(FONT_GEIST, 14, 18, 88, y + 13, t->title, C_TEXT, maxw);
    if (strcmp(t->artist, al->artist) && w + 40 < maxw)
        text_at(FONT_GEIST, 14, 18, 88 + w + 10, y + 13, t->artist, C_TEXT2, maxw - w - 10);
}

static void draw_list(void (*row)(int, float, int)) {
    if (!L.l || !L.n) return;
    hot(0, L.top, SCREEN_W, L.bottom - L.top, -1, Z_LIST, 0);
    gfx_clip(&cv, 0, (int)L.top, SCREEN_W, (int)(L.bottom - L.top));
    int first = (int)(L.l->y / L.row);
    float off = L.l->y - first * L.row;
    int pr = pressed_row();
    for (int i = 0; first + i < L.n; i++) {
        float y = L.top + i * L.row - off;
        if (y >= L.bottom) break;
        int idx = first + i;
        row(idx, y, idx == L.l->sel || idx == pr);
    }
    gfx_noclip(&cv);
}

static void draw_empty(void) {
    int done, total;
    const char *t1, *t2, *t3 = NULL;
    char buf[64];
    if (cat_scanning(&done, &total)) {
        t1 = "Scanning your music\xE2\x80\xA6";
        snprintf(buf, sizeof buf, total ? "%d of %d files" : "Looking for files", done, total);
        t2 = buf;
    } else {
        t1 = "No music yet";
        t2 = "Copy your music to ux0:data/Fidelity/Music over FTP or USB,";
        t3 = "then choose Rescan Library in Settings.";
    }
    float w = text_width(FONT_GEIST, 20, t1);
    text_at(FONT_GEIST, 20, 24, (SCREEN_W - w) / 2, 214, t1, C_TEXT, 0);
    w = text_width(FONT_GEIST, 14, t2);
    text_at(FONT_GEIST, 14, 18, (SCREEN_W - w) / 2, 250, t2, C_TEXT2, 0);
    if (t3) { w = text_width(FONT_GEIST, 14, t3); text_at(FONT_GEIST, 14, 18, (SCREEN_W - w) / 2, 272, t3, C_TEXT2, 0); }
}

static void draw_album_header(void) {
    const cat_album *al = &cat->albums[page_album];
    art_or_placeholder(cat_cover(cat, page_album, 120), 36, 52, 120, 12);
    text_at(FONT_GEIST, 24, 28, 172, 60, al->title, C_TEXT, 752);
    text_at(FONT_GEIST, 16, 20, 172, 94, al->artist, C_TEXT2, 752);
    const cat_track *t = &cat->tracks[al->tracks[0]];
    uint32_t secs = 0;
    for (int i = 0; i < al->ntracks; i++) secs += cat->tracks[al->tracks[i]].secs;
    char songs[24], q[40], len[24];
    songs_text(songs, sizeof songs, al->ntracks);
    fmt_quality(q, sizeof q, t->fmt, t->bits, t->rate);
    if (secs >= 3600) snprintf(len, sizeof len, "%uh %um", secs / 3600, secs / 60 % 60);
    else if (secs >= 60) snprintf(len, sizeof len, "%u min", (secs + 30) / 60);
    else snprintf(len, sizeof len, "%u sec", secs);
    const char *p[5] = { al->year, songs, secs ? len : "", format_label(t->fmt), q };
    float x = 172;
    for (int i = 0; i < 5; i++) if (p[i][0]) x += pill(x, 136, p[i], C_PILL, C_TEXT) + 8;
}

// Mini player (Figma "Mini Player"): 696 x 64, r20, centred at y 428 over
// the list; black with the blurred cover at 25%, art 48 r12, title Geist
// 18/20, artist 12/12, controls on the right, a 2 px progress line at the
// bottom edge. The per-track part is cached in mini_static.
static image mini_static, mini_frame;
static mask ic_play36, ic_pause36, ic_prev20, ic_next20;

static void draw_mini(const player_status *s) {
    if (!mini_static.px) {
        mini_static.px = malloc(MINI_W * MINI_H * 4);
        mini_frame.px = malloc(MINI_W * MINI_H * 4);
        if (!mini_static.px || !mini_frame.px) return;
        mini_static.w = mini_frame.w = MINI_W;
        mini_static.h = mini_frame.h = MINI_H;
        mini_dirty = 1;
    }
    canvas screen = cv;
    if (mini_dirty) {
        gfx_begin(&cv, mini_static.px, MINI_W, MINI_H, MINI_W);
        if (mini_bg.px) gfx_blit(&cv, &mini_bg, 0, 0);
        else gfx_clear(&cv, RGB(0, 0, 0));
        art_or_placeholder(&cover_small, 8, 8, 48, 12);
        text_at(FONT_GEIST, 18, 20, 68, 12, cur_tags.title, C_TEXT, 488);
        text_at(FONT_GEIST, 12, 12, 68, 39, cur_tags.artist[0] ? cur_tags.artist : "Unknown Artist", C_TEXT2, 488);
        mini_dirty = 0;
    }
    memcpy(mini_frame.px, mini_static.px, MINI_W * MINI_H * 4);
    gfx_begin(&cv, mini_frame.px, MINI_W, MINI_H, MINI_W);
    float frac = s->total ? (float)s->pos / s->total : 0;
    if (frac > 1) frac = 1;
    gfx_rect(&cv, 0, 62, MINI_W, 2, RGBA(255, 255, 255, 90));
    gfx_rect(&cv, 0, 62, (int)(MINI_W * frac), 2, C_WHITE);
    cv = screen;
    gfx_image(&cv, &mini_frame, MINI_X, MINI_Y, 20, 255);

    hot(MINI_X, MINI_Y, MINI_W - 140, MINI_H, 20, Z_BUTTON, BTN_TRIANGLE); // card: open Now playing
    hot(692, 434, 44, 52, 14, Z_BUTTON, VB_PREV);
    hot(736, 436, 44, 44, 22, Z_BUTTON, BTN_START);
    hot(780, 434, 44, 52, 14, Z_BUTTON, VB_NEXT);
    gfx_mask(&cv, &ic_prev20, 704, 450, RGBA(255, 255, 255, 179));
    gfx_mask(&cv, s->paused ? &ic_play36 : &ic_pause36, 740, 442, RGBA(255, 255, 255, 230));
    gfx_mask(&cv, &ic_next20, 792, 450, RGBA(255, 255, 255, 179));
}

static void draw_library(const player_status *s) {
    gfx_clear(&cv, RGB(0, 0, 0));
    int empty = !cat || !cat->ntracks;
    if (view == V_LIBRARY) {
        if (tab == TAB_SETTINGS) draw_list(settings_row);
        else if (empty) draw_empty();
        else draw_list(tab == TAB_ALBUMS ? album_row : tab == TAB_TRACKS ? track_row : artist_row);
    } else if (view == V_ALBUM) {
        draw_album_header();
        draw_list(album_track_row);
    } else {
        draw_list(album_row_of_artist);
    }
    draw_top_tabs();
    if (mini_visible(s)) draw_mini(s);

    hint nowp = { "TRI", "Now Playing", BTN_TRIANGLE };
    hint tabs = { "L/R", "Change Tabs", 0 };
    hint scr = { "SELECT", "Screen Off", BTN_SELECT };
    if (view == V_LIBRARY) {
        hint left = { "X", tab == TAB_TRACKS ? "Play" : "Select", BTN_CROSS };
        hint r[3]; int n = 0;
        if (s->has_track) r[n++] = nowp;
        r[n++] = tabs; r[n++] = scr;
        draw_bottom(empty && tab != TAB_SETTINGS ? NULL : &left, r, n, 1);
    } else if (view == V_ALBUM) {
        hint left = { "O", "Back", BTN_CIRCLE };
        hint r[4] = { { "X", "Play", BTN_CROSS }, { "SQ", "Shuffle", BTN_SQUARE }, nowp, scr };
        int n = s->has_track ? 4 : 3;
        if (!s->has_track) r[2] = scr;
        draw_bottom(&left, r, n, 1);
    } else {
        hint left = { "O", "Back", BTN_CIRCLE };
        hint r[3] = { { "X", "Select", BTN_CROSS }, nowp, scr };
        int n = s->has_track ? 3 : 2;
        if (!s->has_track) r[1] = scr;
        draw_bottom(&left, r, n, 1);
    }
}

// ---------------------------------------------------------------- now playing view
// Icon paths are exported from Figma; the pause icons have no Figma
// counterpart and are drawn in the same style (the disc with the symbol cut out).
static const char *PATH_PLAY60 = "M30 60C46.5684 60 60 46.5684 60 30C60 13.4314 46.5684 0 30 0C13.4314 0 0 13.4314 0 30C0 46.5684 13.4314 60 30 60ZM26.3451 20.3489C25.3494 19.67 24 20.3831 24 21.5882V38.4117C24 39.6168 25.3494 40.3299 26.3451 39.651L38.6823 31.2393C39.5556 30.6438 39.5556 29.3562 38.6823 28.7607L26.3451 20.3489Z";
static const char *PATH_PAUSE60 = "M30 60C46.5684 60 60 46.5684 60 30C60 13.4314 46.5684 0 30 0C13.4314 0 0 13.4314 0 30C0 46.5684 13.4314 60 30 60ZM24 20H26C27.1046 20 28 20.8954 28 22V38C28 39.1046 27.1046 40 26 40H24C22.8954 40 22 39.1046 22 38V22C22 20.8954 22.8954 20 24 20ZM34 20H36C37.1046 20 38 20.8954 38 22V38C38 39.1046 37.1046 40 36 40H34C32.8954 40 32 39.1046 32 38V22C32 20.8954 32.8954 20 34 20Z";
static const char *PATH_PLAY36 = "M18 36C27.941 36 36 27.941 36 18C36 8.05887 27.941 0 18 0C8.05887 0 0 8.05887 0 18C0 27.941 8.05887 36 18 36ZM15.8071 12.2093C15.2096 11.802 14.4 12.2298 14.4 12.9529V23.047C14.4 23.7701 15.2096 24.1979 15.8071 23.7906L23.2094 18.7436C23.7334 18.3863 23.7334 17.6137 23.2094 17.2564L15.8071 12.2093Z";
static const char *PATH_PAUSE36 = "M18 36C27.941 36 36 27.941 36 18C36 8.05887 27.941 0 18 0C8.05887 0 0 8.05887 0 18C0 27.941 8.05887 36 18 36ZM14.4 12H15.6C16.2628 12 16.8 12.5372 16.8 13.2V22.8C16.8 23.4628 16.2628 24 15.6 24H14.4C13.7372 24 13.2 23.4628 13.2 22.8V13.2C13.2 12.5372 13.7372 12 14.4 12ZM20.4 12H21.6C22.2628 12 22.8 12.5372 22.8 13.2V22.8C22.8 23.4628 22.2628 24 21.6 24H20.4C19.7372 24 19.2 23.4628 19.2 22.8V13.2C19.2 12.5372 19.7372 12 20.4 12Z";
static const char *PATH_PREV20 = "M14.0986 16.3882C15.1951 17.1184 16.6666 16.3343 16.6666 15.0141L16.6666 4.98574C16.6666 3.66558 15.1951 2.88149 14.0986 3.61174L6.56929 8.62591C5.58796 9.27941 5.58796 10.7205 6.56929 11.374L14.0986 16.3882Z"
                                 "M3.33333 15.8333C3.33333 16.2935 3.70642 16.6666 4.16667 16.6666C4.62692 16.6666 5 16.2935 5 15.8333L5 4.16667C5 3.70642 4.62692 3.33333 4.16667 3.33333C3.70642 3.33333 3.33333 3.70642 3.33333 4.16667L3.33333 15.8333Z";
static const char *PATH_NEXT20 = "M5.90137 3.61183C4.80487 2.88162 3.33337 3.66568 3.33337 4.98587V15.0143C3.33337 16.3344 4.80487 17.1185 5.90137 16.3883L13.4307 11.3741C14.412 10.7206 14.412 9.27951 13.4307 8.62601L5.90137 3.61183Z"
                                 "M16.6667 4.16671C16.6667 3.70647 16.2936 3.33337 15.8333 3.33337C15.3731 3.33337 15 3.70647 15 4.16671V15.8333C15 16.2936 15.3731 16.6667 15.8333 16.6667C16.2936 16.6667 16.6667 16.2936 16.6667 15.8333V4.16671Z";
static const char *PATH_NEXT = "M9.44211 5.77893C7.68771 4.61058 5.33331 5.86509 5.33331 7.97738V24.0228C5.33331 26.1351 7.68771 27.3896 9.44211 26.2212L21.489 18.1985C23.0592 17.1529 23.0592 14.8472 21.489 13.8016L9.44211 5.77893Z"
                               "M26.6667 6.66674C26.6667 5.93035 26.0697 5.3334 25.3333 5.3334C24.5969 5.3334 24 5.93035 24 6.66674V25.3333C24 26.0697 24.5969 26.6667 25.3333 26.6667C26.0697 26.6667 26.6667 26.0697 26.6667 25.3333V6.66674Z";
static const char *PATH_PREV = "M22.5579 26.2211C24.3123 27.3894 26.6667 26.1349 26.6667 24.0226L26.6667 7.97719C26.6667 5.86493 24.3123 4.61039 22.5579 5.77879L10.511 13.8015C8.94082 14.8471 8.94082 17.1528 10.511 18.1984L22.5579 26.2211Z"
                               "M5.33333 25.3333C5.33333 26.0697 5.93027 26.6666 6.66667 26.6666C7.40307 26.6666 8 26.0697 8 25.3333L8 6.66666C8 5.93026 7.40307 5.33333 6.66667 5.33333C5.93027 5.33333 5.33333 5.93026 5.33333 6.66666L5.33333 25.3333Z";
static const char *PATH_SHUFFLE = "M17.2929 3.29289C17.6834 2.90237 18.3166 2.90237 18.7071 3.29289L21.7071 6.29289C22.0976 6.68342 22.0976 7.31658 21.7071 7.70711L18.7071 10.7071C18.3166 11.0976 17.6834 11.0976 17.2929 10.7071C16.9024 10.3166 16.9024 9.68342 17.2929 9.29289L18.5858 8H16.8284C16.5632 8 16.3089 8.10536 16.1213 8.29289L6.29289 18.1213C5.73028 18.6839 4.96722 19 4.17157 19H3C2.44772 19 2 18.5523 2 18C2 17.4477 2.44772 17 3 17H4.17157C4.43679 17 4.69114 16.8946 4.87868 16.7071L14.7071 6.87868C15.2697 6.31607 16.0328 6 16.8284 6H18.5858L17.2929 4.70711C16.9024 4.31658 16.9024 3.68342 17.2929 3.29289ZM2 6C2 5.44772 2.44772 5 3 5H4.17157C4.96722 5 5.73028 5.31607 6.29289 5.87868L8.70711 8.29289C9.09763 8.68342 9.09763 9.31658 8.70711 9.70711C8.31658 10.0976 7.68342 10.0976 7.29289 9.70711L4.87868 7.29289C4.69114 7.10536 4.43679 7 4.17157 7H3C2.44772 7 2 6.55228 2 6ZM17.2929 13.2929C17.6834 12.9024 18.3166 12.9024 18.7071 13.2929L21.7071 16.2929C22.0976 16.6834 22.0976 17.3166 21.7071 17.7071L18.7071 20.7071C18.3166 21.0976 17.6834 21.0976 17.2929 20.7071C16.9024 20.3166 16.9024 19.6834 17.2929 19.2929L18.5858 18H16.8284C16.0328 18 15.2697 17.6839 14.7071 17.1213L13.2929 15.7071C12.9024 15.3166 12.9024 14.6834 13.2929 14.2929C13.6834 13.9024 14.3166 13.9024 14.7071 14.2929L16.1213 15.7071C16.3089 15.8946 16.5632 16 16.8284 16H18.5858L17.2929 14.7071C16.9024 14.3166 16.9024 13.6834 17.2929 13.2929Z";
static const char *PATH_REPEAT = "M17.9571 2.29289C17.5666 1.90237 16.9334 1.90237 16.5429 2.29289C16.1524 2.68342 16.1524 3.31658 16.5429 3.70711L17.8358 5H6C4.34315 5 3 6.34315 3 8V11C3 11.5523 3.44772 12 4 12C4.55228 12 5 11.5523 5 11V8C5 7.44772 5.44772 7 6 7H17.8358L16.5429 8.29289C16.1524 8.68342 16.1524 9.31658 16.5429 9.70711C16.9334 10.0976 17.5666 10.0976 17.9571 9.70711L20.4268 7.23744C21.1102 6.55402 21.1102 5.44598 20.4268 4.76256L17.9571 2.29289Z"
                                 "M20 12C20.5523 12 21 12.4477 21 13V16C21 17.6569 19.6569 19 18 19H6.1641L7.45699 20.2929C7.84752 20.6834 7.84752 21.3166 7.45699 21.7071C7.06647 22.0976 6.4333 22.0976 6.04278 21.7071L3.57311 19.2374C2.88969 18.554 2.88969 17.446 3.57311 16.7626L6.04278 14.2929C6.4333 13.9024 7.06647 13.9024 7.45699 14.2929C7.84752 14.6834 7.84752 15.3166 7.45699 15.7071L6.1641 17H18C18.5523 17 19 16.5523 19 16V13C19 12.4477 19.4477 12 20 12Z";
static mask ic_play, ic_pause, ic_prev, ic_next, ic_shuffle, ic_repeat;

static void icons_init(void) {
    mask_from_path(&ic_play, 60, 60, 1, PATH_PLAY60, 1);
    mask_from_path(&ic_pause, 60, 60, 1, PATH_PAUSE60, 1);
    mask_from_path(&ic_prev, 32, 32, 1, PATH_PREV, 0);
    mask_from_path(&ic_next, 32, 32, 1, PATH_NEXT, 0);
    mask_from_path(&ic_shuffle, 24, 24, 1, PATH_SHUFFLE, 1);
    mask_from_path(&ic_repeat, 24, 24, 1, PATH_REPEAT, 0);
    mask_from_path(&ic_play36, 36, 36, 1, PATH_PLAY36, 1);
    mask_from_path(&ic_pause36, 36, 36, 1, PATH_PAUSE36, 1);
    mask_from_path(&ic_prev20, 20, 20, 1, PATH_PREV20, 0);
    mask_from_path(&ic_next20, 20, 20, 1, PATH_NEXT20, 0);
}

// The parts that only change with the track (background, artwork, details,
// tags, hairlines) are drawn once into np_static; each frame copies it and
// draws the bars, timeline and controls on top. Keeps scrubbing smooth.
static image np_static;

static void draw_np_static(const player_status *s) {
    if (np_bg.px) gfx_blit(&cv, &np_bg, 0, 0);
    else gfx_clear(&cv, RGB(0, 0, 0));
    gfx_rect(&cv, 0, 35, SCREEN_W, 1, C_LINE);  // top bar hairline
    gfx_rect(&cv, 0, 508, SCREEN_W, 1, C_LINE); // bottom bar hairline

    // artwork 400x400, radius 20
    if (cover_big.px) gfx_image(&cv, &cover_big, 36, 72, 20, 255);
    else {
        gfx_rrect(&cv, 36, 72, 400, 400, 20, C_PLACEH);
        icon_note(36 + 130, 72 + 130, 140, RGB(72, 72, 76));
    }

    // details: title / artist / album line, 7 px apart
    const float x = 472, w = 452;
    if (!s->has_track) {
        text_at(FONT_GEIST, 32, 36, x, 84, "Not Playing", C_TEXT, (int)w);
        text_at(FONT_GEIST, 20, 0, x, 127, "Pick something in the library", C_TEXT2, (int)w);
        return;
    }
    char line[600];
    text_at(FONT_GEIST, 32, 36, x, 84, cur_tags.title, C_TEXT, (int)w);
    text_at(FONT_GEIST, 20, 0, x, 127, cur_tags.artist[0] ? cur_tags.artist : "Unknown Artist", C_TEXT2, (int)w);
    if (cur_tags.album[0] && cur_tags.year[0]) snprintf(line, sizeof line, "%s \xE2\x80\xA2 %s", cur_tags.album, cur_tags.year);
    else snprintf(line, sizeof line, "%s", cur_tags.album[0] ? cur_tags.album : cur_tags.year);
    text_at(FONT_GEIST, 14, 0, x, 160, line, C_TEXT2, (int)w);

    // tags: [Hi-Res] Lossless, format, bit depth / rate
    float tx = x;
    int lossless = s->fmt == FMT_FLAC || s->fmt == FMT_WAV;
    int hires = s->bits > 16 || s->rate > 48000;
    if (lossless && hires) tx += pill(tx, 198, "Hi-Res Lossless", RGBA(255, 140, 64, 26), RGB(255, 181, 96)) + 8;
    else if (lossless) tx += pill(tx, 198, "Lossless", C_PILL, C_TEXT) + 8;
    tx += pill(tx, 198, format_label(s->fmt), C_PILL, C_TEXT) + 8;
    char q[40];
    fmt_quality(q, sizeof q, s->fmt, s->bits, s->rate);
    if (q[0]) pill(tx, 198, q, C_PILL, C_TEXT);
}

static void draw_playing(const player_status *s) {
    if (np_static_dirty || !np_static.px) {
        if (!np_static.px && (np_static.px = malloc(SCREEN_W * SCREEN_H * 4))) {
            np_static.w = SCREEN_W; np_static.h = SCREEN_H;
        }
        if (np_static.px) {
            canvas screen = cv;
            gfx_begin(&cv, np_static.px, SCREEN_W, SCREEN_H, SCREEN_W);
            draw_np_static(s);
            cv = screen;
            np_static_dirty = 0;
        }
    }
    if (np_static.px) gfx_blit(&cv, &np_static, 0, 0);
    else draw_np_static(s);

    // top bar: "Now Playing 3 / 28", clock and battery
    char a[16], b[16];
    snprintf(a, sizeof a, "%d", s->index + 1);
    snprintf(b, sizeof b, "%d", s->count);
    seg top[5] = { { "Now Playing ", C_TEXT }, { a, C_TEXT }, { " / ", C_TEXT2 }, { b, C_TEXT } };
    segs_draw(36, 9.5f, top, s->count > 0 && s->index >= 0 ? 4 : 1);
    draw_clock();

    const float x = 472, w = 452;
    // timeline: 8 px bar (12 px while scrubbing), times 10 px below
    float frac = s->total ? (float)s->pos / s->total : 0;
    uint64_t pos = s->pos;
    if (scrub >= 0 && s->total) { frac = scrub; pos = (uint64_t)(scrub * s->total); }
    if (frac > 1) frac = 1;
    float bh = scrub >= 0 ? 12 : 8, by = 346 - bh / 2;
    if (s->has_track && s->total) hot(x - 16, 322, w + 32, 52, -1, Z_SEEK, 0);
    gfx_rrect(&cv, x, by, w, bh, bh / 2, RGBA(255, 255, 255, 90)); // Figma: 30% additive
    if (s->has_track && frac > 0) gfx_rrect(&cv, x, by, w * frac < bh ? bh : w * frac, bh, bh / 2, C_WHITE); // 70% additive saturates
    if (s->has_track) {
        char t1[16], t2[16];
        fmt_secs(t1, sizeof t1, s->rate ? pos / s->rate : 0);
        if (s->total) fmt_secs(t2, sizeof t2, s->total / s->rate);
        else snprintf(t2, sizeof t2, "--:--"); // MP3 length still being counted
        text_at(FONT_GEIST_MEDIUM, 12, 0, x, 360, t1, C_TEXT2, 0);
        text_at(FONT_GEIST_MEDIUM, 12, 0, x + w - text_width(FONT_GEIST_MEDIUM, 12, t2), 360, t2, C_TEXT2, 0);
    }

    // controls (top 400, 60 high): shuffle | prev play next | repeat.
    // Touch zones are larger than the icons; active shuffle/repeat get a pill.
    hot(456, 402, 56, 56, 28, Z_BUTTON, BTN_SQUARE);
    hot(584, 398, 64, 64, 32, Z_BUTTON, BTN_L);
    hot(662, 394, 72, 72, 36, Z_BUTTON, BTN_CROSS);
    hot(748, 398, 64, 64, 32, Z_BUTTON, BTN_R);
    hot(884, 402, 56, 56, 28, Z_BUTTON, BTN_TRIANGLE);
    if (s->shuffle) gfx_rrect(&cv, 466, 412, 36, 36, 10, RGBA(255, 255, 255, 30));
    gfx_mask(&cv, &ic_shuffle, 472, 418, s->shuffle ? C_WHITE : RGBA(255, 255, 255, 128));
    gfx_mask(&cv, &ic_prev, 600, 414, RGBA(255, 255, 255, 179));
    gfx_mask(&cv, s->paused || !s->has_track ? &ic_play : &ic_pause, 668, 400, RGBA(255, 255, 255, 230));
    gfx_mask(&cv, &ic_next, 764, 414, RGBA(255, 255, 255, 179));
    if (s->repeat) gfx_rrect(&cv, 894, 412, 36, 36, 10, RGBA(255, 255, 255, 30));
    gfx_mask(&cv, &ic_repeat, 900, 418, s->repeat ? C_WHITE : RGBA(255, 255, 255, 128));
    if (s->repeat == REPEAT_ONE) { // "1" badge on the pill's corner
        gfx_circle(&cv, 927, 415, 7, C_WHITE);
        int ow = text_width(FONT_GEIST_MEDIUM, 10, "1");
        text_at(FONT_GEIST_MEDIUM, 10, 14, 927 - ow / 2.0f, 408, "1", RGB(20, 20, 22), 0);
    }

    hint left = { "O", "Back", BTN_CIRCLE };
    hint r[6] = {
        { "L/R", "Next/Prev", 0 }, { "\xE2\x86\x90\xE2\x86\x92", "Seek", 0 }, { "SQ", "Shuffle", BTN_SQUARE },
        { "TRI", "Repeat", BTN_TRIANGLE }, { "X", "Play/Pause", BTN_CROSS }, { "SELECT", "Screen Off", BTN_SELECT },
    };
    draw_bottom(&left, r, 6, 0);
}

static void draw_toast(const player_status *s) {
    if (!toast[0] || plat_time_us() > toast_until) { toast[0] = 0; return; }
    int w = text_width(FONT_GEIST_MEDIUM, 13, toast) + 28;
    float x, y;
    if (view == V_PLAYING) { x = 698 - w / 2.0f; y = 468; } // under the controls
    else { x = (SCREEN_W - w) / 2.0f; y = mini_visible(s) ? 388 : 466; }
    gfx_rrect(&cv, x, y, w, 30, 15, RGBA(48, 48, 52, 235));
    text_at(FONT_GEIST_MEDIUM, 13, 30, x + 14, y, toast, C_TEXT, 0);
}

static void render(const player_status *s) {
    int stride;
    uint32_t *px = plat_backbuffer(&stride);
    gfx_begin(&cv, px, SCREEN_W, SCREEN_H, stride);
    nzones = 0;
    list_geom(s);
    if (view == V_PLAYING) draw_playing(s);
    else draw_library(s);
    draw_toast(s);
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
    list_t *l = L.l;

    const zone *z = down && !tc.down && !tc.blocked ? zone_at(x, y) : NULL;
    if (down && !tc.down && !tc.blocked && !z) { // dead area: ignore until lifted
        tc.blocked = 1;
        if (l) l->vel = 0;
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
        tc.caught = l && fabsf(l->vel) > 60; // finger stops a fling; that's not a tap
        if (l) l->vel = 0;
        if (z->kind == Z_SEEK) scrub = seek_frac(x);
        dirty = 1;
    } else if (down) {
        float dy = y - tc.y;
        if (fabsf(x - tc.x0) > TAP_SLOP || fabsf(y - tc.y0) > TAP_SLOP) tc.moved = 1;
        if (tc.z.kind == Z_LIST && l && tc.moved && dy != 0) {
            float dt = (now - tc.t_move) / 1e6f;
            l->y -= dy;
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
            if (!l) break;
            if (tc.moved) {
                // fling, unless the finger rested before lifting
                if (now - tc.t_move < 80000 && fabsf(tc.vel) > 150) l->vel = tc.vel;
            } else if (!tc.caught && list_row_at(tc.y) >= 0) {
                l->sel = list_row_at(tc.y);
                out = BTN_CROSS;
            }
            break;
        case Z_BUTTON:
            if (in_zone(&tc.z, tc.x, tc.y)) out = tc.z.btn;
            break;
        }
    }

    // fling momentum
    if (!tc.down && l && l->vel != 0) {
        float dt = (now - last_tick) / 1e6f;
        if (dt > 0.05f) dt = 0.05f;
        l->y += l->vel * dt;
        l->vel *= expf(-4.0f * dt);
        if (clamp_list_y() || fabsf(l->vel) < 30) l->vel = 0;
        dirty = 1;
    }
    last_tick = now;
    return out;
}

// ---------------------------------------------------------------- navigation
static void play_tracks(const int *idx, int n, int start) {
    if (!n) return;
    char **paths = malloc(n * sizeof *paths);
    if (!paths) return;
    for (int i = 0; i < n; i++) paths[i] = cat->tracks[idx[i]].path;
    player_set_queue(paths, n, start); // copies the strings
    free(paths);
    np_back = view;
    view = V_PLAYING;
    dirty = 1;
}

static void open_album(int a, int from_artist) {
    page_album = a;
    page_album_key = cat->albums[a].key;
    album_from_artist = from_artist;
    memset(&lst_album, 0, sizeof lst_album);
    view = V_ALBUM;
}

static void open_artist(int i) {
    page_artist = i;
    memset(&lst_artist, 0, sizeof lst_artist);
    view = V_ARTIST;
}

static void set_tab(int t) {
    tab = (t + NTABS) % NTABS;
    view = V_LIBRARY;
    save_settings();
}

static void activate(int i) {
    if (view == V_LIBRARY) {
        if (tab == TAB_SETTINGS) {
            if (i == 0) { cat_rescan(); show_toast("Rescanning library"); }
            return;
        }
        if (!cat || i < 0) return;
        if (tab == TAB_ALBUMS && i < cat->nalbums) open_album(i, 0);
        else if (tab == TAB_TRACKS && i < cat->ntracks) play_tracks(cat->by_title, cat->ntracks, i);
        else if (tab == TAB_ARTISTS && i < cat->nartists) open_artist(i);
    } else if (view == V_ALBUM) {
        const cat_album *al = &cat->albums[page_album];
        if (i >= 0 && i < al->ntracks) play_tracks(al->tracks, al->ntracks, i);
    } else if (view == V_ARTIST) {
        const cat_artist *ar = &cat->artists[page_artist];
        if (i >= 0 && i < ar->nalbums) open_album(ar->albums[i], 1);
    }
}

// A new catalog from the scanner: keep the open pages if they still exist.
static void adopt_catalog(catalog *c, const player_status *s) {
    char artist[256] = "";
    if (cat && page_artist >= 0 && page_artist < cat->nartists) snprintf(artist, sizeof artist, "%s", cat->artists[page_artist].name);
    cat_free(cat);
    cat = c;
    int a = -1, ar = -1;
    for (int i = 0; i < cat->nalbums; i++) if (cat->albums[i].key == page_album_key) { a = i; break; }
    for (int i = 0; i < cat->nartists && artist[0]; i++) if (!strcmp(cat->artists[i].name, artist)) { ar = i; break; }
    page_album = a;
    page_artist = ar;
    if ((view == V_ALBUM && a < 0) || (view == V_ARTIST && ar < 0)) view = V_LIBRARY;
    if (album_from_artist && ar < 0) album_from_artist = 0;
    if (np_back == V_ALBUM && a < 0) np_back = V_LIBRARY;
    if (np_back == V_ARTIST && ar < 0) np_back = V_LIBRARY;
    find_current_track(s);
    dirty = 1;
}

static void handle_input(uint32_t b, player_status *s) {
    if (pressed(BTN_START, b, 0)) { player_toggle_pause(); dirty = 1; }
    if (pressed(VB_PREV, b, 0)) { player_prev(); dirty = 1; }
    if (pressed(VB_NEXT, b, 0)) { player_next(); dirty = 1; }

    if (view == V_PLAYING) {
        if (pressed(BTN_L, b, 0)) { player_prev(); dirty = 1; }
        if (pressed(BTN_R, b, 0)) { player_next(); dirty = 1; }
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
        if (pressed(BTN_CIRCLE, b, 0)) {
            view = np_back;
            if ((view == V_ALBUM && page_album < 0) || (view == V_ARTIST && page_artist < 0)) view = V_LIBRARY;
            dirty = 1;
        }
        return;
    }

    if (pressed(BTN_L, b, 0)) { set_tab(tab - 1); dirty = 1; return; }
    if (pressed(BTN_R, b, 0)) { set_tab(tab + 1); dirty = 1; return; }
    for (int i = 0; i < NTABS; i++)
        if (pressed(VB_TAB0 << i, b, 0)) { set_tab(i); dirty = 1; return; }
    if (pressed(BTN_TRIANGLE, b, 0) && s->has_track) { np_back = view; view = V_PLAYING; dirty = 1; return; }
    if (pressed(BTN_CIRCLE, b, 0)) {
        if (view == V_ALBUM) view = album_from_artist ? V_ARTIST : V_LIBRARY;
        else if (view == V_ARTIST) view = V_LIBRARY;
        dirty = 1;
        return;
    }
    if (pressed(BTN_SQUARE, b, 0) && view == V_ALBUM) {
        const cat_album *al = &cat->albums[page_album];
        player_set_shuffle(1);
        save_settings();
        play_tracks(al->tracks, al->ntracks, rand() % al->ntracks);
        show_toast("Shuffle on");
        return;
    }

    if (!L.l) return;
    int up = pressed(BTN_UP, b, 1), down = pressed(BTN_DOWN, b, 1);
    int left = pressed(BTN_LEFT, b, 1), right = pressed(BTN_RIGHT, b, 1);
    int cross = pressed(BTN_CROSS, b, 0);
    if ((up || down || left || right || cross) && reveal_selection()) {
        up = down = left = right = cross = 0;
        dirty = 1;
    }
    int page = (int)((L.view_bottom - L.top) / L.row);
    if (up) { L.l->sel = L.l->sel > 0 ? L.l->sel - 1 : L.n - 1; clamp_scroll(); dirty = 1; }
    if (down) { L.l->sel = L.l->sel < L.n - 1 ? L.l->sel + 1 : 0; clamp_scroll(); dirty = 1; }
    if (left) { L.l->sel -= page; clamp_scroll(); dirty = 1; }
    if (right) { L.l->sel += page; clamp_scroll(); dirty = 1; }
    if (cross && L.n) { activate(L.l->sel); dirty = 1; }
}

// ---------------------------------------------------------------- lifecycle
int app_init(const char *asset_dir) {
    char p[512];
    snprintf(p, sizeof p, "%s/Geist-Regular.ttf", asset_dir);
    if (font_load(FONT_GEIST, p, 1)) return -1;
    snprintf(p, sizeof p, "%s/Geist-Medium.ttf", asset_dir);
    if (font_load(FONT_GEIST_MEDIUM, p, 1)) return -1;
    snprintf(p, sizeof p, "%s/GeistPixel-Square.ttf", asset_dir);
    if (font_load(FONT_PIXEL, p, 1)) return -1;
    icons_init();

    plat_mkdir(plat_data_dir());
    snprintf(settings_path, sizeof settings_path, "%s/settings.txt", plat_data_dir());
    char logp[512];
    snprintf(logp, sizeof logp, "%s/log.txt", plat_data_dir());
    remove(logp); // fresh log each launch
    plat_log("--- Fidelity start");

    ld_m = plat_mutex_create();
    plat_thread_start(loader_thread, NULL, 0);
    cat_init();
    player_init();
    int sh = 0, rep = 0;
    load_settings(&sh, &rep);
    player_set_modes(sh, rep);
    return 0;
}

void app_force_redraw(void) { dirty = 1; np_static_dirty = mini_dirty = 1; }

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

    catalog *nc = cat_poll();
    if (nc) adopt_catalog(nc, &s);

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

    list_geom(&s);
    b |= handle_touch(&s);

    if (pressed(BTN_SELECT, b, 0)) {
        screen_off = 1;
        tc.down = 0;
        tc.blocked = 1;
        scrub = -1;
        if (L.l) L.l->vel = 0;
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
    list_geom(&s);
    if (L.l) {
        if (L.l->sel >= L.n) L.l->sel = L.n - 1;
        if (L.l->sel < 0) L.l->sel = 0;
        if (!tc.down) clamp_list_y();
    }

    uint64_t now = plat_time_us();
    // redraw on change, while a toast is up, and 4x/s for the clock,
    // progress, scan status and thumbnails as they arrive
    if (dirty || toast[0] || now - last_draw > 250000) {
        render(&s);
        last_draw = now;
        dirty = 0;
    } else {
        plat_sleep_us(16000);
    }
    return 0;
}
