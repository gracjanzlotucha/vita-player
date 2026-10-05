#pragma once
#include <stdint.h>
#include "tags.h"

#define RGBA(r, g, b, a) ((uint32_t)(r) | (uint32_t)(g) << 8 | (uint32_t)(b) << 16 | (uint32_t)(a) << 24)
#define RGB(r, g, b) RGBA(r, g, b, 255)
#define WITH_ALPHA(c, a) (((c) & 0x00FFFFFFu) | (uint32_t)(a) << 24)

typedef struct {
    uint32_t *px;
    int w, h, stride;
    int cx0, cy0, cx1, cy1; // clip
} canvas;

void gfx_begin(canvas *c, uint32_t *px, int w, int h, int stride);
void gfx_clip(canvas *c, int x, int y, int w, int h);
void gfx_noclip(canvas *c);

void gfx_clear(canvas *c, uint32_t col);
void gfx_rect(canvas *c, int x, int y, int w, int h, uint32_t col);
void gfx_rrect(canvas *c, float x, float y, float w, float h, float r, uint32_t col);
void gfx_vgradient(canvas *c, int x, int y, int w, int h, uint32_t top, uint32_t bottom);
void gfx_circle(canvas *c, float cx, float cy, float r, uint32_t col);
void gfx_ring(canvas *c, float cx, float cy, float r, float thick, uint32_t col);
void gfx_line(canvas *c, float x0, float y0, float x1, float y1, float thick, uint32_t col);
void gfx_triangle(canvas *c, float x0, float y0, float x1, float y1, float x2, float y2, uint32_t col);
void gfx_triangle_outline(canvas *c, float x0, float y0, float x1, float y1, float x2, float y2, float thick, uint32_t col);
void gfx_image(canvas *c, const image *img, int x, int y, float radius, uint8_t alpha);

uint32_t col_mix(uint32_t a, uint32_t b, float t);

// Text
enum { FONT_REGULAR, FONT_SEMIBOLD, FONT_COUNT };
int  font_load(int face, const char *path);
int  text_width(int face, float size, const char *utf8);
// Draws at baseline y. Truncates with an ellipsis if wider than max_w (>0).
// Returns drawn width.
int  text_draw(canvas *c, int face, float size, float x, float y, const char *utf8, uint32_t col, int max_w);
float font_ascent(int face, float size);
