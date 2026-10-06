#include "catalog.h"
#include "platform.h"
#include "gfx.h"
#define STBI_NO_STDIO // as built in tags.c
#include "stb_image.h"
#include "stb_image_write.h"
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static plat_mutex *cm;
// guarded by cm
static catalog *published;
static int scanning, scan_done, scan_total, rescan_req = 1;

static char idx_path[512], cache_dir[512];

// ---------------------------------------------------------------- helpers
int natcmp(const char *a, const char *b) {
    while (*a && *b) {
        if (isdigit((unsigned char)*a) && isdigit((unsigned char)*b)) {
            while (*a == '0') a++;
            while (*b == '0') b++;
            const char *ea = a, *eb = b;
            while (isdigit((unsigned char)*ea)) ea++;
            while (isdigit((unsigned char)*eb)) eb++;
            if (ea - a != eb - b) return (int)((ea - a) - (eb - b));
            int c = strncmp(a, b, ea - a);
            if (c) return c;
            a = ea; b = eb;
            continue;
        }
        int ca = tolower((unsigned char)*a), cb = tolower((unsigned char)*b);
        if (ca != cb) return ca - cb;
        a++; b++;
    }
    return (unsigned char)*a - (unsigned char)*b;
}

static uint32_t fnv(const char *s, uint32_t h) {
    for (; *s; s++) h = (h ^ (uint8_t)tolower((unsigned char)*s)) * 16777619u;
    return h;
}

static char *sdup(const char *s) { return strdup(s ? s : ""); }

static void track_free(cat_track *t) {
    free(t->path); free(t->title); free(t->artist); free(t->album); free(t->album_artist);
}

static void track_copy(cat_track *d, const cat_track *s) {
    *d = *s;
    d->path = sdup(s->path); d->title = sdup(s->title); d->artist = sdup(s->artist);
    d->album = sdup(s->album); d->album_artist = sdup(s->album_artist);
}

// "ux0:data/Fidelity/Music/Artist/Album/01.flac" -> "Album"
static void parent_name(const char *path, char *out, size_t n) {
    const char *end = strrchr(path, '/');
    if (!end) { snprintf(out, n, "Unknown Album"); return; }
    const char *start = end;
    while (start > path && start[-1] != '/' && start[-1] != ':') start--;
    snprintf(out, n, "%.*s", (int)(end - start), start);
}

// ---------------------------------------------------------------- building
// Albums are tracks sharing an album title and album artist; without an
// album-artist tag, tracks in the same folder with the same album title
// (a compilation) stay together.
static const cat_track *g_tr; // for the qsort comparators (worker thread only)
static char **g_key;

static int cmp_group(const void *x, const void *y) {
    int a = *(const int *)x, b = *(const int *)y;
    int c = strcmp(g_key[a], g_key[b]);
    if (c) return c;
    if (g_tr[a].disc_no != g_tr[b].disc_no) return g_tr[a].disc_no - g_tr[b].disc_no;
    if (g_tr[a].track_no != g_tr[b].track_no) return g_tr[a].track_no - g_tr[b].track_no;
    return natcmp(g_tr[a].path, g_tr[b].path);
}

static const cat_album *g_al;
static int cmp_album(const void *x, const void *y) {
    const cat_album *a = &g_al[*(const int *)x], *b = &g_al[*(const int *)y];
    int c = natcmp(a->title, b->title);
    return c ? c : natcmp(a->artist, b->artist);
}

static int cmp_int(const void *x, const void *y) { return *(const int *)x - *(const int *)y; }

static const char **g_names;
static int cmp_name(const void *x, const void *y) { return natcmp(g_names[*(const int *)x], g_names[*(const int *)y]); }

static int cmp_title(const void *x, const void *y) {
    const cat_track *a = &g_tr[*(const int *)x], *b = &g_tr[*(const int *)y];
    int c = natcmp(a->title, b->title);
    return c ? c : natcmp(a->artist, b->artist);
}

