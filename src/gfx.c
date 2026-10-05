#include "gfx.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define STB_TRUETYPE_IMPLEMENTATION
#include "stb_truetype.h"

static inline float clampf(float v, float a, float b) { return v < a ? a : v > b ? b : v; }

void gfx_begin(canvas *c, uint32_t *px, int w, int h, int stride) {
    c->px = px; c->w = w; c->h = h; c->stride = stride;
    gfx_noclip(c);
}
void gfx_noclip(canvas *c) { c->cx0 = 0; c->cy0 = 0; c->cx1 = c->w; c->cy1 = c->h; }
void gfx_clip(canvas *c, int x, int y, int w, int h) {
    c->cx0 = x < 0 ? 0 : x; c->cy0 = y < 0 ? 0 : y;
    c->cx1 = x + w > c->w ? c->w : x + w; c->cy1 = y + h > c->h ? c->h : y + h;
}

static inline void blend(uint32_t *d, uint32_t col, int cov /*0..255*/) {
    int a = (int)(col >> 24) * cov / 255;
    if (a <= 0) return;
    if (a >= 255) { *d = col | 0xFF000000u; return; }
    uint32_t s = *d;
    int ia = 255 - a;
    uint32_t r = ((col & 255) * a + (s & 255) * ia) / 255;
    uint32_t g = (((col >> 8) & 255) * a + ((s >> 8) & 255) * ia) / 255;
    uint32_t b = (((col >> 16) & 255) * a + ((s >> 16) & 255) * ia) / 255;
    *d = r | g << 8 | b << 16 | 0xFF000000u;
}

uint32_t col_mix(uint32_t a, uint32_t b, float t) {
    t = clampf(t, 0, 1);
    uint32_t out = 0;
    for (int i = 0; i < 32; i += 8) {
        float x = ((a >> i) & 255) * (1 - t) + ((b >> i) & 255) * t;
        out |= (uint32_t)(x + 0.5f) << i;
    }
    return out;
}

void gfx_clear(canvas *c, uint32_t col) {
    for (int y = 0; y < c->h; y++) {
        uint32_t *row = c->px + y * c->stride;
        for (int x = 0; x < c->w; x++) row[x] = col;
    }
}

void gfx_rect(canvas *c, int x, int y, int w, int h, uint32_t col) {
    int x0 = x < c->cx0 ? c->cx0 : x, y0 = y < c->cy0 ? c->cy0 : y;
    int x1 = x + w > c->cx1 ? c->cx1 : x + w, y1 = y + h > c->cy1 ? c->cy1 : y + h;
    int opaque = (col >> 24) == 255;
    for (int yy = y0; yy < y1; yy++) {
        uint32_t *row = c->px + yy * c->stride;
        if (opaque) for (int xx = x0; xx < x1; xx++) row[xx] = col;
        else for (int xx = x0; xx < x1; xx++) blend(&row[xx], col, 255);
    }
}

void gfx_vgradient(canvas *c, int x, int y, int w, int h, uint32_t top, uint32_t bottom) {
    for (int i = 0; i < h; i++) gfx_rect(c, x, y + i, w, 1, col_mix(top, bottom, h > 1 ? (float)i / (h - 1) : 0));
}

// Coverage of a rounded box at pixel centre (px,py); 0..1
static inline float rbox_cov(float px, float py, float x, float y, float w, float h, float r) {
    float cx = x + w * 0.5f, cy = y + h * 0.5f;
    float qx = fabsf(px - cx) - (w * 0.5f - r), qy = fabsf(py - cy) - (h * 0.5f - r);
    float ox = qx > 0 ? qx : 0, oy = qy > 0 ? qy : 0;
    float d = sqrtf(ox * ox + oy * oy) + (qx > qy ? (qx < 0 ? qx : 0) : (qy < 0 ? qy : 0)) - r;
    return clampf(0.5f - d, 0, 1);
}

