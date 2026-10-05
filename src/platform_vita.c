#include "platform.h"
#include <psp2/kernel/processmgr.h>
#include <psp2/kernel/threadmgr.h>
#include <psp2/kernel/sysmem.h>
#include <psp2/display.h>
#include <psp2/ctrl.h>
#include <psp2/touch.h>
#include <psp2/audioout.h>
#include <psp2/power.h>
#include <psp2/rtc.h>
#include <psp2/avconfig.h>
#include <psp2/registrymgr.h>
#include <psp2/appmgr.h>
#include <psp2/io/dirent.h>
#include <psp2/io/stat.h>
#include <psp2/io/fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <malloc.h>
#include <stdarg.h>

// Give newlib a big heap (cover art, glyph cache, decoders).
int _newlib_heap_size_user = 160 * 1024 * 1024;

#define FB_SIZE (2 * 1024 * 1024) // 960*544*4 rounded to 256 KiB
static SceUID fb_block[2];
static void *fb_mem[2];
static int fb_cur;
static uint32_t *render_buf; // cached RAM; copied to CDRAM on present

static int audio_port = -1;
static int audio_rate;
static int main_port_fallback; // BGM port refused: locked to 48 kHz

int plat_init(void) {
    for (int i = 0; i < 2; i++) {
        fb_block[i] = sceKernelAllocMemBlock("fb", SCE_KERNEL_MEMBLOCK_TYPE_USER_CDRAM_RW, FB_SIZE, NULL);
        if (fb_block[i] < 0) return -1;
        sceKernelGetMemBlockBase(fb_block[i], &fb_mem[i]);
        memset(fb_mem[i], 0, FB_SIZE);
    }
    render_buf = (uint32_t *)memalign(64, SCREEN_W * SCREEN_H * 4);
    sceCtrlSetSamplingMode(SCE_CTRL_MODE_DIGITAL);
    sceTouchSetSamplingState(SCE_TOUCH_PORT_FRONT, SCE_TOUCH_SAMPLING_STATE_START);
    // Decoding FLAC is cheap, but give the UI headroom for cover art scaling.
    scePowerSetArmClockFrequency(333);
    return 0;
}

void plat_shutdown(void) {
    plat_display_on();
    if (audio_port >= 0) sceAudioOutReleasePort(audio_port);
}

uint32_t *plat_backbuffer(int *stride) { *stride = SCREEN_W; return render_buf; }

void plat_present(void) {
    fb_cur ^= 1;
    memcpy(fb_mem[fb_cur], render_buf, SCREEN_W * SCREEN_H * 4);
    SceDisplayFrameBuf f;
    memset(&f, 0, sizeof f);
    f.size = sizeof f;
    f.base = fb_mem[fb_cur];
    f.pitch = SCREEN_W;
    f.pixelformat = SCE_DISPLAY_PIXELFORMAT_A8B8G8R8;
    f.width = SCREEN_W;
    f.height = SCREEN_H;
    sceDisplaySetFrameBuf(&f, SCE_DISPLAY_SETBUF_NEXTFRAME);
    sceDisplayWaitVblankStart();
}

// Screen off = backlight to 0. scePowerRequestDisplayOff() puts the console
// into its own screen-off power state, which also stops our audio, so we
// leave the power state alone and only kill the panel brightness.
static int saved_brightness = -1;
static int display_is_off;

void plat_display_off(void) {
    int b = 0;
    if (sceRegMgrGetKeyInt("/CONFIG/DISPLAY", "brightness", &b) >= 0 && b > 0) saved_brightness = b;
    int r = sceAVConfigSetDisplayBrightness(0);
    plat_log("display off: saved brightness %d, set 0 -> 0x%08X", saved_brightness, r);
    sceTouchSetSamplingState(SCE_TOUCH_PORT_FRONT, SCE_TOUCH_SAMPLING_STATE_STOP); // pocket: no stray touches
    display_is_off = 1;
}

void plat_display_on(void) {
    if (!display_is_off) return;
    int b = saved_brightness;
    if (b <= 0) {
        int max = 0;
        sceAVConfigGetDisplayMaxBrightness(&max);
        b = max > 0 ? max * 6 / 10 : 40000;
    }
    int r = sceAVConfigSetDisplayBrightness(b);
    plat_log("display on: brightness %d -> 0x%08X", b, r);
    sceTouchSetSamplingState(SCE_TOUCH_PORT_FRONT, SCE_TOUCH_SAMPLING_STATE_START);
    display_is_off = 0;
}

