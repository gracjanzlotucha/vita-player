#include "app.h"
#include "platform.h"
#include <psp2/kernel/processmgr.h>

unsigned int sceUserMainThreadStackSize = 1024 * 1024;

int main(void) {
    if (plat_init() != 0) return 1;
    if (app_init("app0:assets") != 0) { sceKernelExitProcess(0); return 1; }
    for (;;) app_step(plat_buttons());
    return 0;
}