void gfx_rrect(canvas *c, float x, float y, float w, float h, float r, uint32_t col) {
    if (r > w * 0.5f) r = w * 0.5f;
    if (r > h * 0.5f) r = h * 0.5f;
    int x0 = (int)floorf(x), y0 = (int)floorf(y), x1 = (int)ceilf(x + w), y1 = (int)ceilf(y + h);
    if (x0 < c->cx0) x0 = c->cx0;
    if (y0 < c->cy0) y0 = c->cy0;
    if (x1 > c->cx1) x1 = c->cx1;
    if (y1 > c->cy1) y1 = c->cy1;
    for (int yy = y0; yy < y1; yy++) {
        uint32_t *row = c->px + yy * c->stride;
        float py = yy + 0.5f;
        int corner_row = py < y + r + 1 || py > y + h - r - 1;
        // interior columns (fully covered on non-corner rows)
        int ix0 = (int)ceilf(x + 1), ix1 = (int)floorf(x + w - 1);
        for (int xx = x0; xx < x1; xx++) {
            if (!corner_row && xx >= ix0 && xx < ix1) {
                int e = ix1 < x1 ? ix1 : x1;
                for (; xx < e; xx++) blend(&row[xx], col, 255);
                if (xx >= x1) break;
            }
            float cov = rbox_cov(xx + 0.5f, py, x, y, w, h, r);
            if (cov > 0) blend(&row[xx], col, (int)(cov * 255));
        }
    }
}

void gfx_circle(canvas *c, float cx, float cy, float r, uint32_t col) {
    gfx_rrect(c, cx - r, cy - r, r * 2, r * 2, r, col);
}

void gfx_ring(canvas *c, float cx, float cy, float r, float t, uint32_t col) {
    int x0 = (int)(cx - r - 2), x1 = (int)(cx + r + 2), y0 = (int)(cy - r - 2), y1 = (int)(cy + r + 2);
    for (int y = y0 < c->cy0 ? c->cy0 : y0; y < y1 && y < c->cy1; y++)
        for (int x = x0 < c->cx0 ? c->cx0 : x0; x < x1 && x < c->cx1; x++) {
            float dx = x + 0.5f - cx, dy = y + 0.5f - cy;
            float d = fabsf(sqrtf(dx * dx + dy * dy) - (r - t * 0.5f)) - t * 0.5f;
            float cov = clampf(0.5f - d, 0, 1);
            if (cov > 0) blend(&c->px[y * c->stride + x], col, (int)(cov * 255));
        }
}

void gfx_line(canvas *c, float ax, float ay, float bx, float by, float t, uint32_t col) {
    float hw = t * 0.5f;
    int x0 = (int)floorf(fminf(ax, bx) - hw - 1), x1 = (int)ceilf(fmaxf(ax, bx) + hw + 1);
    int y0 = (int)floorf(fminf(ay, by) - hw - 1), y1 = (int)ceilf(fmaxf(ay, by) + hw + 1);
    float vx = bx - ax, vy = by - ay, ll = vx * vx + vy * vy;
    for (int y = y0 < c->cy0 ? c->cy0 : y0; y < y1 && y < c->cy1; y++)
        for (int x = x0 < c->cx0 ? c->cx0 : x0; x < x1 && x < c->cx1; x++) {
            float px = x + 0.5f - ax, py = y + 0.5f - ay;
            float h = ll > 0 ? clampf((px * vx + py * vy) / ll, 0, 1) : 0;
            float dx = px - vx * h, dy = py - vy * h;
            float d = sqrtf(dx * dx + dy * dy) - hw;
            float cov = clampf(0.5f - d, 0, 1);
            if (cov > 0) blend(&c->px[y * c->stride + x], col, (int)(cov * 255));
        }
}

static float seg_dist(float px, float py, float ax, float ay, float bx, float by) {
    float vx = bx - ax, vy = by - ay, wx = px - ax, wy = py - ay;
    float h = clampf((wx * vx + wy * vy) / (vx * vx + vy * vy), 0, 1);
    float dx = wx - vx * h, dy = wy - vy * h;
    return sqrtf(dx * dx + dy * dy);
}

