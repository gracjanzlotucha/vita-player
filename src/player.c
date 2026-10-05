// Playback engine: one high-priority thread decodes, converts to the
// output format and feeds the audio port. Tracks are chained inside the
// same output buffer, so playback is gapless whenever consecutive tracks
// share a sample rate.
//
// Signal path, in order of preference:
//   16-bit source at a rate the port supports  -> bit-exact passthrough
//   24/32-bit source at a supported rate        -> TPDF dither to 16-bit
//   unsupported rate (88.2k, 96k, 192k, ...)     -> windowed-sinc resample
//                                                  to 44.1k/48k + dither
#include "player.h"
#include "platform.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

static plat_mutex *m;

// ---- shared state (guarded by m) ----
static char **queue;
static int qn;
static int *order;
static int opos = -1;
static int shuffle_on, repeat_mode;
static int paused;
static int cmd_play = -1;       // order position to start
static int64_t cmd_seek = -1;   // absolute source frame
static player_status st;

// ---- audio thread state ----
static decoder dec;
static int dec_open;
static int out_rate;
static int need_resample;
static int16_t outbuf[AUDIO_GRAIN * 2];
static int32_t srcbuf[4096 * 2];

// ---------- dither ----------
static uint32_t rng = 0x12345678;
static inline uint32_t rnd16(void) { rng = rng * 1664525u + 1013904223u; return rng >> 16; }

static inline int16_t clamp16(int32_t v) { return v > 32767 ? 32767 : v < -32768 ? -32768 : (int16_t)v; }

static void s32_to_s16(const int32_t *in, int16_t *out, uint32_t samples, int exact) {
    if (exact) {
        for (uint32_t i = 0; i < samples; i++) out[i] = (int16_t)(in[i] >> 16);
        return;
    }
    for (uint32_t i = 0; i < samples; i++) {
        // TPDF dither, +-1 LSB at 16-bit
        int64_t v = (int64_t)in[i] + (int32_t)rnd16() - (int32_t)rnd16() + 32768;
        out[i] = clamp16((int32_t)(v >> 16));
    }
}

// ---------- resampler ----------
#define RS_ZC 8          // zero crossings per side
#define RS_RES 256       // table points per zero crossing
#define RS_CAP 32768
static float ktab[RS_ZC * RS_RES + 2];
static float fifo[2][RS_CAP];
static int fifo_len;             // valid samples in fifo
static double rs_t;              // fractional read position
static double rs_step;           // src samples per output sample
static double rs_fc;             // cutoff relative to source Nyquist
static int rs_half;              // half-width in source samples
static int rs_eof;

static void rs_build_table(void) {
    for (int i = 0; i <= RS_ZC * RS_RES + 1; i++) {
        double x = (double)i / RS_RES;
        double s = x == 0 ? 1.0 : sin(M_PI * x) / (M_PI * x);
        double w = x >= RS_ZC ? 0 : 0.42 + 0.5 * cos(M_PI * x / RS_ZC) + 0.08 * cos(2 * M_PI * x / RS_ZC);
        ktab[i] = (float)(s * w);
    }
}

static inline float kern(double x) {
    if (x < 0) x = -x;
    double p = x * RS_RES;
    int i = (int)p;
    if (i >= RS_ZC * RS_RES) return 0;
    float f = (float)(p - i);
    return ktab[i] + (ktab[i + 1] - ktab[i]) * f;
}

static void rs_reset(uint32_t src, uint32_t dst) {
    rs_step = (double)src / dst;
    rs_fc = (dst < src ? (double)dst / src : 1.0) * 0.95;
    rs_half = (int)ceil(RS_ZC / rs_fc) + 1;
    memset(fifo, 0, sizeof fifo);
    fifo_len = rs_half;          // leading silence so the first output is centred
    rs_t = rs_half;
    rs_eof = 0;
}

static uint64_t src_pos; // source frames consumed (for the UI)

