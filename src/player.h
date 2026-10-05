#pragma once
#include <stdint.h>
#include "decoder.h"

enum { REPEAT_OFF, REPEAT_ALL, REPEAT_ONE };

typedef struct {
    int has_track;
    int paused;
    int index, count;          // position in play order, queue length
    char path[1024];
    uint64_t pos, total;       // in source frames
    uint32_t rate, bits, channels, out_rate;
    audio_format fmt;
    int shuffle, repeat;
    uint32_t serial;           // changes whenever the current track changes
    int error;                 // last track failed to open
} player_status;

void player_init(void);
// Copies paths. Starts playing queue[start].
void player_set_queue(char **paths, int n, int start);
void player_toggle_pause(void);
void player_next(void);
void player_prev(void);
void player_seek_rel(int seconds);
void player_set_shuffle(int on);
void player_cycle_repeat(void);
void player_set_modes(int shuffle, int repeat); // restore settings
void player_get_status(player_status *s);
