// Сеть для payload на ps4-payload-sdk (FreeBSD-совместимые сокеты).
// Отличия от версии для OpenOrbis: DNS делает системная библиотека SDK (getaddrinfo),
// SO_NBIO и ручная проверка errno == 36 не нужны (EINPROGRESS здесь и есть 36).
#include "net.h"
#include "log.h"
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/select.h>
#include <arpa/inet.h>
#include <netdb.h>

bool net_init() { return true; }   // сеть инициализирует библиотека SDK при первом обращении

bool net_resolve(const char* host, struct in_addr* out) {
    in_addr_t lit = inet_addr(host);
    if (lit != (in_addr_t)-1) { out->s_addr = lit; return true; }

    struct addrinfo hints, *res = NULL;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    int rc = getaddrinfo(host, NULL, &hints, &res);
    if (rc != 0 || !res) { logf_("resolve %s failed rc=%d", host, rc); return false; }
    *out = ((struct sockaddr_in*)res->ai_addr)->sin_addr;
    freeaddrinfo(res);
    return true;
}

static void fill_addr(struct sockaddr_in& a, struct in_addr ip, int port) {
    memset(&a, 0, sizeof(a));
#ifndef __linux__
    a.sin_len = sizeof(a);
#endif
    a.sin_family = AF_INET;
    a.sin_port = htons((unsigned short)port);
    a.sin_addr = ip;
}

int net_connect(const char* host, int port, int timeout_ms) {
    struct in_addr ip;
    if (!net_resolve(host, &ip)) return -1;

    int s = socket(AF_INET, SOCK_STREAM, 0);
    if (s < 0) { logf_("socket failed errno=%d", errno); return -1; }

    int fl = fcntl(s, F_GETFL, 0);
    fcntl(s, F_SETFL, fl | O_NONBLOCK);

    struct sockaddr_in a;
    fill_addr(a, ip, port);

    int r = connect(s, (struct sockaddr*)&a, sizeof(a));
    if (r < 0 && errno != EINPROGRESS) {
        logf_("connect %s:%d failed errno=%d", host, port, errno);
        close(s);
        return -1;
    }
    if (r < 0) {
        fd_set wf;
        FD_ZERO(&wf);
        FD_SET(s, &wf);
        struct timeval tv;
        tv.tv_sec = timeout_ms / 1000;
        tv.tv_usec = (timeout_ms % 1000) * 1000;
        r = select(s + 1, NULL, &wf, NULL, &tv);
        if (r <= 0) {
            logf_("connect %s:%d timeout", host, port);
            close(s);
            return -1;
        }
        int err = 0;
        socklen_t len = sizeof(err);
        getsockopt(s, SOL_SOCKET, SO_ERROR, &err, &len);
        if (err) {
            logf_("connect %s:%d failed so_error=%d", host, port, err);
            close(s);
            return -1;
        }
    }

    fcntl(s, F_SETFL, fl);   // обратно в блокирующий режим
    struct timeval tv;
    tv.tv_sec = timeout_ms / 1000;
    tv.tv_usec = (timeout_ms % 1000) * 1000;
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
    return s;
}

int net_connect_nb(const char* ip, int port) {
    int s = socket(AF_INET, SOCK_STREAM, 0);
    if (s < 0) return -1;

    int fl = fcntl(s, F_GETFL, 0);
    fcntl(s, F_SETFL, fl | O_NONBLOCK);
#ifdef SO_NOSIGPIPE
    int one = 1;
    setsockopt(s, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof(one));
#endif

    struct in_addr addr;
    addr.s_addr = inet_addr(ip);
    struct sockaddr_in a;
    fill_addr(a, addr, port);

    int r = connect(s, (struct sockaddr*)&a, sizeof(a));
    if (r < 0 && errno != EINPROGRESS) { int e = errno; close(s); errno = e; return -1; }
    return s;
}
