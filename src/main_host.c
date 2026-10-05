// Scripted desktop run: drives the real app code with fake button presses
// and saves screenshots. Usage: host_test <music_root> <out_dir>
#include "app.h"
#include "platform.h"
#include "player.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
extern const char *host_shot_path;
extern uint32_t host_buttons;
extern int host_realtime_audio;

static void run_ms(int ms) {
    uint64_t end = plat_time_us() + (uint64_t)ms * 1000;
    while (plat_time_us() < end) app_step(host_buttons);
}
static void tap(uint32_t b) { host_buttons = b; run_ms(60); host_buttons = 0; run_ms(60); }
static char outdir[512];
static void shot(const char *name) {
    static char p[1024];
    snprintf(p, sizeof p, "%s/%s.png", outdir, name);
    app_force_redraw();
    host_shot_path = p;
    run_ms(80);
    printf("shot %s\n", p);
}

int main(int argc, char **argv) {
    snprintf(outdir, sizeof outdir, "%s", argc > 2 ? argv[2] : ".");
    if (app_init(getenv("ASSETS") ? getenv("ASSETS") : "assets") != 0) { fprintf(stderr, "init failed\n"); return 1; }
    const char *script = getenv("SCRIPT") ? getenv("SCRIPT") : "default";
    run_ms(100);
    if (!strcmp(script, "default")) {
        shot("01_browser_root");
        tap(BTN_CROSS);            // open first folder (artist)
        shot("02_browser_artist");
        tap(BTN_CROSS);            // open album
        tap(BTN_DOWN);
        shot("03_browser_album");
        tap(BTN_CROSS);            // play 2nd track
        run_ms(1500);
        shot("04_now_playing");
        tap(BTN_SQUARE);           // shuffle on
        shot("05_shuffle_toast");
        tap(BTN_SQUARE);
        tap(BTN_CIRCLE);           // back to library
        run_ms(500);
        shot("06_browser_playing");
        tap(BTN_R);                // next track
        run_ms(800);
        tap(BTN_TRIANGLE);
        shot("07_next_track");
        tap(BTN_SELECT);           // screen off
        tap(BTN_START);            // pause while off
        tap(BTN_SELECT);           // wake
        shot("08_paused_after_wake");
    } else if (!strcmp(script, "audio")) {
        // play whole tree and let it run to the end, gapless check
        host_realtime_audio = 0;
        tap(BTN_SQUARE);
        player_status s;
        do { run_ms(200); player_get_status(&s); } while (s.has_track);
        printf("finished\n");
    }
    return 0;
}
