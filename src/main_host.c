// Scripted desktop run: drives the real app code with fake button presses
// and saves screenshots. Usage: HOST_ROOT=<music folder> host_test x <out_dir>
#include "app.h"
#include "platform.h"
#include "player.h"
#include "catalog.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
extern const char *host_shot_path;
extern uint32_t host_buttons;
extern int host_realtime_audio;
extern int host_touch_down, host_touch_x, host_touch_y;

static void run_ms(int ms) {
    uint64_t end = plat_time_us() + (uint64_t)ms * 1000;
    while (plat_time_us() < end) app_step(host_buttons);
}
static void tap(uint32_t b) { host_buttons = b; run_ms(60); host_buttons = 0; run_ms(60); }
static void touch(int x, int y) { host_touch_x = x; host_touch_y = y; host_touch_down = 1; }
static void lift(void) { host_touch_down = 0; run_ms(60); }
static void tap_at(int x, int y) { touch(x, y); run_ms(60); lift(); }
// finger moves from (x0,y0) to (x1,y1) over ms, lifted unless hold
static void drag(int x0, int y0, int x1, int y1, int ms, int hold) {
    touch(x0, y0);
    run_ms(30);
    for (int t = 16; t <= ms; t += 16) {
        touch(x0 + (x1 - x0) * t / ms, y0 + (y1 - y0) * t / ms);
        run_ms(16);
    }
    if (!hold) lift();
}
static char outdir[512];
static void shot(const char *name) {
    static char p[1024];
    snprintf(p, sizeof p, "%s/%s.png", outdir, name);
    app_force_redraw();
    host_shot_path = p;
    run_ms(80);
    printf("shot %s\n", p);
}

static void wait_scan(void) {
    run_ms(100);
    while (cat_scanning(NULL, NULL)) run_ms(50);
    run_ms(300); // thumbnails
}

int main(int argc, char **argv) {
    snprintf(outdir, sizeof outdir, "%s", argc > 2 ? argv[2] : ".");
    if (getenv("HOST_FRESH")) { // forget the cached index and thumbnails
        char p[600];
        snprintf(p, sizeof p, "rm -rf %s/library.idx %s/cache %s/settings.txt", plat_data_dir(), plat_data_dir(), plat_data_dir());
        if (system(p)) {}
    }
    if (app_init(getenv("ASSETS") ? getenv("ASSETS") : "assets") != 0) { fprintf(stderr, "init failed\n"); return 1; }
    const char *script = getenv("SCRIPT") ? getenv("SCRIPT") : "default";
    if (!strcmp(script, "default")) {
        run_ms(150);
        shot("01_scanning_or_empty");
        wait_scan();
        shot("02_albums");
        tap(BTN_R);
        shot("03_tracks");
        tap(BTN_R);
        shot("04_artists");
        tap(BTN_R);
        shot("05_settings");
        tap(BTN_R);                // back to Albums
        tap(BTN_DOWN);
        tap(BTN_CROSS);            // open the 2nd album
        run_ms(400);
        shot("06_album_page");
        tap(BTN_DOWN);
        tap(BTN_CROSS);            // play its 2nd track
        run_ms(800);
        shot("07_now_playing");
        tap(BTN_CIRCLE);           // back to the album page, mini player on
        run_ms(200);
        shot("08_album_page_playing");
        tap(BTN_CIRCLE);
        run_ms(200);
        shot("09_albums_playing");
        tap(BTN_L);                // to Settings (wraps), then Artists
        tap(BTN_L);
        tap(BTN_CROSS);
        run_ms(300);
        shot("10_artist_page");
        tap(BTN_TRIANGLE);         // Now playing
        tap(BTN_SELECT);           // screen off
        tap(BTN_START);            // pause while off
        tap(BTN_SELECT);           // wake
        shot("11_paused_after_wake");
    } else if (!strcmp(script, "touch")) {
        wait_scan();
        shot("t01_albums");
        drag(400, 400, 400, 200, 120, 0); // flick up
        run_ms(1500);
        shot("t02_after_fling");
        tap_at(100, 18);           // "Tracks" tab... (Albums is at x 36)
        tap_at(122, 18);
        run_ms(200);
        shot("t03_tracks_tab");
        tap_at(400, 52 + 26);      // play the first track
        run_ms(600);
        shot("t04_now_playing");
        touch(698, 430);           // press play/pause
        shot("t05_pressed");
        lift();
        tap_at(484, 430);          // shuffle
        shot("t06_shuffle");
        drag(475, 346, 698, 346, 300, 1); // scrub
        shot("t07_scrubbing");
        lift();
        tap_at(60, 526);           // "O Back"
        run_ms(200);
        shot("t08_back_with_mini");
        tap_at(758, 460);          // mini play/pause
        run_ms(100);
        shot("t09_mini_paused");
        tap_at(300, 460);          // mini card -> Now playing
        run_ms(200);
        shot("t10_np_from_mini");
    } else if (!strcmp(script, "np")) {
        // Now playing states. HOST_ROOT holds one album; plays its 3rd track.
        wait_scan();
        tap(BTN_CROSS);            // open the album
        tap(BTN_DOWN); tap(BTN_DOWN);
        tap(BTN_CROSS);            // play track 3
        run_ms(600);
        tap(BTN_CROSS);            // pause
        tap_at(550, 346);          // seek to ~17%
        run_ms(300);
        shot("np03_paused");
        tap(BTN_CROSS);            // play
        tap(BTN_SQUARE);           // shuffle on
        tap(BTN_TRIANGLE); tap(BTN_TRIANGLE); // repeat one
        run_ms(200);
        shot("np04_playing_modes");
        tap(BTN_CIRCLE);
        run_ms(300);
        shot("np05_album_page_mini");
        tap(BTN_CIRCLE);
        run_ms(300);
        shot("np06_albums_mini");
    } else if (!strcmp(script, "audio")) {
        // play the first album in order and let it run to the end (gapless check)
        host_realtime_audio = 0;
        wait_scan();
        tap(BTN_CROSS);            // open the first album
        tap(BTN_CROSS);            // play from track 1
        player_status s;
        do { run_ms(200); player_get_status(&s); } while (s.has_track);
        printf("finished\n");
    }
    return 0;
}