// Resetting the power tick also keeps the system's own idle dim/screen-off
// from kicking in while music plays (that path pauses audio too).
void plat_keep_awake(void)  { sceKernelPowerTick(SCE_KERNEL_POWER_TICK_DEFAULT); }

// Older VitaSDK headers lack ON_DEACTIVATE and REQUEST_QUIT, so use the values.
#define SYSEV_ON_DEACTIVATE 0x10000002
#define SYSEV_ON_RESUME     0x10000003
#define SYSEV_REQUEST_QUIT  0x20000001

int plat_focus_event(void) {
    SceAppMgrSystemEvent ev;
    int hit = 0;
    while (sceAppMgrReceiveSystemEvent(&ev) == 0) {
        if (ev.systemEvent == SYSEV_ON_DEACTIVATE ||
            ev.systemEvent == SYSEV_ON_RESUME ||
            ev.systemEvent == SYSEV_REQUEST_QUIT) {
            plat_log("system event 0x%08X", ev.systemEvent);
            hit = 1;
        }
    }
    return hit;
}

uint32_t plat_buttons(void) {
    SceCtrlData pad;
    memset(&pad, 0, sizeof pad);
    sceCtrlPeekBufferPositive(0, &pad, 1);
    uint32_t b = 0, s = pad.buttons;
    if (s & SCE_CTRL_UP) b |= BTN_UP;
    if (s & SCE_CTRL_DOWN) b |= BTN_DOWN;
    if (s & SCE_CTRL_LEFT) b |= BTN_LEFT;
    if (s & SCE_CTRL_RIGHT) b |= BTN_RIGHT;
    if (s & SCE_CTRL_CROSS) b |= BTN_CROSS;
    if (s & SCE_CTRL_CIRCLE) b |= BTN_CIRCLE;
    if (s & SCE_CTRL_SQUARE) b |= BTN_SQUARE;
    if (s & SCE_CTRL_TRIANGLE) b |= BTN_TRIANGLE;
    if (s & SCE_CTRL_LTRIGGER) b |= BTN_L;
    if (s & SCE_CTRL_RTRIGGER) b |= BTN_R;
    if (s & SCE_CTRL_START) b |= BTN_START;
    if (s & SCE_CTRL_SELECT) b |= BTN_SELECT;
    return b;
}

// The front panel reports 1920x1088; the screen is 960x544.
int plat_touch(int *x, int *y) {
    SceTouchData t;
    memset(&t, 0, sizeof t);
    if (sceTouchPeek(SCE_TOUCH_PORT_FRONT, &t, 1) < 0 || t.reportNum == 0) return 0;
    *x = t.report[0].x * SCREEN_W / 1920;
    *y = t.report[0].y * SCREEN_H / 1088;
    return 1;
}

uint64_t plat_time_us(void) { return sceKernelGetProcessTimeWide(); }
void plat_sleep_us(uint32_t us) { sceKernelDelayThread(us); }

void plat_clock_text(char *buf, size_t n) {
    SceDateTime t;
    sceRtcGetCurrentClockLocalTime(&t);
    snprintf(buf, n, "%02d:%02d", t.hour, t.minute);
}
int plat_battery_percent(void) { return scePowerGetBatteryLifePercent(); }
int plat_battery_charging(void) { return scePowerIsBatteryCharging(); }

struct plat_mutex { SceUID id; };
plat_mutex *plat_mutex_create(void) {
    plat_mutex *m = malloc(sizeof *m);
    m->id = sceKernelCreateMutex("fid_mtx", 0, 0, NULL);
    return m;
}
void plat_mutex_lock(plat_mutex *m) { sceKernelLockMutex(m->id, 1, NULL); }
void plat_mutex_unlock(plat_mutex *m) { sceKernelUnlockMutex(m->id, 1); }

typedef struct { plat_thread_fn fn; void *arg; } thread_start;
static int thread_entry(SceSize args, void *argp) {
    thread_start *s = (thread_start *)argp;
    return s->fn(s->arg);
}
int plat_thread_start(plat_thread_fn fn, void *arg, int high_priority) {
    thread_start s = { fn, arg };
    SceUID t = sceKernelCreateThread("fid_thread", thread_entry,
                                     high_priority ? 0x50 : 0xA0,
                                     256 * 1024, 0, 0, NULL);
    if (t < 0) return -1;
    return sceKernelStartThread(t, sizeof s, &s); // args are copied
}

