#pragma once
#include <string>

struct HttpResult {
    bool connected = false;    // удалось ли вообще подключиться
    int status = 0;            // код ответа (200, 404, ...), 0 если не дошло до ответа
    std::string body;
};

// GET host:port/path (path начинается с '/').
HttpResult http_get(const std::string& host, int port, const std::string& path, int timeout_ms);

// Читает кусок [off, off+len) в dst через "Range: bytes=". Возвращает число прочитанных байт
// (0 - за концом файла), -1 - ошибка. total (если не NULL) - полный размер файла из Content-Range.
long http_read_range(const std::string& host, int port, const std::string& path, unsigned long long off,
                     unsigned len, unsigned char* dst, int timeout_ms, unsigned long long* total = nullptr);

// POST с JSON-телом. Служба ps4commander принимает POST только с заголовком
// X-Requested-With: ps4cmd (см. src/http.cpp службы) - он добавляется здесь.
HttpResult http_post(const std::string& host, int port, const std::string& path,
                     const std::string& body, int timeout_ms);
