#include "tags.h"
#include "platform.h"
#include "decoder.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#define STB_VORBIS_HEADER_ONLY
#include "stb_vorbis.c"
#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_JPEG
#define STBI_ONLY_PNG
#define STBI_NO_STDIO
#include "stb_image.h"

static uint32_t be32(const uint8_t *p) { return (uint32_t)p[0] << 24 | p[1] << 16 | p[2] << 8 | p[3]; }
static uint32_t le32(const uint8_t *p) { return (uint32_t)p[3] << 24 | p[2] << 16 | p[1] << 8 | p[0]; }
static uint32_t syncsafe(const uint8_t *p) { return (p[0] & 0x7f) << 21 | (p[1] & 0x7f) << 14 | (p[2] & 0x7f) << 7 | (p[3] & 0x7f); }

// Big reads (embedded pictures, ID3 tags) go in 64 KB pieces so the audio
// thread's reads from the same memory card are never stuck behind one.
static size_t fread_chunked(void *buf, size_t n, FILE *f) {
    size_t done = 0;
    while (done < n) {
        size_t want = n - done > 65536 ? 65536 : n - done;
        size_t got = fread((uint8_t *)buf + done, 1, want, f);
        done += got;
        if (got < want) break;
    }
    return done;
}

static void set_field(char *dst, const char *src, size_t n) {
    if (!src[0]) return;
    size_t i = 0;
    for (; i < n && i < 255 && src[i]; i++) dst[i] = src[i];
    dst[i] = 0;
}

// keeps the year from "2020", "2020-03-14" etc.
static void set_year(track_tags *t, const char *v) {
    if (t->year[0]) return;
    for (int i = 0; i < 4; i++) if (v[i] < '0' || v[i] > '9') return;
    memcpy(t->year, v, 4);
    t->year[4] = 0;
}

// "KEY=value" vorbis comment
static void vorbis_comment(track_tags *t, const char *c, size_t n) {
    const char *eq = memchr(c, '=', n);
    if (!eq) return;
    size_t kl = eq - c;
    const char *v = eq + 1;
    size_t vl = n - kl - 1;
    char val[256];
    if (vl > 255) vl = 255;
    memcpy(val, v, vl); val[vl] = 0;
    if (kl == 5 && !strncasecmp(c, "TITLE", 5)) set_field(t->title, val, vl);
    else if (kl == 6 && !strncasecmp(c, "ARTIST", 6)) set_field(t->artist, val, vl);
    else if ((kl == 11 && !strncasecmp(c, "ALBUMARTIST", 11)) || (kl == 12 && !strncasecmp(c, "ALBUM ARTIST", 12))) {
        set_field(t->album_artist, val, vl);
        if (!t->artist[0]) set_field(t->artist, val, vl);
    }
    else if (kl == 10 && !strncasecmp(c, "DISCNUMBER", 10)) t->disc_no = atoi(val);
    else if (kl == 5 && !strncasecmp(c, "ALBUM", 5)) set_field(t->album, val, vl);
    else if (kl == 11 && !strncasecmp(c, "TRACKNUMBER", 11)) t->track_no = atoi(val);
    else if ((kl == 4 && !strncasecmp(c, "DATE", 4)) || (kl == 4 && !strncasecmp(c, "YEAR", 4))) set_year(t, val);
}

// FLAC PICTURE block -> image bytes (prefers type 3 = front cover)
static int flac_picture(const uint8_t *b, uint32_t len, track_tags *t, int *best_type) {
    if (len < 32) return 0;
    uint32_t type = be32(b), p = 4;
    uint32_t ml = be32(b + p); p += 4 + ml; if (p + 4 > len) return 0;
    uint32_t dl = be32(b + p); p += 4 + dl; if (p + 20 > len) return 0;
    p += 16;
    uint32_t sz = be32(b + p); p += 4;
    if (p + sz > len) return 0;
    if (t->cover_data && !(type == 3 && *best_type != 3)) return 0;
    free(t->cover_data);
    t->cover_data = malloc(sz);
    memcpy(t->cover_data, b + p, sz);
    t->cover_size = sz;
    *best_type = (int)type;
    return 1;
}

