#pragma once
#include <stddef.h>
#include "decoder.h"

typedef struct { char name[256]; int is_dir; audio_format fmt; } lib_entry;

typedef struct {
    char path[1024];     // "" = device list
    lib_entry *items;
    int n;
    int sel, scroll;
} dir_view;

int  lib_open(dir_view *v, const char *path);  // 0 ok
void lib_free(dir_view *v);
void lib_join(char *out, size_t n, const char *dir, const char *name);
void lib_parent(const char *path, char *out, size_t n); // "" when at a device root
const char *lib_basename(const char *path);
// Collects playable files in dir (sorted, optionally recursive).
int  lib_collect(const char *dir, int recursive, char ***out);
void lib_free_list(char **l, int n);
