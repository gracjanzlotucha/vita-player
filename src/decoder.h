#pragma once
#include <stdint.h>

typedef enum { FMT_NONE, FMT_FLAC, FMT_MP3, FMT_WAV, FMT_OGG } audio_format;

typedef struct decoder {
    audio_format fmt;
    void *h;
    uint32_t rate;
    uint32_t channels;
    uint32_t bits;          // source bit depth (16 for lossy)
    uint64_t total_frames;  // 0 if unknown
    int mp3_seek_ready;
    void *aux;              // MP3 seek table
} decoder;

audio_format format_from_name(const char *name);
const char *format_label(audio_format f);
int  decoder_open(decoder *d, const char *path);
void decoder_close(decoder *d);
// Reads up to n frames as interleaved stereo int32 (full-scale = 2^31).
// Returns frames read, 0 at end.
uint32_t decoder_read(decoder *d, int32_t *out, uint32_t n);
int  decoder_seek(decoder *d, uint64_t frame);