// Produce up to n output frames; returns 0 at end of track.
static uint32_t rs_produce(int16_t *out, uint32_t n) {
    uint32_t made = 0;
    while (made < n) {
        int need = (int)rs_t + rs_half + 1;
        if (need >= fifo_len) {
            if (rs_eof) break;
            // compact
            int drop = (int)rs_t - rs_half - 1;
            if (drop > RS_CAP / 2) {
                memmove(fifo[0], fifo[0] + drop, (fifo_len - drop) * sizeof(float));
                memmove(fifo[1], fifo[1] + drop, (fifo_len - drop) * sizeof(float));
                fifo_len -= drop; rs_t -= drop;
            }
            uint32_t room = RS_CAP - fifo_len;
            if (room > 4096) room = 4096;
            uint32_t got = decoder_read(&dec, srcbuf, room);
            if (got == 0) {
                // flush the filter tail with silence
                int pad = rs_half * 2 + 2;
                if (fifo_len + pad > RS_CAP) pad = RS_CAP - fifo_len;
                memset(fifo[0] + fifo_len, 0, pad * sizeof(float));
                memset(fifo[1] + fifo_len, 0, pad * sizeof(float));
                fifo_len += pad;
                rs_eof = 1;
                continue;
            }
            src_pos += got;
            const float k = 1.0f / 2147483648.0f;
            for (uint32_t i = 0; i < got; i++) {
                fifo[0][fifo_len + i] = srcbuf[i * 2] * k;
                fifo[1][fifo_len + i] = srcbuf[i * 2 + 1] * k;
            }
            fifo_len += got;
            continue;
        }
        int c = (int)rs_t;
        double frac = rs_t - c;
        float l = 0, r = 0;
        for (int i = c - rs_half + 1; i <= c + rs_half; i++) {
            float w = kern((rs_t - i) * rs_fc);
            l += fifo[0][i] * w;
            r += fifo[1][i] * w;
        }
        (void)frac;
        l *= (float)rs_fc; r *= (float)rs_fc;
        float d1 = ((int)rnd16() - (int)rnd16()) / 65536.0f;
        float d2 = ((int)rnd16() - (int)rnd16()) / 65536.0f;
        out[made * 2]     = clamp16((int32_t)lrintf(l * 32767.0f + d1));
        out[made * 2 + 1] = clamp16((int32_t)lrintf(r * 32767.0f + d2));
        made++;
        rs_t += rs_step;
    }
    return made;
}

static uint32_t produce(int16_t *out, uint32_t n) {
    if (need_resample) return rs_produce(out, n);
    uint32_t got = decoder_read(&dec, srcbuf, n);
    src_pos += got;
    s32_to_s16(srcbuf, out, got * 2, dec.bits <= 16);
    return got;
}

static int pick_out_rate(uint32_t src) {
    if (plat_audio_rate_supported((int)src)) return (int)src;
    int r = (src % 44100 == 0 || src % 11025 == 0) ? 44100 : 48000;
    return plat_audio_rate_supported(r) ? r : 48000;
}

// ---------- queue helpers (call with m held) ----------
static void build_order(int keep_current) {
    int cur = (opos >= 0 && opos < qn) ? order[opos] : -1;
    for (int i = 0; i < qn; i++) order[i] = i;
    if (shuffle_on) {
        for (int i = qn - 1; i > 0; i--) {
            int j = rand() % (i + 1);
            int t = order[i]; order[i] = order[j]; order[j] = t;
        }
    }
    if (keep_current && cur >= 0) {
        for (int i = 0; i < qn; i++) if (order[i] == cur) {
            if (shuffle_on) { order[i] = order[0]; order[0] = cur; opos = 0; }
            else opos = i;
            break;
        }
    }
}

// Next position after track end. -1 = stop.
static int auto_next(void) {
    if (qn == 0) return -1;
    if (repeat_mode == REPEAT_ONE) return opos;
    if (opos + 1 < qn) return opos + 1;
    if (repeat_mode == REPEAT_ALL) {
        if (shuffle_on) build_order(0);
        return 0;
    }
    return -1;
}

// ---------- audio thread ----------
static void flush_partial(int *fill) {
    if (*fill == 0) return;
    memset(outbuf + *fill * 2, 0, (AUDIO_GRAIN - *fill) * 4);
    plat_audio_output(outbuf);
    *fill = 0;
}

static int open_at(int pos, int *fill) {
    // Called without m held.
    char path[1024];
    plat_mutex_lock(m);
    if (pos < 0 || pos >= qn) { plat_mutex_unlock(m); return -1; }
    snprintf(path, sizeof path, "%s", queue[order[pos]]);
    plat_mutex_unlock(m);

    if (dec_open) { decoder_close(&dec); dec_open = 0; }
    int ok = decoder_open(&dec, path) == 0;
    if (!ok) plat_log("can't open: %s", path);

    plat_mutex_lock(m);
    opos = pos;
    st.serial++;
    snprintf(st.path, sizeof st.path, "%s", path);
    st.error = !ok;
    if (ok) {
        dec_open = 1;
        src_pos = 0;
        int want = pick_out_rate(dec.rate);
        st.rate = dec.rate; st.bits = dec.bits; st.channels = dec.channels;
        st.total = dec.total_frames; st.fmt = dec.fmt; st.pos = 0;
        plat_mutex_unlock(m);
        if (want != out_rate) {
            flush_partial(fill);
            if (plat_audio_set_rate(want) == 0) out_rate = want;
        }
        need_resample = (int)dec.rate != out_rate;
        if (need_resample) rs_reset(dec.rate, out_rate);
        plat_mutex_lock(m);
        st.out_rate = out_rate;
    }
    plat_mutex_unlock(m);
    return ok ? 0 : -1;
}

static void stop_playback(int *fill) {
    flush_partial(fill);
    if (dec_open) { decoder_close(&dec); dec_open = 0; }
    plat_mutex_lock(m);
    st.has_track = 0;
    paused = 1;
    st.serial++;
    plat_mutex_unlock(m);
}