int plat_audio_rate_supported(int rate) {
    if (main_port_fallback) return rate == 48000;
    switch (rate) {
    case 8000: case 11025: case 12000: case 16000: case 22050:
    case 24000: case 32000: case 44100: case 48000: return 1;
    }
    return 0;
}

int plat_audio_open(int rate) {
    // The BGM port accepts 44.1 kHz natively, so CD-rate files are not
    // resampled by us. Turn off the BGM dynamic normalizer: we want the
    // signal untouched.
    audio_port = sceAudioOutOpenPort(SCE_AUDIO_OUT_PORT_TYPE_BGM, AUDIO_GRAIN, rate, SCE_AUDIO_OUT_MODE_STEREO);
    if (audio_port >= 0) {
        int r = sceAudioOutSetAlcMode(SCE_AUDIO_ALC_OFF);
        plat_log("audio: BGM port %d at %d Hz (alc off: 0x%08X)", audio_port, rate, r);
    } else {
        plat_log("audio: BGM port failed 0x%08X, falling back to MAIN 48 kHz", audio_port);
        audio_port = sceAudioOutOpenPort(SCE_AUDIO_OUT_PORT_TYPE_MAIN, AUDIO_GRAIN, 48000, SCE_AUDIO_OUT_MODE_STEREO);
        if (audio_port < 0) { plat_log("audio: MAIN port failed 0x%08X", audio_port); return audio_port; }
        main_port_fallback = 1;
        rate = 48000;
    }
    int vol[2] = { SCE_AUDIO_VOLUME_0DB, SCE_AUDIO_VOLUME_0DB };
    sceAudioOutSetVolume(audio_port, SCE_AUDIO_VOLUME_FLAG_L_CH | SCE_AUDIO_VOLUME_FLAG_R_CH, vol);
    audio_rate = rate;
    return rate;
}

int plat_audio_set_rate(int rate) {
    if (!plat_audio_rate_supported(rate)) return -1;
    if (rate == audio_rate) return 0;
    sceAudioOutOutput(audio_port, NULL); // let queued audio finish first
    int r = sceAudioOutSetConfig(audio_port, -1, rate, -1);
    if (r < 0) { plat_log("audio: set rate %d failed 0x%08X", rate, r); return r; }
    audio_rate = rate;
    return 0;
}

void plat_audio_output(const int16_t *frames) { sceAudioOutOutput(audio_port, frames); }
void plat_audio_drain(void) { sceAudioOutOutput(audio_port, NULL); }

int plat_list_dir(const char *path, plat_dirent **out) {
    SceUID d = sceIoDopen(path);
    if (d < 0) return -1;
    int cap = 64, n = 0;
    plat_dirent *v = malloc(cap * sizeof *v);
    SceIoDirent e;
    for (;;) {
        memset(&e, 0, sizeof e);
        if (sceIoDread(d, &e) <= 0) break;
        if (e.d_name[0] == '.') continue;
        if (n == cap) { cap *= 2; v = realloc(v, cap * sizeof *v); }
        strncpy(v[n].name, e.d_name, sizeof v[n].name - 1);
        v[n].name[sizeof v[n].name - 1] = 0;
        v[n].is_dir = SCE_S_ISDIR(e.d_stat.st_mode);
        n++;
    }
    sceIoDclose(d);
    *out = v;
    return n;
}

int plat_path_exists(const char *path) {
    SceIoStat st;
    return sceIoGetstat(path, &st) >= 0;
}
void plat_mkdir(const char *path) { sceIoMkdir(path, 0777); }
const char *plat_data_dir(void) { return "ux0:data/Fidelity"; }
const char *plat_root_dir(void) { return "ux0:"; }

int plat_devices(char names[][16], int max) {
    static const char *cands[] = { "ux0:", "uma0:", "imc0:", "ur0:", "xmc0:", "grw0:" };
    int n = 0;
    for (unsigned i = 0; i < sizeof cands / sizeof *cands && n < max; i++) {
        SceUID d = sceIoDopen(cands[i]);
        if (d >= 0) { sceIoDclose(d); strcpy(names[n++], cands[i]); }
    }
    return n;
}

void plat_log(const char *fmt, ...) {
    char path[128];
    snprintf(path, sizeof path, "%s/log.txt", plat_data_dir());
    FILE *f = fopen(path, "a");
    if (!f) return;
    va_list ap;
    va_start(ap, fmt);
    vfprintf(f, fmt, ap);
    va_end(ap);
    fputc('\n', f);
    fclose(f);
}
