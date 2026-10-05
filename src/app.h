#pragma once
#include <stdint.h>
int  app_init(const char *asset_dir);
// One UI tick: handle input, render if needed. Returns 0 to keep running.
int  app_step(uint32_t buttons);
void app_force_redraw(void);