static int audio_thread(void *arg) {
    (void)arg;
    int fill = 0;
    for (;;) {
        plat_mutex_lock(m);
        int play = cmd_play; cmd_play = -1;
        int64_t seek = cmd_seek; cmd_seek = -1;
        int is_paused = paused;
        plat_mutex_unlock(m);

        if (play >= 0) {
            fill = 0; // drop whatever was half-built from the old track
            int tries = 0;
            plat_mutex_lock(m);
            int n = qn;
            plat_mutex_unlock(m);
            while (open_at(play, &fill) != 0) {
                // unreadable file: skip forward
                if (++tries >= n) { stop_playback(&fill); break; }
                plat_mutex_lock(m);
                play = (play + 1) % qn;
                plat_mutex_unlock(m);
            }
            if (dec_open) {
                plat_mutex_lock(m);
                st.has_track = 1;
                plat_mutex_unlock(m);
            }
            continue;
        }
        if (seek >= 0 && dec_open) {
            decoder_seek(&dec, (uint64_t)seek);
            src_pos = (uint64_t)seek;
            if (need_resample) rs_reset(dec.rate, out_rate);
            fill = 0;
        }
        if (!dec_open || is_paused) { plat_sleep_us(10000); continue; }

        while (fill < AUDIO_GRAIN) {
            uint32_t got = produce(outbuf + fill * 2, AUDIO_GRAIN - fill);
            if (got) {
                fill += got;
                plat_mutex_lock(m);
                st.pos = src_pos;
                plat_mutex_unlock(m);
                continue;
            }
            // track finished: chain the next one into the same buffer
            plat_mutex_lock(m);
            int nxt = auto_next();
            plat_mutex_unlock(m);
            if (nxt < 0 || open_at(nxt, &fill) != 0) { stop_playback(&fill); break; }
        }
        if (fill == AUDIO_GRAIN) {
            plat_audio_output(outbuf);
            fill = 0;
        }
    }
    return 0;
}

// ---------- public API ----------
void player_init(void) {
    m = plat_mutex_create();
    rs_build_table();
    out_rate = plat_audio_open(44100);
    if (out_rate < 0) out_rate = 48000;
    st.out_rate = out_rate;
    paused = 1;
    plat_thread_start(audio_thread, NULL, 1);
}

void player_set_queue(char **paths, int n, int start) {
    plat_mutex_lock(m);
    for (int i = 0; i < qn; i++) free(queue[i]);
    free(queue); free(order);
    queue = malloc(n * sizeof *queue);
    order = malloc(n * sizeof *order);
    for (int i = 0; i < n; i++) queue[i] = strdup(paths[i]);
    qn = n;
    opos = -1;
    for (int i = 0; i < n; i++) order[i] = i;
    if (shuffle_on) {
        build_order(0);
        // put the chosen track first
        for (int i = 0; i < n; i++) if (order[i] == start) { order[i] = order[0]; order[0] = start; break; }
        cmd_play = 0;
    } else {
        cmd_play = start;
    }
    paused = 0;
    st.count = n;
    plat_mutex_unlock(m);
}

void player_toggle_pause(void) {
    plat_mutex_lock(m);
    if (st.has_track) paused = !paused;
    else if (qn > 0) { cmd_play = 0; paused = 0; } // restart a finished queue
    plat_mutex_unlock(m);
}

void player_next(void) {
    plat_mutex_lock(m);
    if (qn > 0) {
        int nxt = opos + 1;
        if (nxt >= qn) { if (shuffle_on) build_order(0); nxt = 0; }
        cmd_play = nxt;
        paused = 0;
    }
    plat_mutex_unlock(m);
}

void player_prev(void) {
    plat_mutex_lock(m);
    if (qn > 0) {
        if (st.has_track && st.rate && st.pos > (uint64_t)st.rate * 3) cmd_seek = 0;
        else cmd_play = opos > 0 ? opos - 1 : (repeat_mode == REPEAT_ALL ? qn - 1 : 0);
        paused = 0;
    }
    plat_mutex_unlock(m);
}

void player_seek_rel(int seconds) {
    plat_mutex_lock(m);
    if (st.has_track && st.rate) {
        int64_t base = cmd_seek >= 0 ? cmd_seek : (int64_t)st.pos;
        int64_t t = base + (int64_t)seconds * st.rate;
        if (t < 0) t = 0;
        if (st.total && t >= (int64_t)st.total) t = st.total - 1;
        cmd_seek = t;
        st.pos = (uint64_t)t; // reflect immediately in the UI
    }
    plat_mutex_unlock(m);
}

void player_set_shuffle(int on) {
    plat_mutex_lock(m);
    shuffle_on = on;
    if (qn) build_order(1);
    plat_mutex_unlock(m);
}

void player_cycle_repeat(void) {
    plat_mutex_lock(m);
    repeat_mode = (repeat_mode + 1) % 3;
    plat_mutex_unlock(m);
}

void player_set_modes(int sh, int rep) {
    plat_mutex_lock(m);
    shuffle_on = sh; repeat_mode = rep;
    plat_mutex_unlock(m);
}

void player_get_status(player_status *s) {
    plat_mutex_lock(m);
    *s = st;
    s->paused = paused;
    s->index = opos;
    s->count = qn;
    s->shuffle = shuffle_on;
    s->repeat = repeat_mode;
    plat_mutex_unlock(m);
}