static catalog *build(const cat_track *src, int n) {
    if (n < 0) n = 0;
    catalog *c = calloc(1, sizeof *c);
    c->tracks = calloc(n ? n : 1, sizeof *c->tracks);
    c->ntracks = n;
    for (int i = 0; i < n; i++) track_copy(&c->tracks[i], &src[i]);
    cat_track *t = c->tracks;

    // group key per track
    char **key = malloc((n ? n : 1) * sizeof *key);
    for (int i = 0; i < n; i++) {
        char folder[256], buf[1200];
        if (t[i].album_artist[0]) snprintf(buf, sizeof buf, "%s\x1f%s", t[i].album, t[i].album_artist);
        else { parent_name(t[i].path, folder, sizeof folder); snprintf(buf, sizeof buf, "%s\x1f/%s", t[i].album, folder); }
        for (char *p = buf; *p; p++) *p = (char)tolower((unsigned char)*p);
        key[i] = strdup(buf);
    }
    int *ord = malloc((n ? n : 1) * sizeof *ord);
    for (int i = 0; i < n; i++) ord[i] = i;
    g_tr = t; g_key = key;
    qsort(ord, n, sizeof *ord, cmp_group);

    // runs of equal keys -> albums
    cat_album *al = calloc(n ? n : 1, sizeof *al);
    int na = 0;
    for (int i = 0; i < n;) {
        int j = i;
        while (j < n && !strcmp(key[ord[j]], key[ord[i]])) j++;
        cat_album *a = &al[na];
        const cat_track *f = &t[ord[i]];
        a->title = sdup(f->album);
        const char *artist = f->album_artist[0] ? f->album_artist : f->artist;
        if (!f->album_artist[0])
            for (int k = i + 1; k < j; k++)
                if (natcmp(t[ord[k]].artist, f->artist)) { artist = "Various Artists"; break; }
        a->artist = sdup(artist);
        a->key = fnv(key[ord[i]], 2166136261u) | 1; // 0 marks a free thumbnail slot
        a->ntracks = j - i;
        a->tracks = malloc(a->ntracks * sizeof *a->tracks);
        for (int k = i; k < j; k++) {
            a->tracks[k - i] = ord[k];
            if (!a->year[0] && t[ord[k]].year[0]) snprintf(a->year, sizeof a->year, "%s", t[ord[k]].year);
        }
        na++;
        i = j;
    }
    // sort albums by title, then remap
    int *aord = malloc((na ? na : 1) * sizeof *aord);
    for (int i = 0; i < na; i++) aord[i] = i;
    g_al = al;
    qsort(aord, na, sizeof *aord, cmp_album);
    c->albums = calloc(na ? na : 1, sizeof *c->albums);
    c->nalbums = na;
    for (int i = 0; i < na; i++) {
        c->albums[i] = al[aord[i]];
        for (int k = 0; k < c->albums[i].ntracks; k++) t[c->albums[i].tracks[k]].album_idx = i;
    }

    // artists = distinct album artists
    const char **names = malloc((na ? na : 1) * sizeof *names);
    int *nord = malloc((na ? na : 1) * sizeof *nord);
    for (int i = 0; i < na; i++) { names[i] = c->albums[i].artist; nord[i] = i; }
    g_names = names;
    qsort(nord, na, sizeof *nord, cmp_name);
    c->artists = calloc(na ? na : 1, sizeof *c->artists);
    for (int i = 0; i < na;) {
        int j = i;
        while (j < na && !natcmp(names[nord[j]], names[nord[i]])) j++;
        cat_artist *ar = &c->artists[c->nartists];
        ar->name = sdup(names[nord[i]]);
        ar->nalbums = j - i;
        ar->albums = malloc(ar->nalbums * sizeof *ar->albums);
        for (int k = i; k < j; k++) {
            ar->albums[k - i] = nord[k];
            ar->ntracks += c->albums[nord[k]].ntracks;
            c->albums[nord[k]].artist_idx = c->nartists;
        }
        // album indices are in title order, so sorting them numerically lists
        // the artist's albums by title
        qsort(ar->albums, ar->nalbums, sizeof *ar->albums, cmp_int);
        c->nartists++;
        i = j;
    }

    c->by_title = malloc((n ? n : 1) * sizeof *c->by_title);
    for (int i = 0; i < n; i++) c->by_title[i] = i;
    qsort(c->by_title, n, sizeof *c->by_title, cmp_title);

    for (int i = 0; i < n; i++) free(key[i]);
    free(key); free(ord); free(al); free(aord); free(names); free(nord);
    return c;
}

