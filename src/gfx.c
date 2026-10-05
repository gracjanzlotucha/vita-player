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

void gfx_blit(canvas *c, const image *img, int x, int y) {
    if (!img || !img->px) return;
    int x0 = x < c->cx0 ? c->cx0 : x, x1 = x + img->w > c->cx1 ? c->cx1 : x + img->w;
    if (x1 <= x0) return;
    for (int iy = 0; iy < img->h; iy++) {
        int yy = y + iy;
        if (yy < c->cy0 || yy >= c->cy1) continue;
        memcpy(&c->px[yy * c->stride + x0], &img->px[iy * img->w + x0 - x], (size_t)(x1 - x0) * 4);
    }
}

// ---------------- path masks ----------------
typedef struct { float x0, y0, x1, y1; } pedge;
typedef struct { pedge *e; int n, cap; float cx, cy, sx, sy; float scale; } pbuild;

static void pb_line(pbuild *b, float x, float y) {
    if (b->n == b->cap) { b->cap = b->cap ? b->cap * 2 : 64; b->e = realloc(b->e, b->cap * sizeof *b->e); }
    b->e[b->n++] = (pedge){ b->cx * b->scale, b->cy * b->scale, x * b->scale, y * b->scale };
    b->cx = x; b->cy = y;
}

static void pb_cubic(pbuild *b, float x1, float y1, float x2, float y2, float x, float y) {
    float x0 = b->cx, y0 = b->cy;
    for (int i = 1; i <= 16; i++) {
        float t = i / 16.0f, u = 1 - t;
        pb_line(b, u * u * u * x0 + 3 * u * u * t * x1 + 3 * u * t * t * x2 + t * t * t * x,
                   u * u * u * y0 + 3 * u * u * t * y1 + 3 * u * t * t * y2 + t * t * t * y);
    }
}

static const char *pnum(const char *p, float *v) {
    while (*p == ' ' || *p == ',' || *p == '\n' || *p == '\t' || *p == '\r') p++;
    char *end;
    *v = strtof(p, &end);
    return end == p ? NULL : end;
}

static int path_has_num(const char *p) {
    while (*p == ' ' || *p == ',' || *p == '\n' || *p == '\t' || *p == '\r') p++;
    return (*p >= '0' && *p <= '9') || *p == '-' || *p == '+' || *p == '.';
}

