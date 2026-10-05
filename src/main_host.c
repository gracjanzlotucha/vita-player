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
    } else if (!strcmp(script, "touch")) {
        shot("t01_root");
        tap_at(300, 80 + 26);      // first row: open folder
        tap_at(300, 80 + 26);      // open album
        shot("t02_album");
        drag(300, 400, 300, 250, 120, 0); // flick up
        run_ms(100);
        shot("t03_flinging");
        run_ms(1500);
        shot("t04_after_fling");
        touch(300, 80 + 52 * 3 + 26); // finger resting on a row
        run_ms(100);
        shot("t05a_row_pressed");
        drag(300, 80 + 52 * 3 + 26, 300, 380, 400, 1); // ...becomes a slow drag down
        shot("t05_dragging");
        lift();
        tap(BTN_DOWN);             // selection was scrolled away: comes back on screen
        shot("t05b_dpad_after_scroll");
        tap_at(300, 80 + 52 * 2 + 26); // play the third visible row
        run_ms(800);
        shot("t06_now_playing");
        touch(698, 416);           // press play/pause: pressed highlight
        shot("t07_pressed");
        lift();                    // ...release pauses
        shot("t08_paused");
        tap_at(484, 416);          // shuffle
        shot("t09_shuffle");
        drag(475, 330, 698, 330, 300, 1); // scrub to the middle
        shot("t10_scrubbing");
        lift();
        shot("t11_after_seek");
        tap_at(786, 416);          // next
        run_ms(300);
        shot("t12_next");
        tap_at(760, 526);          // "Library" hint
        run_ms(200);
        shot("t13_library_hint");
        tap_at(896, 465);          // mini player play/pause
        shot("t14_mini_toggle");
        tap_at(300, 465);          // mini player card -> now playing
        shot("t15_mini_open");
        tap_at(880, 526);          // "Screen off" hint
        tap_at(698, 416);          // ignored while the screen is off
        tap(BTN_SELECT);           // wake
        shot("t16_after_wake");

    } else if (!strcmp(script, "np")) {
        // Now playing states. HOST_ROOT = an album folder; plays its 3rd file.
        shot("np01_nothing_playing_lib");
        tap(BTN_TRIANGLE);
        shot("np02_not_playing");
        tap(BTN_CIRCLE);
        tap(BTN_DOWN); tap(BTN_DOWN);
        tap(BTN_CROSS);
        run_ms(600);
        tap(BTN_CROSS);            // pause
        tap_at(550, 330);          // seek to ~17%
        run_ms(300);
        shot("np03_paused");
        tap(BTN_CROSS);            // play
        tap(BTN_SQUARE);           // shuffle on
        tap(BTN_TRIANGLE); tap(BTN_TRIANGLE); // repeat one
        run_ms(200);
        shot("np04_playing_modes");
        drag(560, 330, 760, 330, 300, 1);
        shot("np05_scrubbing");
        lift();
        touch(698, 416);
        shot("np06_pressed_play");
        lift();
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