void cat_free(catalog *c) {
    if (!c) return;
    for (int i = 0; i < c->ntracks; i++) track_free(&c->tracks[i]);
    for (int i = 0; i < c->nalbums; i++) { free(c->albums[i].title); free(c->albums[i].artist); free(c->albums[i].tracks); }
    for (int i = 0; i < c->nartists; i++) { free(c->artists[i].name); free(c->artists[i].albums); }
    free(c->tracks); free(c->albums); free(c->artists); free(c->by_title);
    free(c);
}

// ---------------------------------------------------------------- index file
// One line per track, tab-separated; tabs/newlines in tags become spaces.
#define IDX_MAGIC "FIDX1"

static void put_field(FILE *f, const char *s) {
    for (; *s; s++) fputc(*s == '\t' || *s == '\n' || *s == '\r' ? ' ' : *s, f);
    fputc('\t', f);
}

static void save_index(const cat_track *t, int n) {
    char tmp[600];
    snprintf(tmp, sizeof tmp, "%s.tmp", idx_path);
    FILE *f = fopen(tmp, "w");
    if (!f) return;
    setvbuf(f, NULL, _IOFBF, 64 * 1024);
    fprintf(f, IDX_MAGIC "\n");
    for (int i = 0; i < n; i++) {
        put_field(f, t[i].path);
        fprintf(f, "%llu\t%llu\t", (unsigned long long)t[i].size, (unsigned long long)t[i].mtime);
        put_field(f, t[i].title); put_field(f, t[i].artist); put_field(f, t[i].album);
        put_field(f, t[i].album_artist); put_field(f, t[i].year);
        fprintf(f, "%d\t%d\t%u\t%u\t%u\t%d\n", t[i].track_no, t[i].disc_no, t[i].rate, t[i].bits, t[i].secs, (int)t[i].fmt);
    }
    int ok = fclose(f) == 0;
    remove(idx_path);
    if (ok) rename(tmp, idx_path);
}

static int load_index(cat_track **out) {
    *out = NULL;
    FILE *f = fopen(idx_path, "r");
    if (!f) return 0;
    setvbuf(f, NULL, _IOFBF, 64 * 1024);
    static char line[4096];
    if (!fgets(line, sizeof line, f) || strncmp(line, IDX_MAGIC, 5)) { fclose(f); return 0; }
    int n = 0, cap = 256;
    cat_track *v = malloc(cap * sizeof *v);
    while (fgets(line, sizeof line, f)) {
        line[strcspn(line, "\r\n")] = 0;
        char *fld[14];
        int k = 0;
        char *p = line;
        while (k < 14) {
            fld[k++] = p;
            char *tab = strchr(p, '\t');
            if (!tab) break;
            *tab = 0;
            p = tab + 1;
        }
        if (k < 14) continue; // malformed
        if (n == cap) { cap *= 2; v = realloc(v, cap * sizeof *v); }
        cat_track *t = &v[n++];
        memset(t, 0, sizeof *t);
        t->path = sdup(fld[0]);
        t->size = strtoull(fld[1], NULL, 10); t->mtime = strtoull(fld[2], NULL, 10);
        t->title = sdup(fld[3]); t->artist = sdup(fld[4]); t->album = sdup(fld[5]); t->album_artist = sdup(fld[6]);
        snprintf(t->year, sizeof t->year, "%s", fld[7]);
        t->track_no = atoi(fld[8]); t->disc_no = atoi(fld[9]);
        t->rate = (uint32_t)strtoul(fld[10], NULL, 10); t->bits = (uint32_t)strtoul(fld[11], NULL, 10);
        t->secs = (uint32_t)strtoul(fld[12], NULL, 10); t->fmt = (audio_format)atoi(fld[13]);
    }
    fclose(f);
    *out = v;
    return n;
}