static void path_parse(pbuild *b, const char *p) {
    char cmd = 0;
    float lcx = 0, lcy = 0; // last control point, for S/T
    char prev = 0;
    while (*p) {
        while (*p == ' ' || *p == ',' || *p == '\n' || *p == '\t' || *p == '\r') p++;
        if (!*p) break;
        if ((*p >= 'A' && *p <= 'Z') || (*p >= 'a' && *p <= 'z')) cmd = *p++;
        else if (!cmd || !path_has_num(p)) { p++; continue; }
        int rel = cmd >= 'a';
        float ox = rel ? b->cx : 0, oy = rel ? b->cy : 0, v[6];
        switch (cmd | 0x20) {
        case 'z':
            if (b->cx != b->sx || b->cy != b->sy) pb_line(b, b->sx, b->sy);
            cmd = 0; // Z takes no numbers: skip any stray ones
            break;
        case 'm':
            if (!(p = pnum(p, &v[0])) || !(p = pnum(p, &v[1]))) return;
            if (b->cx != b->sx || b->cy != b->sy) pb_line(b, b->sx, b->sy); // close previous subpath
            b->cx = b->sx = ox + v[0]; b->cy = b->sy = oy + v[1];
            cmd = rel ? 'l' : 'L'; // further pairs are line-tos
            break;
        case 'l':
            if (!(p = pnum(p, &v[0])) || !(p = pnum(p, &v[1]))) return;
            pb_line(b, ox + v[0], oy + v[1]);
            break;
        case 'h':
            if (!(p = pnum(p, &v[0]))) return;
            pb_line(b, ox + v[0], b->cy);
            break;
        case 'v':
            if (!(p = pnum(p, &v[0]))) return;
            pb_line(b, b->cx, oy + v[0]);
            break;
        case 'c':
            for (int i = 0; i < 6; i++) if (!(p = pnum(p, &v[i]))) return;
            pb_cubic(b, ox + v[0], oy + v[1], ox + v[2], oy + v[3], ox + v[4], oy + v[5]);
            lcx = ox + v[2]; lcy = oy + v[3];
            break;
        case 's': {
            for (int i = 0; i < 4; i++) if (!(p = pnum(p, &v[i]))) return;
            int smooth = (prev | 0x20) == 'c' || (prev | 0x20) == 's';
            float x1 = smooth ? 2 * b->cx - lcx : b->cx, y1 = smooth ? 2 * b->cy - lcy : b->cy;
            pb_cubic(b, x1, y1, ox + v[0], oy + v[1], ox + v[2], oy + v[3]);
            lcx = ox + v[0]; lcy = oy + v[1];
            break;
        }
        case 'q': case 't': {
            float qx, qy;
            if ((cmd | 0x20) == 'q') {
                for (int i = 0; i < 4; i++) if (!(p = pnum(p, &v[i]))) return;
                qx = ox + v[0]; qy = oy + v[1]; v[0] = v[2]; v[1] = v[3];
            } else {
                for (int i = 0; i < 2; i++) if (!(p = pnum(p, &v[i]))) return;
                int smooth = (prev | 0x20) == 'q' || (prev | 0x20) == 't';
                qx = smooth ? 2 * b->cx - lcx : b->cx; qy = smooth ? 2 * b->cy - lcy : b->cy;
            }
            float x = ox + v[0], y = oy + v[1];
            pb_cubic(b, b->cx + 2.0f / 3 * (qx - b->cx), b->cy + 2.0f / 3 * (qy - b->cy),
                     x + 2.0f / 3 * (qx - x), y + 2.0f / 3 * (qy - y), x, y);
            lcx = qx; lcy = qy;
            break;
        }
        case 'a': // arcs are not used by our icons: approximate with a line
            for (int i = 0; i < 5; i++) if (!(p = pnum(p, &v[0]))) return;
            if (!(p = pnum(p, &v[0])) || !(p = pnum(p, &v[1]))) return;
            pb_line(b, ox + v[0], oy + v[1]);
            break;
        default:
            p++;
            break;
        }
        prev = cmd;
    }
    if (b->cx != b->sx || b->cy != b->sy) pb_line(b, b->sx, b->sy);
}

typedef struct { float x; int dir; } xing;
static int xing_cmp(const void *a, const void *b) {
    float d = ((const xing *)a)->x - ((const xing *)b)->x;
    return d < 0 ? -1 : d > 0;
}

// Scanline fill: 5 sub-rows per pixel, exact horizontal coverage.
int mask_from_path(mask *m, int w, int h, float scale, const char *d, int evenodd) {
    enum { SS = 5 };
    pbuild b = { 0 };
    b.scale = scale;
    path_parse(&b, d);
    m->w = w; m->h = h;
    m->a = calloc((size_t)w * h, 1);
    float *acc = malloc((w + 1) * sizeof *acc);
    xing *xs = malloc((b.n + 1) * sizeof *xs);
    if (!m->a || !acc || !xs) { free(acc); free(xs); free(b.e); return -1; }
    for (int y = 0; y < h; y++) {
        memset(acc, 0, (w + 1) * sizeof *acc);
        for (int s = 0; s < SS; s++) {
            float sy = y + (s + 0.5f) / SS;
            int nx = 0;
            for (int i = 0; i < b.n; i++) {
                pedge *e = &b.e[i];
                if (e->y0 == e->y1) continue;
                int dir = e->y1 > e->y0 ? 1 : -1;
                float ya = dir > 0 ? e->y0 : e->y1, yb = dir > 0 ? e->y1 : e->y0;
                if (sy < ya || sy >= yb) continue;
                xs[nx].x = e->x0 + (sy - e->y0) * (e->x1 - e->x0) / (e->y1 - e->y0);
                xs[nx++].dir = dir;
            }
            qsort(xs, nx, sizeof *xs, xing_cmp);
            int wind = 0;
            for (int i = 0; i + 1 < nx; i++) {
                wind += xs[i].dir;
                int inside = evenodd ? (i + 1) & 1 : wind != 0;
                if (!inside) continue;
                float xa = xs[i].x < 0 ? 0 : xs[i].x, xb = xs[i + 1].x > w ? w : xs[i + 1].x;
                if (xb <= xa) continue;
                int ia = (int)xa, ib = (int)xb;
                if (ia == ib) { acc[ia] += xb - xa; continue; }
                acc[ia] += ia + 1 - xa;
                for (int k = ia + 1; k < ib; k++) acc[k] += 1;
                acc[ib] += xb - ib;
            }
        }
        for (int x = 0; x < w; x++) {
            int v = (int)(acc[x] * 255.0f / SS + 0.5f);
            m->a[y * w + x] = v > 255 ? 255 : (uint8_t)v;
        }
    }
    free(acc); free(xs); free(b.e);
    return 0;
}

