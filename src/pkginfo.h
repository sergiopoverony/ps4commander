#pragma once
// Разбор заголовка PKG и подбор связанных пакетов (патчи, DLC, русификаторы) по правилам из ps4installer.
#include <cstdint>
#include <string>
#include <vector>

struct PkgInfo {
    std::string path;
    uint64_t file_size = 0, pkg_size = 0;
    std::string content_id, title_id;   // UP0006-CUSA15080_00-NAME, CUSA15080
    uint32_t content_type = 0, flags = 0;
    std::string kind;                   // game | patch | dlc | dlc_nodata | delta_patch
    bool is_patch = false;
    std::string title, app_ver;         // из param.sfo внутри PKG
    int ver_num = -1;                   // 01.04 -> 104
    bool backport = false, rus = false, dlc_name = false;   // признаки по имени файла и папки
    bool incomplete() const { return pkg_size > 0 && file_size < pkg_size; }
};

struct PkgSkip { PkgInfo pkg; std::string reason; };

// Читает заголовок и param.sfo через VFS (локальный диск и SMB). false: err = not_pkg или код ошибки.
bool pi_read(const std::string& path, uint64_t file_size, PkgInfo& out, std::string& err);
// Разбор уже прочитанных первых байт (для проверок).
bool pi_parse_header(const uint8_t* buf, size_t len, PkgInfo* out, std::string* err);
void pi_name_tags(PkgInfo* p);
// Что и в каком порядке ставить (правила ps4installer): backport не ставится при прошивке выше 9, из обычных патчей остаётся
// самый новый; порядок внутри игры: игра, патчи (по возрастанию версии), DLC, русификаторы.
std::vector<PkgInfo> pi_plan(const std::vector<PkgInfo>& in, int fw_major, std::vector<PkgSkip>* skipped);
int pi_fw_major();
// Ищет рядом с пакетом (его папка и подпапки на один уровень) пакеты той же игры. JSON для окна выбора; пусто и err при ошибке.
std::string pi_related_json(const std::string& norm_path, std::string& err);
