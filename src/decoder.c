#include "decoder.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#define DR_FLAC_IMPLEMENTATION
#define DR_FLAC_NO_OGG
#include "dr_flac.h"
#define DR_MP3_IMPLEMENTATION
#include "dr_mp3.h"
#define DR_WAV_IMPLEMENTATION
#include "dr_wav.h"
#define STB_VORBIS_NO_PUSHDATA_API
#include "stb_vorbis.c"
#undef L
#undef C
#undef R

static const char *ext_of(const char *name) {
    const char *dot = strrchr(name, '.');
    return dot ? dot + 1 : "";
}

audio_format format_from_name(const char *name) {
    const char *e = ext_of(name);
    if (!strcasecmp(e, "flac")) return FMT_FLAC;
    if (!strcasecmp(e, "mp3")) return FMT_MP3;
    if (!strcasecmp(e, "wav")) return FMT_WAV;
    if (!strcasecmp(e, "ogg")) return FMT_OGG;
    return FMT_NONE;
}

const char *format_label(audio_format f) {
    switch (f) {
    case FMT_FLAC: return "FLAC";
    case FMT_MP3: return "MP3";
    case FMT_WAV: return "WAV";
    case FMT_OGG: return "OGG";
    default: return "";
    }
}

// Files are read through stdio with a large buffer: on the Vita every read
// is a syscall to the memory card, and the libraries read in small pieces.
// The three dr_libs share one callback shape (SET/CUR/END = 0/1/2).
#define FILE_BUF (128 * 1024)
static FILE *fopen_buffered(const char *path) {
    FILE *f = fopen(path, "rb");
    if (f) setvbuf(f, NULL, _IOFBF, FILE_BUF);
    return f;
}
static size_t io_read(void *u, void *buf, size_t n) { return fread(buf, 1, n, (FILE *)u); }
static int io_seek(void *u, int off, int origin) {
    return fseek((FILE *)u, off, origin == 1 ? SEEK_CUR : origin == 2 ? SEEK_END : SEEK_SET) == 0;
}
static int io_tell(void *u, int64_t *pos) {
    long p = ftell((FILE *)u);
    if (p < 0) return 0;
    *pos = p;
    return 1;
}

static int mp3_init(drmp3 *m, FILE *f) {
    return drmp3_init(m, io_read, (drmp3_seek_proc)io_seek, (drmp3_tell_proc)io_tell, NULL, f, NULL);
}

int decoder_open(decoder *d, const char *path) {
    memset(d, 0, sizeof *d);
    d->fmt = format_from_name(path);
    if (d->fmt == FMT_FLAC || d->fmt == FMT_WAV || d->fmt == FMT_MP3) {
        if (!(d->file = fopen_buffered(path))) return -1;
    }
    switch (d->fmt) {
    case FMT_FLAC: {
        drflac *f = drflac_open(io_read, (drflac_seek_proc)io_seek, (drflac_tell_proc)io_tell, d->file, NULL);
        if (!f) { fclose(d->file); d->file = NULL; return -1; }
        d->h = f; d->rate = f->sampleRate; d->channels = f->channels;
        d->bits = f->bitsPerSample; d->total_frames = f->totalPCMFrameCount;
        break;
    }
    case FMT_WAV: {
        drwav *w = malloc(sizeof *w);
        if (!drwav_init(w, io_read, (drwav_seek_proc)io_seek, (drwav_tell_proc)io_tell, d->file, NULL)) {
            free(w); fclose(d->file); d->file = NULL; return -1;
        }
        d->h = w; d->rate = w->sampleRate; d->channels = w->channels;
        d->bits = w->bitsPerSample; d->total_frames = w->totalPCMFrameCount;
        break;
    }
    case FMT_MP3: {
        drmp3 *m = malloc(sizeof *m);
        if (!mp3_init(m, d->file)) { free(m); fclose(d->file); d->file = NULL; return -1; }
        d->h = m; d->rate = m->sampleRate; d->channels = m->channels; d->bits = 16;
        // Known from the Xing/Info header; otherwise counting means reading the
        // whole file, which the background index does (decoder_mp3_index).
        d->total_frames = m->totalPCMFrameCount != DRMP3_UINT64_MAX ? drmp3_get_pcm_frame_count(m) : 0;
        break;
    }
    case FMT_OGG: {
        int err;
        stb_vorbis *v = stb_vorbis_open_filename(path, &err, NULL);
        if (!v) return -1;
        stb_vorbis_info i = stb_vorbis_get_info(v);
        d->h = v; d->rate = i.sample_rate; d->channels = i.channels; d->bits = 16;
        d->total_frames = stb_vorbis_stream_length_in_samples(v);
        break;
    }
    default: return -1;
    }
    if (d->channels == 0 || d->rate == 0) { decoder_close(d); return -1; }
    return 0;
}

void decoder_close(decoder *d) {
    if (!d->h) return;
    switch (d->fmt) {
    case FMT_FLAC: drflac_close(d->h); break;
    case FMT_WAV: drwav_uninit(d->h); free(d->h); break;
    case FMT_MP3: drmp3_uninit(d->h); free(d->h); break;
    case FMT_OGG: stb_vorbis_close(d->h); break;
    default: break;
    }
    d->h = NULL;
    if (d->file) { fclose(d->file); d->file = NULL; }
    free(d->aux);
    d->aux = NULL;
}