// ---------------------------------------------------------------- thumbnails
// Slots are never freed, so the images handed to the UI stay valid.
#define THUMB_SLOTS 8192
typedef struct { uint32_t key; uint8_t state; image i48, i36; } thumb; // state: 0 free/retry, 1 queued, 2 ready, 3 no art
static thumb thumbs[THUMB_SLOTS];
#define TQ 48
static struct { uint32_t key; char path[1024]; } tq[TQ]; // requests, newest last
static int tqn;

static thumb *thumb_slot(uint32_t key) { // with cm held
    for (uint32_t i = 0; i < THUMB_SLOTS; i++) {
        thumb *t = &thumbs[(key + i) & (THUMB_SLOTS - 1)];
        if (t->key == key || !t->key) { t->key = key; return t; }
    }
    return NULL;
}

const image *cat_thumb(const catalog *c, int album, int size) {
    if (!c || album < 0 || album >= c->nalbums) return NULL;
    const cat_album *a = &c->albums[album];
    const image *out = NULL;
    plat_mutex_lock(cm);
    thumb *t = thumb_slot(a->key);
    if (t) {
        if (t->state == 2) out = size <= 36 ? &t->i36 : &t->i48;
        else if (t->state == 0) {
            if (tqn == TQ) { // drop the oldest request; it'll be asked again if still visible
                thumb *old = thumb_slot(tq[0].key);
                if (old && old->state == 1) old->state = 0;
                memmove(&tq[0], &tq[1], (TQ - 1) * sizeof tq[0]);
                tqn--;
            }
            tq[tqn].key = a->key;
            snprintf(tq[tqn].path, sizeof tq[tqn].path, "%s", c->tracks[a->tracks[0]].path);
            tqn++;
            t->state = 1;
        }
    }
    plat_mutex_unlock(cm);
    return out;
}

static void thumb_file(char *out, size_t n, uint32_t key) { snprintf(out, n, "%s/%08x.t48", cache_dir, key); }

// Serves the newest request. Returns 0 if there was none.
static int service_thumb(void) {
    plat_mutex_lock(cm);
    if (!tqn) { plat_mutex_unlock(cm); return 0; }
    tqn--;
    uint32_t key = tq[tqn].key;
    char path[1024];
    snprintf(path, sizeof path, "%s", tq[tqn].path);
    plat_mutex_unlock(cm);

    image i48 = { 0 }, i36 = { 0 };
    char file[600];
    thumb_file(file, sizeof file, key);
    int ok = 0;
    FILE *f = fopen(file, "rb");
    if (f) {
        i48.w = i48.h = 48;
        i48.px = malloc(48 * 48 * 4);
        ok = i48.px && fread(i48.px, 4, 48 * 48, f) == 48 * 48;
        fclose(f);
        if (!ok) image_free(&i48);
    }
    if (!ok) {
        track_tags tg;
        tags_read(path, &tg, 1);
        if (tg.cover_data) {
            plat_cpu_boost(1);
            ok = image_from_cover(tg.cover_data, tg.cover_size, 48, &i48) == 0;
            plat_cpu_boost(0);
            if (ok && (f = fopen(file, "wb"))) { fwrite(i48.px, 4, 48 * 48, f); fclose(f); }
        }
        tags_free(&tg);
    }
    if (ok) image_scale(&i48, 36, &i36);

    plat_mutex_lock(cm);
    thumb *t = thumb_slot(key);
    if (t) {
        if (ok) { t->i48 = i48; t->i36 = i36; t->state = 2; }
        else t->state = 3;
    }
    plat_mutex_unlock(cm);
    return 1;
}

