#pragma once
#include <stdint.h>
#include <stddef.h>

typedef struct { int w, h; uint32_t *px; } image; // RGBA8888, R low byte

typedef struct {
    char title[256], artist[256], album[256];
    char album_artist[256];
    char year[8];         // "2020", or empty
    int disc_no;
    int track_no;
    uint8_t *cover_data;  // raw embedded/folder image bytes (jpg/png)
    size_t cover_size;
} track_tags;

// Reads tags and finds cover art (embedded first, then folder image).
void tags_read(const char *path, track_tags *t, int want_cover);
void tags_free(track_tags *t);

// Decodes cover bytes and scales to size x size (center-cropped square).
int  image_from_cover(const uint8_t *data, size_t n, int size, image *out);
int  image_scale(const image *src, int size, image *out); // square, downscale only
void image_free(image *img);
uint32_t image_average(const image *img); // RGBA
