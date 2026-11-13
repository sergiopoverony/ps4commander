#include "httpc.h"
#include "net.h"
#include <unistd.h>
#include <sys/socket.h>
#include <stdio.h>
#include <stdlib.h>

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