void mask_free(mask *m) { free(m->a); m->a = NULL; m->w = m->h = 0; }

void gfx_mask(canvas *c, const mask *m, int x, int y, uint32_t col) {
    if (!m || !m->a) return;
    for (int j = 0; j < m->h; j++) {
        int yy = y + j;
        if (yy < c->cy0 || yy >= c->cy1) continue;
        for (int i = 0; i < m->w; i++) {
            int xx = x + i;
            if (xx < c->cx0 || xx >= c->cx1) continue;
            uint8_t a = m->a[j * m->w + i];
            if (a) blend(&c->px[yy * c->stride + xx], col, a);
        }
    }
}

// ---------------- text ----------------
static stbtt_fontinfo fonts[FONT_COUNT];
static unsigned char *font_data[FONT_COUNT];
static int font_css[FONT_COUNT];

static float face_scale(int face, float size) {
    return font_css[face] ? stbtt_ScaleForMappingEmToPixels(&fonts[face], size)
                          : stbtt_ScaleForPixelHeight(&fonts[face], size);
}

int font_load(int face, const char *path, int css_px) {
    font_css[face] = css_px;
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
    int gi, w, h, xoff, yoff;
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
            float sc = face_scale(face, size);
            int gi = stbtt_FindGlyphIndex(fi, cp);
            if (gi == 0 && cp != ' ') gi = stbtt_FindGlyphIndex(fi, '?');
            int adv, lsb;
            stbtt_GetGlyphHMetrics(fi, gi, &adv, &lsb);
            g->used = 1; g->key_cp = cp; g->key_size = sz; g->key_face = (uint8_t)face;
            g->gi = gi;
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
    return a * face_scale(face, size);
}

float font_baseline(int face, float size, float line_h) {
    int a, d, l;
    stbtt_GetFontVMetrics(&fonts[face], &a, &d, &l);
    float sc = face_scale(face, size);
    if (line_h <= 0) line_h = (a - d + l) * sc;
    return (line_h - (a - d) * sc) / 2 + a * sc;
}

// kerning between two glyphs (css faces only)
static float kern(int face, float size, const glyph *prev, const glyph *g) {
    if (!font_css[face] || !prev || !g) return 0;
    int k = stbtt_GetGlyphKernAdvance(&fonts[face], prev->gi, g->gi);
    return k ? k * face_scale(face, size) : 0;
}

int text_width(int face, float size, const char *s) {
    float w = 0;
    glyph *prev = NULL;
    while (*s) {
        glyph *g = get_glyph(face, size, utf8_next(&s));
        if (g) w += g->adv + kern(face, size, prev, g);
        prev = g;
    }
    return (int)ceilf(w);
}

static float draw_run(canvas *c, int face, float size, float x, float y, const char *s, const char *end, uint32_t col) {
    glyph *prev = NULL;
    while (*s && (!end || s < end)) {
        glyph *g = get_glyph(face, size, utf8_next(&s));
        if (!g) continue;
        x += kern(face, size, prev, g);
        prev = g;
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