static float tri_sdf(float px, float py, float x0, float y0, float x1, float y1, float x2, float y2) {
    float d = fminf(seg_dist(px, py, x0, y0, x1, y1), fminf(seg_dist(px, py, x1, y1, x2, y2), seg_dist(px, py, x2, y2, x0, y0)));
    float e0 = (x1 - x0) * (py - y0) - (y1 - y0) * (px - x0);
    float e1 = (x2 - x1) * (py - y1) - (y2 - y1) * (px - x1);
    float e2 = (x0 - x2) * (py - y2) - (y0 - y2) * (px - x2);
    int inside = (e0 >= 0 && e1 >= 0 && e2 >= 0) || (e0 <= 0 && e1 <= 0 && e2 <= 0);
    return inside ? -d : d;
}

static void tri_common(canvas *c, float x0, float y0, float x1, float y1, float x2, float y2, float thick, uint32_t col) {
    int bx0 = (int)floorf(fminf(x0, fminf(x1, x2)) - thick - 1), bx1 = (int)ceilf(fmaxf(x0, fmaxf(x1, x2)) + thick + 1);
    int by0 = (int)floorf(fminf(y0, fminf(y1, y2)) - thick - 1), by1 = (int)ceilf(fmaxf(y0, fmaxf(y1, y2)) + thick + 1);
    for (int y = by0 < c->cy0 ? c->cy0 : by0; y < by1 && y < c->cy1; y++)
        for (int x = bx0 < c->cx0 ? c->cx0 : bx0; x < bx1 && x < c->cx1; x++) {
            float d = tri_sdf(x + 0.5f, y + 0.5f, x0, y0, x1, y1, x2, y2);
            if (thick > 0) d = fabsf(d) - thick * 0.5f;
            float cov = clampf(0.5f - d, 0, 1);
            if (cov > 0) blend(&c->px[y * c->stride + x], col, (int)(cov * 255));
        }
}
void gfx_triangle(canvas *c, float x0, float y0, float x1, float y1, float x2, float y2, uint32_t col) {
    tri_common(c, x0, y0, x1, y1, x2, y2, 0, col);
}
void gfx_triangle_outline(canvas *c, float x0, float y0, float x1, float y1, float x2, float y2, float t, uint32_t col) {
    tri_common(c, x0, y0, x1, y1, x2, y2, t, col);
}

void gfx_image(canvas *c, const image *img, int x, int y, float r, uint8_t alpha) {
    if (!img || !img->px) return;
    for (int iy = 0; iy < img->h; iy++) {
        int yy = y + iy;
        if (yy < c->cy0 || yy >= c->cy1) continue;
        int corner_row = r > 0 && (iy < r + 1 || iy > img->h - r - 2);
        for (int ix = 0; ix < img->w; ix++) {
            int xx = x + ix;
            if (xx < c->cx0 || xx >= c->cx1) continue;
            int cov = alpha;
            if (corner_row && (ix < r + 1 || ix > img->w - r - 2))
                cov = (int)(rbox_cov(ix + 0.5f, iy + 0.5f, 0, 0, img->w, img->h, r) * alpha);
            blend(&c->px[yy * c->stride + xx], img->px[iy * img->w + ix], cov);
        }
    }
}

// ---------------- text ----------------
static stbtt_fontinfo fonts[FONT_COUNT];
static unsigned char *font_data[FONT_COUNT];

int font_load(int face, const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) return -1;
    fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
    font_data[face] = malloc(n);
    if (fread(font_data[face], 1, n, f) != (size_t)n) { fclose(f); return -1; }
    fclose(f);
    return stbtt_InitFont(&fonts[face], font_data[face], stbtt_GetFontOffsetForIndex(font_data[face], 0)) ? 0 : -1;
}

typedef struct {
    uint32_t key_cp; uint16_t key_size; uint8_t key_face, used;
    int w, h, xoff, yoff;
    float adv;
    uint8_t *bmp;
} glyph;

#define GCACHE 4096
static glyph gcache[GCACHE];