// ---------------------------------------------------------------- big cover
static uint32_t cv_req_key, cv_new_key; // guarded by cm
static int cv_req, cv_req_size;
static char cv_req_path[1024];
static image cv_new;
static image cv_cur;                    // UI thread only
static uint32_t cv_cur_key;

const image *cat_cover(const catalog *c, int album, int size) {
    if (!c || album < 0 || album >= c->nalbums) return NULL;
    uint32_t key = c->albums[album].key;
    if (cv_cur_key == key) return cv_cur.px && cv_cur.w == size ? &cv_cur : NULL;
    const image *out = NULL;
    plat_mutex_lock(cm);
    if (cv_new_key == key) {
        image_free(&cv_cur);
        cv_cur = cv_new; cv_cur_key = key;
        memset(&cv_new, 0, sizeof cv_new); cv_new_key = 0;
        out = cv_cur.px ? &cv_cur : NULL;
    } else if (cv_req_key != key) {
        cv_req_key = key; cv_req_size = size; cv_req = 1;
        snprintf(cv_req_path, sizeof cv_req_path, "%s", c->tracks[c->albums[album].tracks[0]].path);
    }
    plat_mutex_unlock(cm);
    return out;
}

static int service_cover(void) {
    plat_mutex_lock(cm);
    if (!cv_req) { plat_mutex_unlock(cm); return 0; }
    cv_req = 0;
    uint32_t key = cv_req_key;
    int size = cv_req_size;
    char path[1024];
    snprintf(path, sizeof path, "%s", cv_req_path);
    plat_mutex_unlock(cm);
    image img = { 0 };
    track_tags tg;
    tags_read(path, &tg, 1);
    if (tg.cover_data) {
        plat_cpu_boost(1);
        image_from_cover(tg.cover_data, tg.cover_size, size, &img);
        plat_cpu_boost(0);
    }
    tags_free(&tg);
    plat_mutex_lock(cm);
    image_free(&cv_new); // never collected
    cv_new = img; cv_new_key = key; // no art: an empty image, so the UI stops asking
    plat_mutex_unlock(cm);
    return 1;
}

// ---------------------------------------------------------------- cover flow art
// Decoded covers at CAT_ART px, cached on the card as JPEG (a few tens of KB
// each) so flicking back through the library doesn't decode again.
static void art_file(char *out, size_t n, uint32_t key) { snprintf(out, n, "%s/%08x.a%d.jpg", cache_dir, key, CAT_ART); }

static int load_art(uint32_t key, const char *path, image *out) {
    char file[600];
    art_file(file, sizeof file, key);
    int w = 0, h = 0, comp;
    uint8_t *px = NULL;
    FILE *f = fopen(file, "rb");
    if (f) {
        fseek(f, 0, SEEK_END);
        long n = ftell(f);
        fseek(f, 0, SEEK_SET);
        uint8_t *buf = n > 0 ? malloc(n) : NULL;
        if (buf && fread(buf, 1, n, f) == (size_t)n) px = stbi_load_from_memory(buf, (int)n, &w, &h, &comp, 4);
        free(buf);
        fclose(f);
    }
    if (px && w == CAT_ART && h == CAT_ART) {
        out->w = w; out->h = h;
        out->px = malloc((size_t)w * h * 4);
        if (out->px) memcpy(out->px, px, (size_t)w * h * 4);
        stbi_image_free(px);
        return out->px ? 0 : -1;
    }
    if (px) stbi_image_free(px);
    track_tags tg;
    tags_read(path, &tg, 1);
    int ok = -1;
    if (tg.cover_data) {
        plat_cpu_boost(1);
        ok = image_from_cover(tg.cover_data, tg.cover_size, CAT_ART, out);
        plat_cpu_boost(0);
        if (ok == 0) stbi_write_jpg(file, CAT_ART, CAT_ART, 4, out->px, 90);
    }
    tags_free(&tg);
    return ok;
}

