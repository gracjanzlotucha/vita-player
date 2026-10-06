// Music library: everything under plat_music_dir(), grouped into albums and
// artists by tags. A worker thread scans in the background and keeps an
// index on the memory card, so only new or changed files are read again.
// Album thumbnails are made on demand and cached on the card too.
#pragma once
#include <stdint.h>
#include "decoder.h"
#include "tags.h"

typedef struct {
    char *path, *title, *artist, *album, *album_artist;
    char year[8];
    int track_no, disc_no;
    uint32_t rate, bits, secs;
    audio_format fmt;
    uint64_t size, mtime;  // change detection
    int album_idx;         // index into catalog.albums
} cat_track;

typedef struct {
    char *title, *artist;  // album artist, else track artist
    char year[8];
    int *tracks, ntracks;  // disc / track order
    int artist_idx;        // index into catalog.artists
    uint32_t key;          // thumbnail id
} cat_album;

typedef struct {
    char *name;
    int *albums, nalbums;  // sorted by title
    int ntracks;
} cat_artist;

typedef struct catalog {
    cat_track *tracks;
    int ntracks;
    cat_album *albums;     // sorted by title
    int nalbums;
    cat_artist *artists;   // sorted by name
    int nartists;
    int *by_title;         // track indices sorted by title (the Tracks tab)
} catalog;

void cat_init(void);       // loads the cached index, starts the worker (scans once)
void cat_rescan(void);
// A newer catalog, if the worker published one since the last call; the
// caller owns it (free the old one with cat_free). NULL otherwise.
catalog *cat_poll(void);
void cat_free(catalog *c);
// 1 while scanning; *done / *total are files read so far / files found.
int  cat_scanning(int *done, int *total);
// Album thumbnail at 48 or 36 px. NULL until ready; asks the worker for it.
// Pointers stay valid for the life of the app.
const image *cat_thumb(const catalog *c, int album, int size);
// One larger cover (e.g. the album page header), decoded on the worker.
// Call from the UI thread only; NULL until ready or if there is no art.
const image *cat_cover(const catalog *c, int album, int size);
// Cover art for Cover Flow (CAT_ART px square), decoded on the worker and
// cached on the card as JPEG; the newest request is served first. UI thread
// only: a pointer stays valid until the next cat_art() call.
#define CAT_ART 340
const image *cat_art(const catalog *c, int album);
// The blurred full-screen background for an album (Now Playing style).
// UI thread only; NULL until ready. Stays valid until it changes.
const image *cat_backdrop(const catalog *c, int album);
// Natural, case-insensitive compare: "2 - x" < "10 - y"
int  natcmp(const char *a, const char *b);
