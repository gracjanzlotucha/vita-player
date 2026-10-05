// Thin platform layer: everything Vita-specific lives behind this header.
// platform_vita.c implements it for the console, platform_host.c for a
// desktop test build (renders frames to PNG, audio to WAV).
#pragma once
#include <stdint.h>
#include <stddef.h>

#define SCREEN_W 960
#define SCREEN_H 544

// Buttons (bitmask)
enum {
    BTN_UP = 1 << 0, BTN_DOWN = 1 << 1, BTN_LEFT = 1 << 2, BTN_RIGHT = 1 << 3,
    BTN_CROSS = 1 << 4, BTN_CIRCLE = 1 << 5, BTN_SQUARE = 1 << 6, BTN_TRIANGLE = 1 << 7,
    BTN_L = 1 << 8, BTN_R = 1 << 9, BTN_START = 1 << 10, BTN_SELECT = 1 << 11,
};

int  plat_init(void);
void plat_shutdown(void);

// Display. plat_backbuffer returns RGBA8888 (R in low byte), stride in pixels.
uint32_t *plat_backbuffer(int *stride);
void plat_present(void);           // flip + vsync
void plat_display_off(void);
void plat_display_on(void);
void plat_keep_awake(void);        // call periodically while playing
// Returns 1 when the app lost focus / resumed from sleep (PS button, power).
int  plat_focus_event(void);

// Input
uint32_t plat_buttons(void);

// Time
uint64_t plat_time_us(void);
void plat_sleep_us(uint32_t us);
void plat_clock_text(char *buf, size_t n); // "17:54"
int  plat_battery_percent(void);           // -1 if unknown
int  plat_battery_charging(void);

// Threads / locks
typedef struct plat_mutex plat_mutex;
plat_mutex *plat_mutex_create(void);
void plat_mutex_lock(plat_mutex *m);
void plat_mutex_unlock(plat_mutex *m);
typedef int (*plat_thread_fn)(void *arg);
int  plat_thread_start(plat_thread_fn fn, void *arg, int high_priority);

// Audio output: signed 16-bit stereo interleaved.
#define AUDIO_GRAIN 1024
int  plat_audio_open(int rate);          // returns the rate actually opened, <0 on error
int  plat_audio_set_rate(int rate);      // returns 0 ok, <0 if rate unsupported
int  plat_audio_rate_supported(int rate);
void plat_audio_output(const int16_t *frames); // blocks; AUDIO_GRAIN frames
void plat_audio_drain(void);

// Filesystem
typedef struct { char name[256]; int is_dir; } plat_dirent;
// Lists a directory. Returns count or -1. Caller frees *out.
int  plat_list_dir(const char *path, plat_dirent **out);
int  plat_path_exists(const char *path);
void plat_mkdir(const char *path);
const char *plat_data_dir(void);   // writable dir for settings
const char *plat_root_dir(void);   // starting browse dir
void plat_log(const char *fmt, ...); // appends to <data_dir>/log.txt
// Root device list (e.g. ux0:, uma0:). Returns count, fills names.
int  plat_devices(char names[][16], int max);
