#include "log.h"
#include "version.h"
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <mutex>
#include <sys/stat.h>

static std::mutex g_mx;
static FILE* g_file = nullptr;
static std::string g_ring;
static const size_t kRingMax = 64 * 1024;
static const long kFileMax = 512 * 1024;
static time_t g_t0 = 0;

void log_open(const char* dir) {
    std::lock_guard<std::mutex> l(g_mx);
    g_t0 = time(nullptr);
    mkdir(dir, 0777);
    std::string path = std::string(dir) + "/log.txt";
    struct stat st;
    if (stat(path.c_str(), &st) == 0 && st.st_size > kFileMax) {
        std::string old = std::string(dir) + "/log.old.txt";
        rename(path.c_str(), old.c_str());
    }
    g_file = fopen(path.c_str(), "a");
}

void app_log(const char* fmt, ...) {
    char msg[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof(msg), fmt, ap);
    va_end(ap);
    char line[1100];
    snprintf(line, sizeof(line), "[%5ld] %s\n", (long)(time(nullptr) - g_t0), msg);
    std::lock_guard<std::mutex> l(g_mx);
    fputs(line, stdout);
    fflush(stdout);
    if (g_file) {
        fputs(line, g_file);
        fflush(g_file);
    }
    g_ring += line;
    if (g_ring.size() > kRingMax) {
        size_t cut = g_ring.size() - kRingMax / 2;
        size_t nl = g_ring.find('\n', cut);
        g_ring.erase(0, nl == std::string::npos ? cut : nl + 1);
    }
}

std::string log_tail(size_t max_bytes) {
    std::lock_guard<std::mutex> l(g_mx);
    if (g_ring.size() <= max_bytes) return g_ring;
    std::string t = g_ring.substr(g_ring.size() - max_bytes);
    size_t nl = t.find('\n');
    return nl == std::string::npos ? t : t.substr(nl + 1);
}