static glyph *get_glyph(int face, float size, uint32_t cp) {
    uint16_t sz = (uint16_t)(size * 4);
    uint32_t hsh = (cp * 2654435761u) ^ (sz * 40503u) ^ (face * 97u);
    for (int i = 0; i < GCACHE; i++) {
        glyph *g = &gcache[(hsh + i) & (GCACHE - 1)];
        if (g->used && g->key_cp == cp && g->key_size == sz && g->key_face == face) return g;
        if (!g->used) {
            stbtt_fontinfo *fi = &fonts[face];
            float sc = stbtt_ScaleForPixelHeight(fi, size);
            int gi = stbtt_FindGlyphIndex(fi, cp);
            if (gi == 0 && cp != ' ') gi = stbtt_FindGlyphIndex(fi, '?');
            int adv, lsb;
            stbtt_GetGlyphHMetrics(fi, gi, &adv, &lsb);
            g->used = 1; g->key_cp = cp; g->key_size = sz; g->key_face = (uint8_t)face;
            g->adv = adv * sc;
            g->bmp = stbtt_GetGlyphBitmap(fi, sc, sc, gi, &g->w, &g->h, &g->xoff, &g->yoff);
            return g;
        }
    }
    return NULL; // cache full (very unlikely)
}

static uint32_t utf8_next(const char **s) {
    const unsigned char *p = (const unsigned char *)*s;
    uint32_t c = *p++;
    if (c >= 0xF0 && p[0] && p[1] && p[2]) { c = (c & 7) << 18 | (p[0] & 63) << 12 | (p[1] & 63) << 6 | (p[2] & 63); p += 3; }
    else if (c >= 0xE0 && p[0] && p[1]) { c = (c & 15) << 12 | (p[0] & 63) << 6 | (p[1] & 63); p += 2; }
    else if (c >= 0xC0 && p[0]) { c = (c & 31) << 6 | (p[0] & 63); p += 1; }
    *s = (const char *)p;
    return c;
}

float font_ascent(int face, float size) {
    int a, d, l;
    stbtt_GetFontVMetrics(&fonts[face], &a, &d, &l);
    return a * stbtt_ScaleForPixelHeight(&fonts[face], size);
}

int text_width(int face, float size, const char *s) {
    float w = 0;
    while (*s) {
        glyph *g = get_glyph(face, size, utf8_next(&s));
        if (g) w += g->adv;
    }
    return (int)ceilf(w);
}

static float draw_run(canvas *c, int face, float size, float x, float y, const char *s, const char *end, uint32_t col) {
    while (*s && (!end || s < end)) {
        glyph *g = get_glyph(face, size, utf8_next(&s));
        if (!g) continue;
        int gx = (int)lroundf(x) + g->xoff, gy = (int)lroundf(y) + g->yoff;
        for (int j = 0; j < g->h; j++) {
            int yy = gy + j;
            if (yy < c->cy0 || yy >= c->cy1) continue;
            for (int i = 0; i < g->w; i++) {
                int xx = gx + i;
                if (xx < c->cx0 || xx >= c->cx1) continue;
                uint8_t a = g->bmp[j * g->w + i];
                if (a) blend(&c->px[yy * c->stride + xx], col, a);
            }
        }
        x += g->adv;
    }
    return x;
}

int text_draw(canvas *c, int face, float size, float x, float y, const char *s, uint32_t col, int max_w) {
    int w = text_width(face, size, s);
    if (max_w <= 0 || w <= max_w) return (int)(draw_run(c, face, size, x, y, s, NULL, col) - x);
    const char *ell = "\xE2\x80\xA6";
    float ew = (float)text_width(face, size, ell);
    float acc = 0;
    const char *p = s, *cut = s;
    while (*p) {
        const char *q = p;
        glyph *g = get_glyph(face, size, utf8_next(&q));
        if (acc + (g ? g->adv : 0) + ew > max_w) break;
        acc += g ? g->adv : 0;
        p = q; cut = p;
    }
    // drop trailing spaces before the ellipsis
    while (cut > s && cut[-1] == ' ') cut--;
    float ex = draw_run(c, face, size, x, y, s, cut, col);
    return (int)(draw_run(c, face, size, ex, y, ell, NULL, col) - x);
}
