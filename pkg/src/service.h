#pragma once
#include <string>
#include <vector>
#include <stdint.h>

// Служба ps4commander (payload, порт 8484) живёт отдельно от этого приложения:
// приложение только запускает её (отправкой ELF в BinLoader GoldHEN) и говорит с ней по HTTP,
// теми же /api/* маршрутами, что и веб-страница.

extern const char* SERVICE_HOST;   // "127.0.0.1"
extern const int   SERVICE_PORT;   // 8484

// Один элемент списка каталога (как в /api/ls: entries[]).
struct Entry {
    std::string name;
    char type = 'f';        // 'd' папка, 'f' файл, 'l' ссылка, 'o' неизвестно
    int64_t size = 0;
    int64_t mtime = 0;
    std::string perm;       // восьмеричные права, напр. "755"
    std::string link;       // цель ссылки, если type == 'l'
    bool td = false;        // ссылка на папку (в вебе поле "td")
    std::string user, group;   // владелец и группа ("u"/"g")
};
struct LsView {
    bool ok = false;
    std::string path, parent, err;
    std::vector<Entry> entries;
    int64_t freeB = -1, totalB = -1;
    bool danger = false, trunc = false;
};
bool service_ls(const std::string& path, LsView& out);

// Режим "разрешить изменения в системных областях" (флажок в меню веб-версии): добавляет
// "unsafe":true в запросы mkdir/rename/копирования/удаления/chmod.
void service_set_unsafe(bool on);

// /api/roots - список точек монтирования для быстрого перехода; version - версия службы.
struct RootView { std::string path; bool mnt = false, smb = false; };
bool service_roots(std::vector<RootView>& out, std::string& version);

// Задания копирования/переноса/удаления/chmod/mkdir (см. src/jobs.cpp и src/http.cpp службы).
struct JobView {
    bool ok = false;
    long long id = 0;
    std::string op, state, cur;
    long long done = 0, total = 0, filesDone = 0, filesTotal = 0, secs = 0, errTotal = 0;
};
bool service_job_start(const std::string& op, const std::vector<std::string>& paths,
                        const std::string& dest, bool overwrite, bool recursive,
                        const std::string& mode, std::string& err);
bool service_job_state(JobView& out);
bool service_job_cancel();
bool service_mkdir(const std::string& path, std::string& err);
bool service_rename(const std::string& path, const std::string& to, std::string& err);
bool service_chmod(const std::string& path, const std::string& mode, bool recursive, std::string& err);
bool service_delete(const std::vector<std::string>& paths, bool recursive, std::string& err);

// Установка .pkg (BGFT) - см. src/install.cpp службы.
struct PkgItemView { std::string name, path, status, msg; long long total = 0, done = 0; };
struct PkgQueueView { bool active = false; std::vector<PkgItemView> items; };
bool service_pkg_install(const std::vector<std::string>& paths, std::string& err);
bool service_pkg_state(PkgQueueView& out);
bool service_pkg_cancel();

// Состояние (тема, последние пути панелей) - /api/state.
bool service_state_get(std::string& l, std::string& r, std::string& theme);
bool service_state_set_theme(const std::string& theme);
bool service_state_set_paths(const std::string& l, const std::string& r);   // запомнить папки панелей

// Просмотр текстового файла (как /api/view, с ограничением длины - для экрана этого достаточно).
bool service_view(const std::string& path, int64_t maxLen, std::string& data, std::string& err);

// Отправляет ELF из PKG (/app0/assets/misc/ps4commander.elf) в BinLoader (127.0.0.1:9090).
bool service_send_payload(std::string& err, bool* connectFailed = nullptr);

// Устанавливает payload в автозапуск GoldHEN (/data/GoldHEN/payloads.ini, секция [AutoRun]).
bool service_install_autorun(std::string& err);

// POST /quit - просит службу завершиться.
bool service_quit();

// Поиск файлов в папке (как /api/find в вебе).
struct FindHit { std::string path; char type = 'f'; int64_t size = 0; };
struct FindResult { std::vector<FindHit> hits; long long scanned = 0; bool trunc = false; };
bool service_find(const std::string& path, const std::string& q, FindResult& out, std::string& err);

// Журнал службы (/api/log).
bool service_log(int maxBytes, std::string& out);

// Сетевые папки SMB (/api/smb*): список сохранённых подключений, поиск серверов, список шар,
// сохранение и удаление - те же маршруты, что в диалоге SMB веб-версии.
struct SmbConnView { std::string alias, host, share, user, domain; bool hasPass = false; };
bool service_smb_list(std::vector<SmbConnView>& out, std::string& err);
struct SmbScanView { bool running = false; long long done = 0, total = 0; std::vector<std::string> found; };
bool service_smb_scan_start(std::string& err);          // err == "busy" - поиск уже идёт (не ошибка)
bool service_smb_scan_state(SmbScanView& out);
bool service_smb_shares(const SmbConnView& c, const std::string& pass, std::vector<std::string>& shares, std::string& err);
bool service_smb_save(const SmbConnView& c, const std::string& pass, std::string& err);
bool service_smb_del(const std::string& alias, std::string& err);