// UI side: a small LRU of decoded covers, owned by the UI thread (so the
// images it draws are never freed under it). The worker only fills a mailbox.
#define ART_SLOTS 16
static struct { uint32_t key; int state; image img; uint64_t used; } art[ART_SLOTS]; // state: 0 retry, 1 asked, 2 ready, 3 none
static uint64_t art_clock;
#define AQ 8
static struct { uint32_t key; char path[1024]; } aq[AQ]; // requests (cm), newest last
static int aqn;
static struct { uint32_t key; image img; int ok; } ares[AQ]; // results (cm)
static int aresn;

const image *cat_art(const catalog *c, int album) {
    if (!c || album < 0 || album >= c->nalbums) return NULL;
    uint32_t key = c->albums[album].key;
    plat_mutex_lock(cm);
    for (int i = 0; i < aresn; i++) { // collect finished covers
        int s = -1;
        for (int k = 0; k < ART_SLOTS; k++) if (art[k].key == ares[i].key && art[k].state == 1) { s = k; break; }
        if (s >= 0) { art[s].img = ares[i].img; art[s].state = ares[i].ok ? 2 : 3; }
        else image_free(&ares[i].img);
    }
    aresn = 0;
    int s = -1;
    for (int k = 0; k < ART_SLOTS; k++) if (art[k].key == key) { s = k; break; }
    if (s < 0) { // take an empty or the least recently used slot
        s = 0;
        for (int k = 1; k < ART_SLOTS; k++) if (art[k].used < art[s].used) s = k;
        image_free(&art[s].img);
        art[s].key = key;
        art[s].state = 0;
    }
    art[s].used = ++art_clock;
    if (art[s].state == 0) {
        if (aqn == AQ) { // drop the oldest request; that cover is asked for again when needed
            for (int k = 0; k < ART_SLOTS; k++) if (art[k].key == aq[0].key && art[k].state == 1) art[k].state = 0;
            memmove(&aq[0], &aq[1], (AQ - 1) * sizeof aq[0]);
            aqn--;
        }
        aq[aqn].key = key;
        snprintf(aq[aqn].path, sizeof aq[aqn].path, "%s", c->tracks[c->albums[album].tracks[0]].path);
        aqn++;
        art[s].state = 1;
    }
    const image *out = art[s].state == 2 ? &art[s].img : NULL;
    plat_mutex_unlock(cm);
    return out;
}

static int service_art(void) {
    plat_mutex_lock(cm);
    if (!aqn) { plat_mutex_unlock(cm); return 0; }
    aqn--;
    uint32_t key = aq[aqn].key;
    char path[1024];
    snprintf(path, sizeof path, "%s", aq[aqn].path);
    plat_mutex_unlock(cm);
    image img = { 0 };
    int ok = load_art(key, path, &img) == 0;
    plat_mutex_lock(cm);
    if (aresn < AQ) { ares[aresn].key = key; ares[aresn].img = img; ares[aresn].ok = ok; aresn++; }
    else image_free(&img);
    plat_mutex_unlock(cm);
    return 1;
}

// One backdrop at a time, same hand-over as cat_cover.
static uint32_t bd_req_key, bd_new_key; // cm
static int bd_req;
static char bd_req_path[1024];
static image bd_new;
static image bd_cur;                    // UI thread only
static uint32_t bd_cur_key;