static void read_flac(FILE *f, track_tags *t, int want_cover) {
    uint8_t h[4];
    if (fread(h, 1, 4, f) != 4 || memcmp(h, "fLaC", 4)) return;
    int last = 0, best = -1;
    while (!last) {
        if (fread(h, 1, 4, f) != 4) return;
        last = h[0] & 0x80;
        int type = h[0] & 0x7f;
        uint32_t len = (uint32_t)h[1] << 16 | h[2] << 8 | h[3];
        if (type == 4 || (type == 6 && want_cover)) {
            uint8_t *b = malloc(len);
            if (!b || fread_chunked(b, len, f) != len) { free(b); return; }
            if (type == 4 && len >= 8) {
                uint32_t p = 4 + le32(b);               // skip vendor
                if (p + 4 <= len) {
                    uint32_t cnt = le32(b + p); p += 4;
                    for (uint32_t i = 0; i < cnt && p + 4 <= len; i++) {
                        uint32_t cl = le32(b + p); p += 4;
                        if (p + cl > len) break;
                        vorbis_comment(t, (const char *)b + p, cl);
                        p += cl;
                    }
                }
            } else if (type == 6) {
                flac_picture(b, len, t, &best);
            }
            free(b);
        } else {
            fseek(f, len, SEEK_CUR);
        }
    }
}

// ID3 text frame -> UTF-8
static void id3_text(char *dst, const uint8_t *b, uint32_t n) {
    if (n < 1) return;
    uint8_t enc = b[0];
    b++; n--;
    char out[256]; size_t o = 0;
    if (enc == 0 || enc == 3) { // latin1 / utf8
        for (uint32_t i = 0; i < n && b[i] && o < 250; i++) {
            if (enc == 0 && b[i] >= 0x80) { out[o++] = 0xC0 | (b[i] >> 6); out[o++] = 0x80 | (b[i] & 0x3f); }
            else out[o++] = b[i];
        }
    } else { // utf16 (1 = with BOM, 2 = BE)
        int bigend = enc == 2;
        uint32_t i = 0;
        if (enc == 1 && n >= 2) { bigend = b[0] == 0xFE; i = 2; }
        for (; i + 1 < n && o < 248; i += 2) {
            uint32_t c = bigend ? (b[i] << 8 | b[i + 1]) : (b[i + 1] << 8 | b[i]);
            if (!c) break;
            if (c >= 0xD800 && c < 0xDC00 && i + 3 < n) {
                uint32_t c2 = bigend ? (b[i + 2] << 8 | b[i + 3]) : (b[i + 3] << 8 | b[i + 2]);
                c = 0x10000 + ((c - 0xD800) << 10) + (c2 - 0xDC00); i += 2;
            }
            if (c < 0x80) out[o++] = c;
            else if (c < 0x800) { out[o++] = 0xC0 | c >> 6; out[o++] = 0x80 | (c & 0x3f); }
            else if (c < 0x10000) { out[o++] = 0xE0 | c >> 12; out[o++] = 0x80 | ((c >> 6) & 0x3f); out[o++] = 0x80 | (c & 0x3f); }
            else { out[o++] = 0xF0 | c >> 18; out[o++] = 0x80 | ((c >> 12) & 0x3f); out[o++] = 0x80 | ((c >> 6) & 0x3f); out[o++] = 0x80 | (c & 0x3f); }
        }
    }
    out[o] = 0;
    set_field(dst, out, o);
}

static void id3_apic(track_tags *t, const uint8_t *b, uint32_t n, int v22) {
    if (t->cover_data || n < 4) return;
    uint32_t p = 1;
    if (v22) p += 3;
    else { while (p < n && b[p]) p++; p++; }   // mime
    if (p >= n) return;
    p++;                                         // picture type
    int enc = b[0];
    if (enc == 1 || enc == 2) { while (p + 1 < n && (b[p] || b[p + 1])) p += 2; p += 2; }
    else { while (p < n && b[p]) p++; p++; }   // description
    if (p >= n) return;
    t->cover_size = n - p;
    t->cover_data = malloc(t->cover_size);
    memcpy(t->cover_data, b + p, t->cover_size);
}

