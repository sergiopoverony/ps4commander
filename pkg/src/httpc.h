#pragma once
#include <string>

struct HttpResult {
    bool connected = false;    // удалось ли вообще подключиться
    int status = 0;            // код ответа (200, 404, ...), 0 если не дошло до ответа
    std::string body;
};

// GET host:port/path (path начинается с '/').
HttpResult http_get(const std::string& host, int port, const std::string& path, int timeout_ms);

// POST с JSON-телом. Служба ps4commander принимает POST только с заголовком
// X-Requested-With: ps4cmd (см. src/http.cpp службы) - он добавляется здесь.
HttpResult http_post(const std::string& host, int port, const std::string& path,
                     const std::string& body, int timeout_ms);
