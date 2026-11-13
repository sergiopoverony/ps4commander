#pragma once
// Сетевые папки SMB, смонтированные через FUSE в обычную папку на флешке (для установки .pkg без копирования).
#include <cstdint>
#include <string>
#include <vector>
#include <sys/stat.h>
#include <sys/statvfs.h>

// Операции файловой системы (одинаковые на консоли и на ПК; пути внутри точки монтирования: "/alias/a/b").
// Возвращают 0/число байт или -errno.
int nf_getattr(const std::string& p, struct stat* st);
int nf_readdir(const std::string& p, std::vector<std::string>& names);
struct NfHandle;
int nf_open(const std::string& p, NfHandle** out);
long nf_read(NfHandle* h, char* buf, size_t size, uint64_t off);
void nf_close(NfHandle* h);
int nf_statfs(struct statvfs* sv);

// Монтирует (один раз за запуск) и возвращает путь папки, в которой лежат шары как каталоги.
std::string netfs_fw_str();   // версия прошивки "9.60" (пусто в тестах)
// false: err = no_usb, no_fw, no_fuse, reload, mount (причина пишется в лог).
// usb=false: точка монтирования в /data/pkg, usb=true: на подключённой флешке (no_usb, если её нет).
bool netfs_root(std::string& root, std::string& err, bool usb = false);
// "/smb/alias/a/b.pkg" -> "<root>/alias/a/b.pkg"
std::string netfs_map(const std::string& root, const std::string& smb_path);

// Эта папка - точка монтирования FUSE (в списке дисков её не показываем и не трогаем: у умершего монтирования stat зависает).
bool netfs_is_fuse_path(const std::string& p);

// Установка закончилась: пробует отмонтировать папку FUSE (ядро может отказать - тогда монтирование остаётся и используется снова).
void netfs_idle();