static void read_id3(FILE *f, track_tags *t, int want_cover) {
    uint8_t h[10];
    if (fread(h, 1, 10, f) != 10 || memcmp(h, "ID3", 3)) return;
    int ver = h[3];
    uint32_t size = syncsafe(h + 6);
    if (size > 32 * 1024 * 1024) return;
    uint8_t *b = malloc(size);
    if (!b || fread_chunked(b, size, f) != size) { free(b); return; }
    uint32_t p = 0;
    if (h[5] & 0x40 && ver >= 3) p += (ver == 4 ? syncsafe(b) : be32(b) + 4); // extended header
    while (p + (ver == 2 ? 6 : 10) <= size) {
        const uint8_t *fh = b + p;
        if (!fh[0]) break;
        uint32_t fl; char id[5] = { 0 };
        if (ver == 2) { memcpy(id, fh, 3); fl = fh[3] << 16 | fh[4] << 8 | fh[5]; p += 6; }
        else { memcpy(id, fh, 4); fl = ver == 4 ? syncsafe(fh + 4) : be32(fh + 4); p += 10; }
        if (p + fl > size) break;
        const uint8_t *d = b + p;
        if (!strcmp(id, "TIT2") || !strcmp(id, "TT2")) id3_text(t->title, d, fl);
        else if (!strcmp(id, "TPE1") || !strcmp(id, "TP1")) id3_text(t->artist, d, fl);
        else if (!strcmp(id, "TPE2") || !strcmp(id, "TP2")) { id3_text(t->album_artist, d, fl); if (!t->artist[0]) id3_text(t->artist, d, fl); }
        else if (!strcmp(id, "TPOS") || !strcmp(id, "TPA")) { char n[16] = { 0 }; id3_text(n, d, fl < 15 ? fl : 15); t->disc_no = atoi(n); }
        else if (!strcmp(id, "TALB") || !strcmp(id, "TAL")) id3_text(t->album, d, fl);
        else if (!strcmp(id, "TRCK") || !strcmp(id, "TRK")) { char n[16] = { 0 }; id3_text(n, d, fl < 15 ? fl : 15); t->track_no = atoi(n); }
        else if (!strcmp(id, "TDRC") || !strcmp(id, "TYER") || !strcmp(id, "TYE")) { char y[16] = { 0 }; id3_text(y, d, fl < 15 ? fl : 15); set_year(t, y); }
        else if (want_cover && (!strcmp(id, "APIC") || !strcmp(id, "PIC"))) id3_apic(t, d, fl, ver == 2);
        p += fl;
    }
    free(b);
}

static void read_ogg(const char *path, track_tags *t) {
    int err;
    stb_vorbis *v = stb_vorbis_open_filename(path, &err, NULL);
    if (!v) return;
    stb_vorbis_comment c = stb_vorbis_get_comment(v);
    for (int i = 0; i < c.comment_list_length; i++)
        vorbis_comment(t, c.comment_list[i], strlen(c.comment_list[i]));
    stb_vorbis_close(v);
}

static void folder_cover(const char *path, track_tags *t) {
    char dir[1024];
    snprintf(dir, sizeof dir, "%s", path);
    char *slash = strrchr(dir, '/');
    if (!slash) { slash = strchr(dir, ':'); if (!slash) return; slash[1] = 0; }
    else *slash = 0;
    plat_dirent *ents; int n = plat_list_dir(dir, &ents);
    if (n < 0) return;
    static const char *names[] = { "cover", "folder", "front", "album", "albumart" };
    int best = 99, bi = -1;
    for (int i = 0; i < n; i++) {
        if (ents[i].is_dir) continue;
        const char *e = strrchr(ents[i].name, '.');
        if (!e || (strcasecmp(e, ".jpg") && strcasecmp(e, ".jpeg") && strcasecmp(e, ".png"))) continue;
        int rank = 50;
        for (int k = 0; k < 5; k++) {
            size_t l = strlen(names[k]);
            if (!strncasecmp(ents[i].name, names[k], l) && ents[i].name + l == e) { rank = k; break; }
        }
        if (rank < best) { best = rank; bi = i; }
    }
    if (bi >= 0) {
        char full[1280];
        size_t dl = strlen(dir);
        snprintf(full, sizeof full, "%s%s%s", dir, (dl && dir[dl - 1] == ':') ? "" : "/", ents[bi].name);
        FILE *f = fopen(full, "rb");
        if (f) {
            fseek(f, 0, SEEK_END); long sz = ftell(f); fseek(f, 0, SEEK_SET);
            if (sz > 0 && sz < 32 * 1024 * 1024) {
                t->cover_data = malloc(sz);
                if (fread_chunked(t->cover_data, sz, f) == (size_t)sz) t->cover_size = sz;
                else { free(t->cover_data); t->cover_data = NULL; }
            }
            fclose(f);
        }
    }
    free(ents);
}

