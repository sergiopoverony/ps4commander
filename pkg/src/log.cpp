#include "log.h"
#include <stdio.h>
#include <stdarg.h>
#include <sys/stat.h>

#ifndef LOG_DIR
#define LOG_DIR "/data/ps4commander_app"
#endif
static const char* LOGPATH = LOG_DIR "/log.txt";

void log_reset() {
    mkdir(LOG_DIR, 0777);
    FILE* f = fopen(LOGPATH, "w");
    if (f) fclose(f);
}

void logf_(const char* fmt, ...) {
    FILE* f = fopen(LOGPATH, "a");
    if (!f) return;
    va_list ap;
    va_start(ap, fmt);
    vfprintf(f, fmt, ap);
    va_end(ap);
    fputc('\n', f);
    fclose(f);
}

const char* log_path() { return LOGPATH; }
