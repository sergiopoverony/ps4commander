#pragma once
// Потоковый ZIP без сжатия (метод «store»): архив собирается на лету и сразу уходит клиенту,
// на диске ничего не создаётся. Размер известен заранее, поддерживается ZIP64 (файлы и архивы > 4 ГБ).
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

struct ZipEntry {
    std::string rel;     // путь внутри архива (без начального «/»; у каталога заканчивается на «/»)
    std::string abs;     // путь на диске
    bool dir = false;
    uint64_t size = 0;
    uint32_t mode = 0;   // права (rwx)
    long long mtime = 0;
};
struct ZipPlan {
    std::vector<ZipEntry> entries;
    uint64_t total = 0;  // точный размер архива в байтах
    uint64_t skipped = 0;  // пропущено (ссылки, нечитаемые)
};

// Собирает список файлов. base - каталог, names - имена внутри него (без «/»).
// false и err: bad_path, ENOENT, too_many, outside_root, ...
bool zip_plan(const std::string& base, const std::vector<std::string>& names, ZipPlan& plan, std::string& err);
// Пишет архив через sink (возвращает false при обрыве клиента). true, если записан целиком.
// Если файл за время скачивания изменился, он дополняется нулями или обрезается до заранее объявленного размера.
bool zip_write(const ZipPlan& plan, const std::function<bool(const char*, size_t)>& sink);
