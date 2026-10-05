#include "decoder.h"
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

int decoder_open(decoder *d, const char *path) {
    memset(d, 0, sizeof *d);
    d->fmt = format_from_name(path);
    switch (d->fmt) {
    case FMT_FLAC: {
        drflac *f = drflac_open_file(path, NULL);
        if (!f) return -1;
        d->h = f; d->rate = f->sampleRate; d->channels = f->channels;
        d->bits = f->bitsPerSample; d->total_frames = f->totalPCMFrameCount;
        break;
    }
    case FMT_WAV: {
        drwav *w = malloc(sizeof *w);
        if (!drwav_init_file(w, path, NULL)) { free(w); return -1; }
        d->h = w; d->rate = w->sampleRate; d->channels = w->channels;
        d->bits = w->bitsPerSample; d->total_frames = w->totalPCMFrameCount;
        break;
    }
    case FMT_MP3: {
        drmp3 *m = malloc(sizeof *m);
        if (!drmp3_init_file(m, path, NULL)) { free(m); return -1; }
        d->h = m; d->rate = m->sampleRate; d->channels = m->channels; d->bits = 16;
        d->total_frames = drmp3_get_pcm_frame_count(m);
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