const image *cat_backdrop(const catalog *c, int album) {
    if (!c || album < 0 || album >= c->nalbums) return NULL;
    uint32_t key = c->albums[album].key;
    const image *out = bd_cur.px ? &bd_cur : NULL; // keep showing the last one meanwhile
    if (bd_cur_key == key) return out;
    plat_mutex_lock(cm);
    if (bd_new_key == key) {
        image_free(&bd_cur);
        bd_cur = bd_new; bd_cur_key = key;
        memset(&bd_new, 0, sizeof bd_new); bd_new_key = 0;
        out = bd_cur.px ? &bd_cur : NULL;
    } else if (bd_req_key != key) {
        bd_req_key = key; bd_req = 1;
        snprintf(bd_req_path, sizeof bd_req_path, "%s", c->tracks[c->albums[album].tracks[0]].path);
    }
    plat_mutex_unlock(cm);
    return out;
}

static int service_backdrop(void) {
    plat_mutex_lock(cm);
    if (!bd_req) { plat_mutex_unlock(cm); return 0; }
    bd_req = 0;
    uint32_t key = bd_req_key;
    char path[1024];
    snprintf(path, sizeof path, "%s", bd_req_path);
    plat_mutex_unlock(cm);
    image cov = { 0 }, bg = { 0 };
    if (load_art(key, path, &cov) == 0) {
        plat_cpu_boost(1);
        image_backdrop(&cov, &bg, SCREEN_W, SCREEN_H, -46, -253, 1051, 1050, 0.25f);
        plat_cpu_boost(0);
    }
    image_free(&cov);
    plat_mutex_lock(cm);
    image_free(&bd_new);
    bd_new = bg; bd_new_key = key; // no art: empty, so the UI stops asking
    plat_mutex_unlock(cm);
    return 1;
}

// ---------------------------------------------------------------- scanning
typedef struct { char *path; uint64_t size, mtime; } found;

static void publish(const cat_track *t, int n) {
    catalog *c = build(t, n);
    plat_mutex_lock(cm);
    cat_free(published); // never collected
    published = c;
    plat_mutex_unlock(cm);
}

static void read_track(cat_track *t, const found *f) {
    memset(t, 0, sizeof *t);
    track_tags tg;
    tags_read(f->path, &tg, 0);
    char folder[256];
    parent_name(f->path, folder, sizeof folder);
    t->path = sdup(f->path);
    t->size = f->size; t->mtime = f->mtime;
    t->title = sdup(tg.title);
    t->artist = sdup(tg.artist[0] ? tg.artist : "Unknown Artist");
    t->album = sdup(tg.album[0] ? tg.album : folder);
    t->album_artist = sdup(tg.album_artist);
    snprintf(t->year, sizeof t->year, "%s", tg.year);
    t->track_no = tg.track_no; t->disc_no = tg.disc_no;
    tags_free(&tg);
    decoder d;
    t->fmt = format_from_name(f->path);
    if (decoder_open(&d, f->path) == 0) {
        t->rate = d.rate; t->bits = d.bits;
        t->secs = d.rate ? (uint32_t)(d.total_frames / d.rate) : 0;
        decoder_close(&d);
    }
}

static uint32_t path_hash(const char *s) { uint32_t h = 2166136261u; for (; *s; s++) h = (h ^ (uint8_t)*s) * 16777619u; return h; }

