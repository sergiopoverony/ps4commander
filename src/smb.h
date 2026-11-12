#pragma once
// Клиент SMB2/3 (libsmb2): сетевые папки как обычные каталоги по путям /smb/<имя>/...
#include <cstdint>
#include <string>
#include <vector>

struct SmbConn {
    std::string alias, host, share, user, pass, domain;
};

struct SmbStat {
    bool dir = false, link = false;
    uint64_t size = 0;
    int64_t mtime = 0;
};

// "/smb" или "/smb/...".
bool smb_is_path(const std::string& norm);
// "/smb/alias/a/b" -> alias="alias", inner="a/b" (для корня шары пусто). false для самого "/smb".
bool smb_split(const std::string& norm, std::string& alias, std::string& inner);
// Одна и та же сетевая шара? (для rename без копирования)
bool smb_same_share(const std::string& a, const std::string& b);

// ---- настройки подключений (хранятся на консоли в smb.json) ----
void smb_load_config();
std::vector<SmbConn> smb_list();
bool smb_get(const std::string& alias, SmbConn& out);
std::string smb_conns_json();  // без паролей
// Проверяет подключение и сохраняет. Пустой pass у существующего имени сохраняет прежний пароль.
// err: bad_alias, bad_host, bad_share, или код ошибки подключения (EACCES, ECONNREFUSED, ETIMEDOUT, ...).
bool smb_save(SmbConn c, std::string& err);
bool smb_remove(const std::string& alias);

// ---- операции; 0 при успехе, иначе положительный errno ----
int smb_stat(const std::string& path, SmbStat& st);
struct SmbEnt { std::string name; SmbStat st; };
int smb_list_dir(const std::string& path, std::vector<SmbEnt>& out);
int smb_mkdir(const std::string& path);
int smb_rmdir(const std::string& path);
int smb_unlink(const std::string& path);
int smb_rename(const std::string& from, const std::string& to);
int smb_space(const std::string& path, uint64_t& total, uint64_t& avail);

struct SmbFile;
int smb_open(const std::string& path, bool write, SmbFile** out);
// Возвращает число байт (0 = конец файла) или -errno.
long smb_read(SmbFile* f, uint64_t off, void* buf, size_t n);
long smb_write(SmbFile* f, uint64_t off, const void* buf, size_t n);
void smb_close(SmbFile* f);

void smb_shutdown();
