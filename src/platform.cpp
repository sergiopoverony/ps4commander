#include "platform.h"
#include "log.h"
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <sys/types.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <unistd.h>

#ifndef HOST_TEST
struct notify_request_t {
    int type, reqId, priority, msgId, targetId, userId, unk1, unk2, appId, errorNum, unk3;
    unsigned char useIconImageUri;
    char message[1024];
    char iconImageUri[1024];
    char unk[1024];
};
static_assert(sizeof(notify_request_t) == 3120, "notification request size");
extern "C" int sceKernelSendNotificationRequest(int, notify_request_t*, size_t, int);
extern "C" int sceSystemServicePowerTick(void);
#endif

std::string local_ip() {
    // ВАЖНО: адрес берём только через обычные сокеты/интерфейсы, БЕЗ libSceNetCtl. Версии
    // 1.2.0.10-1.3.2.5 вызывали sceNetCtlGetInfo и линковались с libSceNetCtl.sprx - на части
    // консолей (по отчётам тестеров) payload при этом вообще не доходит до main() ("payload
    // started" и тишина, папки /data/ps4commander нет). Тот же способ, что в ps4torrent.
    std::string found;
#ifndef HOST_TEST
    auto fmt = [](uint32_t be) {
        uint32_t a = ntohl(be);
        char buf[32];
        snprintf(buf, sizeof(buf), "%u.%u.%u.%u", (a >> 24) & 255, (a >> 16) & 255, (a >> 8) & 255, a & 255);
        return std::string(buf);
    };
    // 1) Список интерфейсов: работает и без интернета, лишь бы был адрес в локальной сети.
    struct ifaddrs* ifa = nullptr;
    if (getifaddrs(&ifa) == 0) {
        for (struct ifaddrs* p = ifa; p; p = p->ifa_next) {
            if (!p->ifa_addr || p->ifa_addr->sa_family != AF_INET) continue;
            if (!(p->ifa_flags & IFF_UP) || (p->ifa_flags & IFF_LOOPBACK)) continue;
            uint32_t sa = ((struct sockaddr_in*)p->ifa_addr)->sin_addr.s_addr;
            uint32_t h = ntohl(sa);
            if (h == 0 || (h >> 24) == 127 || (h >> 16) == 0xA9FE) continue;   // пусто, loopback, link-local
            found = fmt(sa);
            break;
        }
        freeifaddrs(ifa);
    }
    if (!found.empty()) return found;
    // 2) Запасной способ: UDP «подключение» к внешнему адресу ничего не отправляет, но выбирает
    //    маршрут и адрес.
    int s = socket(AF_INET, SOCK_DGRAM, 0);
    if (s >= 0) {
        struct sockaddr_in to;
        memset(&to, 0, sizeof(to));
        to.sin_family = AF_INET;
        to.sin_port = htons(53);
        to.sin_addr.s_addr = htonl(0x08080808);  // 8.8.8.8
        if (connect(s, (struct sockaddr*)&to, sizeof(to)) == 0) {
            struct sockaddr_in me;
            socklen_t l = sizeof(me);
            memset(&me, 0, sizeof(me));
            if (getsockname(s, (struct sockaddr*)&me, &l) == 0) {
                uint32_t h = ntohl(me.sin_addr.s_addr);
                if (h != 0 && (h >> 24) != 127) found = fmt(me.sin_addr.s_addr);
            }
        }
        close(s);
    }
#endif
    return found;
}

bool local_subnet(unsigned& ip, unsigned& mask) {
#ifdef HOST_TEST
    ip = 0x7F000001; mask = 0xFFFFFFFF;   // в тестах «сеть» - один адрес 127.0.0.1
    return true;
#else
    bool ok = false;
    struct ifaddrs* ifa = nullptr;
    if (getifaddrs(&ifa) == 0) {
        for (struct ifaddrs* p = ifa; p && !ok; p = p->ifa_next) {
            if (!p->ifa_addr || !p->ifa_netmask || p->ifa_addr->sa_family != AF_INET) continue;
            if (!(p->ifa_flags & IFF_UP) || (p->ifa_flags & IFF_LOOPBACK)) continue;
            uint32_t a = ntohl(((struct sockaddr_in*)p->ifa_addr)->sin_addr.s_addr);
            uint32_t m = ntohl(((struct sockaddr_in*)p->ifa_netmask)->sin_addr.s_addr);
            if (a == 0 || (a >> 24) == 127 || (a >> 16) == 0xA9FE || m == 0) continue;
            ip = a; mask = m; ok = true;
        }
        freeifaddrs(ifa);
    }
    return ok;
#endif
}

void notify(const char* fmt, ...) {
    char buf[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
#ifdef HOST_TEST
    printf("[notify] %s\n", buf);
#else
    notify_request_t req;
    memset(&req, 0, sizeof(req));
    snprintf(req.message, sizeof(req.message), "%s", buf);
    sceKernelSendNotificationRequest(0, &req, sizeof(req), 0);
#endif
}

static int g_ticks = 0;
#ifndef HOST_TEST
static bool g_tick_err = false;
#endif
int host_test_ticks() { return g_ticks; }

void keep_awake_tick() {
    ++g_ticks;
#ifndef HOST_TEST
    int r = sceSystemServicePowerTick();
    if (g_ticks == 1) app_log("защита от сна: sceSystemServicePowerTick вернул %d (0 = принято)", r);
    else if (r != 0 && !g_tick_err) { g_tick_err = true; app_log("защита от сна: sceSystemServicePowerTick вернул %d", r); }
#endif
}