static void scan(cat_track **known, int *nknown) {
    // walk the music folder
    int nf = 0, capf = 256, nd = 1, capd = 16;
    found *files = malloc(capf * sizeof *files);
    char **dirs = malloc(capd * sizeof *dirs);
    dirs[0] = strdup(plat_music_dir());
    while (nd) {
        char *dir = dirs[--nd];
        plat_dirent *ents;
        int n = plat_list_dir(dir, &ents);
        for (int i = 0; i < n; i++) {
            char full[1024];
            snprintf(full, sizeof full, "%s/%s", dir, ents[i].name);
            if (ents[i].is_dir) {
                if (nd == capd) { capd *= 2; dirs = realloc(dirs, capd * sizeof *dirs); }
                dirs[nd++] = strdup(full);
            } else if (format_from_name(ents[i].name) != FMT_NONE) {
                if (nf == capf) { capf *= 2; files = realloc(files, capf * sizeof *files); }
                files[nf++] = (found){ strdup(full), ents[i].size, ents[i].mtime };
            }
        }
        if (n >= 0) free(ents);
        free(dir);
    }
    free(dirs);
    plat_mutex_lock(cm);
    scan_total = nf; scan_done = 0;
    plat_mutex_unlock(cm);

    // index the known tracks by path
    int hn = 1;
    while (hn < *nknown * 2 + 2) hn <<= 1;
    int *ht = malloc(hn * sizeof *ht);
    for (int i = 0; i < hn; i++) ht[i] = -1;
    for (int i = 0; i < *nknown; i++) {
        uint32_t h = path_hash((*known)[i].path) & (hn - 1);
        while (ht[h] >= 0) h = (h + 1) & (hn - 1);
        ht[h] = i;
    }

    cat_track *out = malloc((nf ? nf : 1) * sizeof *out);
    int changed = nf != *nknown, n = 0;
    uint64_t last_pub = plat_time_us();
    for (int i = 0; i < nf; i++) {
        int hit = -1;
        for (uint32_t h = path_hash(files[i].path) & (hn - 1); ht[h] >= 0; h = (h + 1) & (hn - 1))
            if (!strcmp((*known)[ht[h]].path, files[i].path)) { hit = ht[h]; break; }
        if (hit >= 0 && (*known)[hit].size == files[i].size && (*known)[hit].mtime == files[i].mtime) {
            track_copy(&out[n++], &(*known)[hit]);
        } else {
            read_track(&out[n++], &files[i]);
            changed = 1;
            if (!service_cover() && !service_backdrop() && !service_art()) service_thumb(); // keep visible art coming during a long scan
            if (plat_time_us() - last_pub > 2000000) { publish(out, n); last_pub = plat_time_us(); }
        }
        plat_mutex_lock(cm);
        scan_done = i + 1;
        plat_mutex_unlock(cm);
    }
    for (int i = 0; i < nf; i++) free(files[i].path);
    free(files); free(ht);

    for (int i = 0; i < *nknown; i++) track_free(&(*known)[i]);
    free(*known);
    *known = out;
    *nknown = n;
    if (changed) { publish(out, n); save_index(out, n); }
    plat_log("library: %d tracks%s", n, changed ? " (updated)" : "");
}

static int worker(void *arg) {
    (void)arg;
    cat_track *known;
    int nknown = load_index(&known);
    publish(known, nknown);
    for (;;) {
        plat_mutex_lock(cm);
        int go = rescan_req;
        rescan_req = 0;
        if (go) scanning = 1;
        plat_mutex_unlock(cm);
        if (go) {
            scan(&known, &nknown);
            plat_mutex_lock(cm);
            scanning = 0;
            plat_mutex_unlock(cm);
            continue;
        }
        if (!service_cover() && !service_backdrop() && !service_art() && !service_thumb()) plat_sleep_us(20000);
    }
    return 0;
}

// ---------------------------------------------------------------- API
void cat_init(void) {
    cm = plat_mutex_create();
    snprintf(idx_path, sizeof idx_path, "%s/library.idx", plat_data_dir());
    snprintf(cache_dir, sizeof cache_dir, "%s/cache", plat_data_dir());
    plat_mkdir(cache_dir);
    plat_mkdir(plat_music_dir());
    scanning = 1;
    plat_thread_start(worker, NULL, 0);
}

void cat_rescan(void) {
    plat_mutex_lock(cm);
    rescan_req = 1;
    scanning = 1;
    plat_mutex_unlock(cm);
}

catalog *cat_poll(void) {
    plat_mutex_lock(cm);
    catalog *c = published;
    published = NULL;
    plat_mutex_unlock(cm);
    return c;
}

int cat_scanning(int *done, int *total) {
    plat_mutex_lock(cm);
    int s = scanning;
    if (done) *done = scan_done;
    if (total) *total = scan_total;
    plat_mutex_unlock(cm);
    return s;
}