void tags_read(const char *path, track_tags *t, int want_cover) {
    memset(t, 0, sizeof *t);
    audio_format fmt = format_from_name(path);
    if (fmt == FMT_OGG) read_ogg(path, t);
    else {
        FILE *f = fopen(path, "rb");
        if (f) {
            if (fmt == FMT_FLAC) read_flac(f, t, want_cover);
            else if (fmt == FMT_MP3) read_id3(f, t, want_cover);
            fclose(f);
        }
    }
    if (!t->title[0]) {
        const char *b = strrchr(path, '/');
        if (!b) b = strrchr(path, ':');
        b = b ? b + 1 : path;
        snprintf(t->title, sizeof t->title, "%s", b);
        char *dot = strrchr(t->title, '.');
        if (dot) *dot = 0;
    }
    if (want_cover && !t->cover_data) folder_cover(path, t);
}

void tags_free(track_tags *t) { free(t->cover_data); t->cover_data = NULL; }

int image_from_cover(const uint8_t *data, size_t n, int size, image *out) {
    int w, h, c;
    uint8_t *src = stbi_load_from_memory(data, (int)n, &w, &h, &c, 4);
    if (!src) return -1;
    // centre-crop to square, then area-average (down) / bilinear (up)
    int side = w < h ? w : h;
    int ox = (w - side) / 2, oy = (h - side) / 2;
    out->w = out->h = size;
    out->px = malloc(size * size * 4);
    double scale = (double)side / size;
    for (int y = 0; y < size; y++) {
        for (int x = 0; x < size; x++) {
            uint32_t acc[4] = { 0 }, cnt = 0;
            if (scale > 1.0) {
                int x0 = (int)(x * scale), x1 = (int)((x + 1) * scale);
                int y0 = (int)(y * scale), y1 = (int)((y + 1) * scale);
                if (x1 <= x0) x1 = x0 + 1;
                if (y1 <= y0) y1 = y0 + 1;
                // sample a sparse grid on huge downscales to keep it fast
                int sx = (x1 - x0) > 4 ? (x1 - x0) / 4 : 1, sy = (y1 - y0) > 4 ? (y1 - y0) / 4 : 1;
                for (int yy = y0; yy < y1; yy += sy)
                    for (int xx = x0; xx < x1; xx += sx) {
                        const uint8_t *p = src + ((oy + yy) * w + ox + xx) * 4;
                        acc[0] += p[0]; acc[1] += p[1]; acc[2] += p[2]; cnt++;
                    }
            } else {
                double fx = (x + 0.5) * scale - 0.5, fy = (y + 0.5) * scale - 0.5;
                int ix = (int)fx, iy = (int)fy;
                if (ix < 0) ix = 0;
                if (iy < 0) iy = 0;
                const uint8_t *p = src + ((oy + iy) * w + ox + ix) * 4;
                acc[0] = p[0]; acc[1] = p[1]; acc[2] = p[2]; cnt = 1;
            }
            out->px[y * size + x] = (acc[0] / cnt) | (acc[1] / cnt) << 8 | (acc[2] / cnt) << 16 | 0xFF000000u;
        }
    }
    stbi_image_free(src);
    return 0;
}

// Area-average downscale of a square image (e.g. the 48 px thumbnail from
// the 400 px cover, instead of decoding the file a second time).
int image_scale(const image *src, int size, image *out) {
    if (!src->px || src->w < size) return -1;
    out->w = out->h = size;
    out->px = malloc(size * size * 4);
    if (!out->px) return -1;
    for (int y = 0; y < size; y++) {
        int y0 = y * src->h / size, y1 = (y + 1) * src->h / size;
        for (int x = 0; x < size; x++) {
            int x0 = x * src->w / size, x1 = (x + 1) * src->w / size;
            uint32_t r = 0, g = 0, b = 0, n = 0;
            for (int yy = y0; yy < y1; yy++)
                for (int xx = x0; xx < x1; xx++, n++) {
                    uint32_t p = src->px[yy * src->w + xx];
                    r += p & 255; g += (p >> 8) & 255; b += (p >> 16) & 255;
                }
            out->px[y * size + x] = (r / n) | (g / n) << 8 | (b / n) << 16 | 0xFF000000u;
        }
    }
    return 0;
}

void image_free(image *img) { free(img->px); img->px = NULL; img->w = img->h = 0; }

uint32_t image_average(const image *img) {
    uint64_t r = 0, g = 0, b = 0, n = (uint64_t)img->w * img->h;
    if (!n) return 0xFF202020;
    for (uint64_t i = 0; i < n; i++) {
        uint32_t p = img->px[i];
        r += p & 255; g += (p >> 8) & 255; b += (p >> 16) & 255;
    }
    return (uint32_t)(r / n) | (uint32_t)(g / n) << 8 | (uint32_t)(b / n) << 16 | 0xFF000000u;
}
