// Desktop test build. No window: frames are written to PNG on request,
// audio is written to a WAV file, input comes from a scripted sequence.
#include "platform.h"
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <dirent.h>
#include <sys/stat.h>
#include <stdarg.h>
#include "stb_image_write.h"

static uint32_t fb[SCREEN_W * SCREEN_H];
static FILE *wav;
static uint32_t wav_frames;
static int audio_rate;
static int frame_no;
const char *host_shot_path; // if set, next present writes a PNG here
uint32_t host_buttons;      // set by test driver
int host_touch_down, host_touch_x, host_touch_y; // ditto
int host_realtime_audio = 1;

int plat_init(void) { return 0; }
void plat_shutdown(void) {}
uint32_t *plat_backbuffer(int *stride) { *stride = SCREEN_W; return fb; }

void plat_present(void) {
    frame_no++;
    if (host_shot_path) {
        stbi_write_png(host_shot_path, SCREEN_W, SCREEN_H, 4, fb, SCREEN_W * 4);
        host_shot_path = NULL;
    }
}
void plat_display_off(void) { fprintf(stderr, "[host] display off\n"); }
void plat_display_on(void) { fprintf(stderr, "[host] display on\n"); }
void plat_keep_awake(void) {}
int plat_focus_event(void) { return 0; }
uint32_t plat_buttons(void) { return host_buttons; }
int plat_touch(int *x, int *y) { *x = host_touch_x; *y = host_touch_y; return host_touch_down; }

uint64_t plat_time_us(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (uint64_t)t.tv_sec * 1000000 + t.tv_nsec / 1000;
}
void plat_sleep_us(uint32_t us) { usleep(us); }
void plat_clock_text(char *buf, size_t n) { snprintf(buf, n, "17:54"); }
int plat_battery_percent(void) { return 82; }
int plat_battery_charging(void) { return 0; }

struct plat_mutex { pthread_mutex_t m; };
plat_mutex *plat_mutex_create(void) {
    plat_mutex *m = malloc(sizeof *m);
    pthread_mutex_init(&m->m, NULL);
    return m;
}
void plat_mutex_lock(plat_mutex *m) { pthread_mutex_lock(&m->m); }
void plat_mutex_unlock(plat_mutex *m) { pthread_mutex_unlock(&m->m); }

typedef struct { plat_thread_fn fn; void *arg; } ts;
static void *entry(void *p) { ts s = *(ts *)p; free(p); s.fn(s.arg); return NULL; }
int plat_thread_start(plat_thread_fn fn, void *arg, int hp) {
    (void)hp;
    ts *s = malloc(sizeof *s); s->fn = fn; s->arg = arg;
    pthread_t t;
    return pthread_create(&t, NULL, entry, s);
}

int plat_audio_rate_supported(int rate) {
    switch (rate) {
    case 8000: case 11025: case 12000: case 16000: case 22050:
    case 24000: case 32000: case 44100: case 48000: return 1;
    }
    return 0;
}
static void wav_header(void) {
    uint32_t data = wav_frames * 4;
    uint8_t h[44];
    memcpy(h, "RIFF", 4); *(uint32_t *)(h + 4) = 36 + data; memcpy(h + 8, "WAVEfmt ", 8);
    *(uint32_t *)(h + 16) = 16; *(uint16_t *)(h + 20) = 1; *(uint16_t *)(h + 22) = 2;
    *(uint32_t *)(h + 24) = audio_rate; *(uint32_t *)(h + 28) = audio_rate * 4;
    *(uint16_t *)(h + 32) = 4; *(uint16_t *)(h + 34) = 16; memcpy(h + 36, "data", 4);
    *(uint32_t *)(h + 40) = data;
    fseek(wav, 0, SEEK_SET); fwrite(h, 1, 44, wav); fseek(wav, 0, SEEK_END);
}
int plat_audio_open(int rate) {
    const char *p = getenv("HOST_WAV");
    audio_rate = rate;
    wav = fopen(p ? p : "/dev/null", "wb+");
    wav_header();
    return rate;
}
int plat_audio_set_rate(int rate) {
    if (!plat_audio_rate_supported(rate)) return -1;
    if (rate != audio_rate) fprintf(stderr, "[host] audio rate -> %d\n", rate);
    audio_rate = rate;
    return 0;
}
void plat_audio_output(const int16_t *f) {
    fwrite(f, 4, AUDIO_GRAIN, wav);
    wav_frames += AUDIO_GRAIN;
    wav_header();
    if (host_realtime_audio) usleep(1000000ull * AUDIO_GRAIN / audio_rate);
}
void plat_audio_drain(void) {}

int plat_list_dir(const char *path, plat_dirent **out) {
    DIR *d = opendir(path);
    if (!d) return -1;
    int cap = 64, n = 0;
    plat_dirent *v = malloc(cap * sizeof *v);
    struct dirent *e;
    while ((e = readdir(d))) {
        if (e->d_name[0] == '.') continue;
        if (n == cap) { cap *= 2; v = realloc(v, cap * sizeof *v); }
        snprintf(v[n].name, sizeof v[n].name, "%s", e->d_name);
        char full[1024];
        snprintf(full, sizeof full, "%s/%s", path, e->d_name);
        struct stat st;
        v[n].is_dir = stat(full, &st) == 0 && S_ISDIR(st.st_mode);
        n++;
    }
    closedir(d);
    *out = v;
    return n;
}
int plat_path_exists(const char *p) { struct stat st; return stat(p, &st) == 0; }
void plat_mkdir(const char *p) { mkdir(p, 0777); }
const char *plat_data_dir(void) { const char *d = getenv("HOST_DATA"); return d ? d : "/tmp/fidelity-data"; }
const char *plat_root_dir(void) { const char *r = getenv("HOST_ROOT"); return r ? r : "."; }
int plat_devices(char names[][16], int max) { (void)max; strcpy(names[0], "ux0:"); strcpy(names[1], "uma0:"); return 2; }
void plat_log(const char *fmt, ...) {
    va_list ap; va_start(ap, fmt);
    fprintf(stderr, "[log] "); vfprintf(stderr, fmt, ap); fputc('\n', stderr);
    va_end(ap);
}
