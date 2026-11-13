#include "httpc.h"
#include "net.h"
#include <unistd.h>
#include <sys/socket.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// Служба отвечает HTTP/1.0 и сама закрывает соединение после ответа, поэтому читаем до конца сокета.

static bool send_all(int s, const char* data, size_t len)
{
    size_t sent = 0;
    while (sent < len) {
        int n = send(s, data + sent, len - sent, 0);
        if (n <= 0) return false;
        sent += (size_t)n;
    }
    return true;
}

static HttpResult do_request(const std::string& host, int port, const std::string& req, int timeout_ms)
{
    HttpResult r;
    int s = net_connect(host.c_str(), port, timeout_ms);
    if (s < 0) return r;
    r.connected = true;

    if (!send_all(s, req.data(), req.size())) { close(s); return r; }

    std::string resp;
    char buf[8192];
    for (;;) {
        int n = recv(s, buf, sizeof(buf), 0);
        if (n <= 0) break;
        resp.append(buf, (size_t)n);
        if (resp.size() > 32 * 1024 * 1024) break;   // защита от бесконечного ответа (списки файлов бывают большие)
    }
    close(s);

    size_t hend = resp.find("\r\n\r\n");
    if (hend == std::string::npos) return r;

    std::string head = resp.substr(0, hend);
    r.body = resp.substr(hend + 4);
    size_t sp1 = head.find(' ');
    if (sp1 != std::string::npos) r.status = atoi(head.c_str() + sp1 + 1);
    return r;
}

HttpResult http_get(const std::string& host, int port, const std::string& path, int timeout_ms)
{
    char req[1200];
    snprintf(req, sizeof(req), "GET %s HTTP/1.0\r\nHost: %s\r\nConnection: close\r\n\r\n", path.c_str(), host.c_str());
    return do_request(host, port, req, timeout_ms);
}

HttpResult http_post(const std::string& host, int port, const std::string& path,
                     const std::string& body, int timeout_ms)
{
    char head[1024];
    snprintf(head, sizeof(head),
             "POST %s HTTP/1.0\r\nHost: %s\r\nX-Requested-With: ps4cmd\r\nContent-Type: application/json\r\n"
             "Content-Length: %d\r\nConnection: close\r\n\r\n",
             path.c_str(), host.c_str(), (int)body.size());
    return do_request(host, port, std::string(head) + body, timeout_ms);
}

long http_read_range(const std::string& host, int port, const std::string& path, unsigned long long off,
                     unsigned len, unsigned char* dst, int timeout_ms, unsigned long long* total)
{
    if (len == 0) return 0;
    int s = net_connect(host.c_str(), port, timeout_ms);
    if (s < 0) return -1;
    char req[1400];
    snprintf(req, sizeof(req), "GET %s HTTP/1.0\r\nHost: %s\r\nRange: bytes=%llu-%llu\r\nConnection: close\r\n\r\n",
             path.c_str(), host.c_str(), off, off + len - 1);
    if (!send_all(s, req, strlen(req))) { close(s); return -1; }

    std::string head;
    char buf[4096];
    size_t hend = std::string::npos;
    while (hend == std::string::npos) {
        int n = recv(s, buf, sizeof(buf), 0);
        if (n <= 0) { close(s); return -1; }
        head.append(buf, (size_t)n);
        hend = head.find("\r\n\r\n");
        if (head.size() > 16384 && hend == std::string::npos) { close(s); return -1; }
    }
    int status = 0;
    size_t sp = head.find(' ');
    if (sp != std::string::npos) status = atoi(head.c_str() + sp + 1);
    if (total) {
        size_t cr = head.find("Content-Range: bytes ");
        if (cr != std::string::npos && cr < hend) {
            size_t sl = head.find('/', cr);
            if (sl != std::string::npos && sl < hend) *total = strtoull(head.c_str() + sl + 1, NULL, 10);
        }
    }
    if (status == 416) { close(s); return 0; }
    if (status != 206 && !(status == 200 && off == 0)) { close(s); return -1; }
    size_t got = head.size() - (hend + 4);
    if (got > len) got = len;
    memcpy(dst, head.data() + hend + 4, got);
    while (got < len) {
        int n = recv(s, (char*)dst + got, len - got, 0);
        if (n <= 0) break;
        got += (size_t)n;
    }
    close(s);
    return (long)got;
}
