#include "library.h"
#include "platform.h"
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// Natural, case-insensitive compare: "2 - x" < "10 - y"
static int natcmp(const char *a, const char *b) {
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

static int ent_cmp(const void *x, const void *y) {
    const lib_entry *a = x, *b = y;
    if (a->is_dir != b->is_dir) return b->is_dir - a->is_dir;
    return natcmp(a->name, b->name);
}

static int str_cmp(const void *x, const void *y) {
    return natcmp(*(char *const *)x, *(char *const *)y);
}

void lib_join(char *out, size_t n, const char *dir, const char *name) {
    size_t l = strlen(dir);
    int sep = l && dir[l - 1] != ':' && dir[l - 1] != '/';
    snprintf(out, n, "%s%s%s", dir, sep ? "/" : "", name);
}

void lib_parent(const char *path, char *out, size_t n) {
    snprintf(out, n, "%s", path);
    size_t l = strlen(out);
    if (l && out[l - 1] == '/') out[--l] = 0;
    char *s = strrchr(out, '/');
    if (s) {
        // keep the ':' root form ("ux0:") rather than "ux0:/"
        if (s > out && s[-1] == ':') s[0] = 0;
        else if (s == out) s[1] = 0;
        else *s = 0;
        return;
    }
    char *colon = strchr(out, ':');
    if (colon && colon[1]) { colon[1] = 0; return; }
    out[0] = 0; // device list
}

const char *lib_basename(const char *path) {
    const char *b = strrchr(path, '/');
    if (!b) b = strrchr(path, ':');
    return b ? b + 1 : path;
}

int lib_open(dir_view *v, const char *path) {
    lib_free(v);
    snprintf(v->path, sizeof v->path, "%s", path);
    v->sel = v->scroll = 0;
    if (!path[0]) {
        char names[8][16];
        int n = plat_devices(names, 8);
        v->items = calloc(n ? n : 1, sizeof *v->items);
        for (int i = 0; i < n; i++) { snprintf(v->items[i].name, 256, "%s", names[i]); v->items[i].is_dir = 1; }
        v->n = n;
        return 0;
    }
    plat_dirent *ents;
    int n = plat_list_dir(path, &ents);
    if (n < 0) { v->n = 0; v->items = NULL; return -1; }
    v->items = calloc(n ? n : 1, sizeof *v->items);
    int k = 0;
    for (int i = 0; i < n; i++) {
        audio_format f = ents[i].is_dir ? FMT_NONE : format_from_name(ents[i].name);
        if (!ents[i].is_dir && f == FMT_NONE) continue; // hide non-audio files
        memcpy(v->items[k].name, ents[i].name, 256);
        v->items[k].is_dir = ents[i].is_dir;
        v->items[k].fmt = f;
        k++;
    }
    free(ents);
    v->n = k;
    qsort(v->items, k, sizeof *v->items, ent_cmp);
    return 0;
}

void lib_free(dir_view *v) { free(v->items); v->items = NULL; v->n = 0; }

#define MAX_COLLECT 20000

static void collect(const char *dir, int recursive, char ***out, int *n, int *cap, int depth) {
    plat_dirent *ents;
    int cnt = plat_list_dir(dir, &ents);
    if (cnt < 0) return;
    // files of this folder first (sorted), then subfolders (sorted)
    char **files = malloc((cnt ? cnt : 1) * sizeof *files), **dirs = malloc((cnt ? cnt : 1) * sizeof *dirs);
    int nf = 0, nd = 0;
    for (int i = 0; i < cnt; i++) {
        if (ents[i].is_dir) { if (recursive) dirs[nd++] = ents[i].name; }
        else if (format_from_name(ents[i].name) != FMT_NONE) files[nf++] = ents[i].name;
    }
    qsort(files, nf, sizeof *files, str_cmp);
    qsort(dirs, nd, sizeof *dirs, str_cmp);
    for (int i = 0; i < nf && *n < MAX_COLLECT; i++) {
        if (*n == *cap) { *cap *= 2; *out = realloc(*out, *cap * sizeof **out); }
        char full[1024];
        lib_join(full, sizeof full, dir, files[i]);
        (*out)[(*n)++] = strdup(full);
    }
    for (int i = 0; i < nd && *n < MAX_COLLECT && depth < 12; i++) {
        char full[1024];
        lib_join(full, sizeof full, dir, dirs[i]);
        collect(full, recursive, out, n, cap, depth + 1);
    }
    free(files); free(dirs); free(ents);
}

int lib_collect(const char *dir, int recursive, char ***out) {
    int n = 0, cap = 256;
    *out = malloc(cap * sizeof **out);
    collect(dir, recursive, out, &n, &cap, 0);
    return n;
}

void lib_free_list(char **l, int n) {
    for (int i = 0; i < n; i++) free(l[i]);
    free(l);
}