// Scratch for multichannel sources; we keep the first two channels
// (front L/R), mono is duplicated.
static int32_t scratch32[4096 * 8];
static int16_t scratch16[4096 * 8];

static void to_stereo32(const int32_t *in, int32_t *out, uint32_t n, uint32_t ch) {
    for (uint32_t i = 0; i < n; i++) {
        int32_t l = in[i * ch];
        int32_t r = ch > 1 ? in[i * ch + 1] : l;
        out[i * 2] = l; out[i * 2 + 1] = r;
    }
}
static void to_stereo16(const int16_t *in, int32_t *out, uint32_t n, uint32_t ch) {
    for (uint32_t i = 0; i < n; i++) {
        int32_t l = in[i * ch];
        int32_t r = ch > 1 ? in[i * ch + 1] : l;
        out[i * 2] = l << 16; out[i * 2 + 1] = r << 16;
    }
}

uint32_t decoder_read(decoder *d, int32_t *out, uint32_t n) {
    if (n > 4096) n = 4096;
    uint32_t ch = d->channels > 8 ? 8 : d->channels;
    uint32_t got = 0;
    switch (d->fmt) {
    case FMT_FLAC:
        if (d->channels == 2) return (uint32_t)drflac_read_pcm_frames_s32(d->h, n, out);
        got = (uint32_t)drflac_read_pcm_frames_s32(d->h, n, scratch32);
        to_stereo32(scratch32, out, got, ch);
        return got;
    case FMT_WAV:
        if (d->channels == 2) return (uint32_t)drwav_read_pcm_frames_s32(d->h, n, out);
        got = (uint32_t)drwav_read_pcm_frames_s32(d->h, n, scratch32);
        to_stereo32(scratch32, out, got, ch);
        return got;
    case FMT_MP3:
        got = (uint32_t)drmp3_read_pcm_frames_s16(d->h, n, scratch16);
        to_stereo16(scratch16, out, got, ch);
        return got;
    case FMT_OGG:
        got = (uint32_t)stb_vorbis_get_samples_short_interleaved(d->h, 2, scratch16, n * 2);
        to_stereo16(scratch16, out, got, 2);
        return got;
    default: return 0;
    }
}

int decoder_seek(decoder *d, uint64_t frame) {
    if (d->total_frames && frame >= d->total_frames) frame = d->total_frames - 1;
    switch (d->fmt) {
    case FMT_FLAC: return drflac_seek_to_pcm_frame(d->h, frame) ? 0 : -1;
    case FMT_WAV: return drwav_seek_to_pcm_frame(d->h, frame) ? 0 : -1;
    case FMT_MP3: {
        if (!d->mp3_seek_ready) {
            // Build a seek table once so later seeks don't decode from the start.
            static drmp3_seek_point pts[1024];
            drmp3_uint32 cnt = 1024;
            uint64_t cur = ((drmp3 *)d->h)->currentPCMFrame;
            if (drmp3_calculate_seek_points(d->h, &cnt, pts)) {
                drmp3_seek_point *copy = malloc(cnt * sizeof *copy);
                memcpy(copy, pts, cnt * sizeof *copy);
                drmp3_bind_seek_table(d->h, cnt, copy);
                d->aux = copy;
            }
            drmp3_seek_to_pcm_frame(d->h, cur);
            d->mp3_seek_ready = 1;
        }
        return drmp3_seek_to_pcm_frame(d->h, frame) ? 0 : -1;
    }
    case FMT_OGG: return stb_vorbis_seek(d->h, (unsigned)frame) ? 0 : -1;
    default: return -1;
    }
}

typedef struct { drmp3_uint32 n; drmp3_seek_point pts[]; } mp3_index;

void *decoder_mp3_index(const char *path, uint64_t *total) {
    *total = 0;
    FILE *f = fopen_buffered(path);
    if (!f) return NULL;
    drmp3 m;
    if (!mp3_init(&m, f)) { fclose(f); return NULL; }
    if (m.totalPCMFrameCount == DRMP3_UINT64_MAX) {
        drmp3_uint64 mp3_frames, pcm_frames;
        if (drmp3_get_mp3_and_pcm_frame_count(&m, &mp3_frames, &pcm_frames)) *total = pcm_frames;
    }
    mp3_index *ix = malloc(sizeof *ix + 1024 * sizeof(drmp3_seek_point));
    if (ix) {
        ix->n = 1024;
        if (!drmp3_calculate_seek_points(&m, &ix->n, ix->pts)) { free(ix); ix = NULL; }
    }
    drmp3_uninit(&m);
    fclose(f);
    return ix;
}

void decoder_mp3_use_index(decoder *d, void *index, uint64_t total) {
    mp3_index *ix = index;
    if (d->fmt != FMT_MP3 || !d->h) { free(ix); return; }
    if (!d->total_frames && total) d->total_frames = total;
    if (d->mp3_seek_ready) { free(ix); return; } // a seek already built one
    if (ix) {
        drmp3_bind_seek_table(d->h, ix->n, ix->pts);
        free(d->aux);
        d->aux = ix;
    }
    d->mp3_seek_ready = 1;
}

void decoder_mp3_index_free(void *index) { free(index); }
