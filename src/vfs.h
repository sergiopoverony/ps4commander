#pragma once
// Единый доступ к файлам: локальная файловая система консоли и сетевые папки SMB (/smb/...).
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

struct VStat {
    bool dir = false, reg = false, lnk = false;
    uint64_t size = 0;
    int64_t mtime = 0;
    uint32_t mode = 0;  // права (rwx), для SMB условные
};

// Все функции с кодом возврата: 0 при успехе, иначе положительный errno.
int v_lstat(const std::string& p, VStat& st);
int v_list(const std::string& p, std::vector<std::string>& names);
int v_mkdir(const std::string& p, uint32_t mode);
int v_rmdir(const std::string& p);
int v_unlink(const std::string& p);
// EXDEV, если локальный и сетевой пути (или разные шары): вызывающий копирует и удаляет.
int v_rename(const std::string& a, const std::string& b);
int v_chmod(const std::string& p, uint32_t mode);  // у SMB ничего не делает

struct VFile {
    int fd = -1;
    struct SmbFile* sf = nullptr;
    uint64_t off = 0;
};
int v_open_read(const std::string& p, VFile& f);
int v_open_write(const std::string& p, VFile& f, uint32_t mode);
// Читает/пишет последовательно. Возвращает число байт (0 = конец) или -errno.
long v_read(VFile& f, void* buf, size_t n);
long v_write(VFile& f, const void* buf, size_t n);
long v_pread(VFile& f, uint64_t off, void* buf, size_t n);
void v_close(VFile& f);
