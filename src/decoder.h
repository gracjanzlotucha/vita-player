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
    void *file;             // our buffered FILE (FLAC/WAV/MP3)
} decoder;

audio_format format_from_name(const char *name);
const char *format_label(audio_format f);
int  decoder_open(decoder *d, const char *path);
void decoder_close(decoder *d);
// Reads up to n frames as interleaved stereo int32 (full-scale = 2^31).
// Returns frames read, 0 at end.
uint32_t decoder_read(decoder *d, int32_t *out, uint32_t n);
int  decoder_seek(decoder *d, uint64_t frame);

// MP3 seek index, built off the audio thread (it reads the whole file).
// total gets the exact length when the file has no Xing/Info header.
void *decoder_mp3_index(const char *path, uint64_t *total);
// Hands an index to an open MP3 decoder (takes ownership).
void decoder_mp3_use_index(decoder *d, void *index, uint64_t total);
void decoder_mp3_index_free(void *index);
