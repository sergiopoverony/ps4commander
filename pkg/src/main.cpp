#include <stdio.h>
#include <signal.h>
#include <sys/stat.h>
#include <orbis/libkernel.h>

#include "log.h"
#include "net.h"
#include "ui.h"
#include "i18n.h"

#ifndef APP_VERSION
#define APP_VERSION "0.0.0.0"
#endif

int main()
{
    signal(SIGPIPE, SIG_IGN);

    mkdir("/data/ps4commander_app", 0777);
    log_reset();
    logf_("---- ps4commander app %s start ----", APP_VERSION);

    net_init();
    i18n_init();
    if (!ui_init()) logf_("ui_init failed: no screen");

    for (;;) {
        ui_pump();
        sceKernelUsleep(30000);
    }
    return 0;
}
