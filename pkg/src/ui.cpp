// Интерфейс PKG-приложения: две панели, нижняя строка F1-F10, меню - всё как в веб-версии,
// но под геймпад. Все обращения к службе (HTTP) идут в фоновых потоках (async.h): главный поток
// только читает геймпад и рисует, поэтому интерфейс не "подвисает" на медленных папках/сети.
#include "ui.h"
#include "service.h"
#include "gfx.h"
#include "async.h"
#include "image.h"
#include "media.h"
#include "video.h"
#include "i18n.h"
#include "log.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <time.h>
#include <algorithm>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include <orbis/Pad.h>
#include <orbis/UserService.h>
#include <orbis/ImeDialog.h>
#include <orbis/CommonDialog.h>
#include <orbis/Sysmodule.h>

extern "C" uint64_t sceKernelGetProcessTime(void);
extern "C" int sceSystemServiceHideSplashScreen(void);

#ifndef APP_VERSION
#define APP_VERSION "0.0.0.0"
#endif

// ---------------------------------------------------------------- геометрия (экран 1920x1080)

static const int MARGIN_X  = 36;
static const int TOP_BASE  = 38;          // базовая линия верхней строки
static const int PANEL_TOP = 56;
static const int PANEL_H   = 866;
static const int HEAD_H    = 34;          // заголовок панели (путь)
static const int COLH_H    = 28;          // шапка столбцов (Name/Size/Date/Attr)
static const int FOOT_H    = 34;          // подвал панели (сведения/свободное место)
static const int ROW_H     = 28;
static const int NAME_Y    = PANEL_TOP + PANEL_H + 31;    // посередине между панелями и нижним меню    // строка с именем выбранного файла/папки (как #msg в вебе)
static const int BAR_TOP   = PANEL_TOP + PANEL_H + 50;
static const int BAR_H     = 42;
static const int HINT_Y    = 1038;        // подсказка по кнопкам - одной строкой
static const int CREDIT_Y  = 1068;                        // версия и авторы - у нижнего края экрана

static int g_redraw = 2;                  // сколько кадров ещё рисовать (2 - оба буфера экрана)
static bool g_ready = false;

static void redraw() { g_redraw = 2; }

// ---------------------------------------------------------------- вспомогательное

static std::string tr_sub(const char* key, const char* def, std::initializer_list<std::pair<const char*, std::string>> subs)
{
    std::string s = tr(key, def);
    for (auto& kv : subs) {
        std::string tok = std::string("{") + kv.first + "}";
        size_t pos;
        while ((pos = s.find(tok)) != std::string::npos) s.replace(pos, tok.size(), kv.second);
    }
    return s;
}

// Как errText() в веб-версии: e_<код> из переводов; коды вида EACCES - как есть; иначе "Error: код".
static std::string tr_err(const std::string& code)
{
    std::string k = "e_" + code;
    std::string v = tr(k.c_str(), "\x01");
    if (v != "\x01") return v;
    bool errno_like = code.size() > 1 && code[0] == 'E';
    for (char c : code) if (!((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9'))) errno_like = false;
    if (errno_like) return code;
    return tr("e_generic", "Error") + (code.empty() ? "" : ": " + code);
}

// Как fmtSize() в вебе.
static std::string fmt_size(int64_t n)
{
    char b[48];
    if (n < 1024) { snprintf(b, sizeof(b), "%lld B", (long long)n); return b; }
    const char* u[] = { "KB", "MB", "GB", "TB" };
    double v = (double)n; int i = -1;
    do { v /= 1024.0; i++; } while (v >= 1024.0 && i < 3);
    snprintf(b, sizeof(b), v >= 100 ? "%.0f %s" : v >= 10 ? "%.1f %s" : "%.2f %s", v, u[i]);
    return b;
}

// Как fmtDate() в вебе: "YYYY-MM-DD HH:MM".
static std::string fmt_date(int64_t sec)
{
    if (sec <= 0) return "";
    time_t t = (time_t)sec;
    struct tm tmv;
    if (!gmtime_r(&t, &tmv)) return "";
    char b[32];
    snprintf(b, sizeof(b), "%04d-%02d-%02d %02d:%02d", tmv.tm_year + 1900, tmv.tm_mon + 1, tmv.tm_mday, tmv.tm_hour, tmv.tm_min);
    return b;
}

static std::string parent_of(const std::string& p)
{
    size_t i = p.find_last_of('/');
    return (i == std::string::npos || i == 0) ? "/" : p.substr(0, i);
}
static std::string base_of(const std::string& p)
{
    size_t i = p.find_last_of('/');
    return i == std::string::npos ? p : p.substr(i + 1);
}
static std::string join_path(const std::string& dir, const std::string& name)
{
    if (dir == "/") return "/" + name;
    return dir + "/" + name;
}

static std::string lower(std::string s) { for (auto& c : s) if (c >= 'A' && c <= 'Z') c = (char)(c + 32); return s; }

// ---------------------------------------------------------------- модель панели

struct Entry2 {
    std::string name, perm, link, user, group;
    char type = 'f';
    bool td = false;                 // ссылка на папку
    int64_t size = 0, mtime = 0;
    bool isDir() const { return type == 'd' || td; }
};

struct Panel {
    std::string path;
    std::vector<Entry2> items;       // первый элемент - ".." (если путь не корень)
    int cur = 0, scroll = 0;
    std::vector<std::string> sel;    // отмеченные имена
    bool hasUp = false;
    bool danger = false;
    std::string err;
    int64_t freeB = -1, totalB = -1;
    volatile int reqId = 0;          // номер последнего запроса: устаревшие ответы отбрасываются
    bool loading = false;
};

static Panel g_panels[2];
static int g_active = 0;
static bool g_unsafe = false;        // "разрешить изменения в системных областях"
static int  g_busy = 0;              // сколько фоновых действий выполняется (для индикатора)
static int  g_userId = 0;            // пользователь PS4 (для звука и системной клавиатуры)
static bool g_stateReady = false;
static uint64_t g_stateAt = 0;       // когда сохранить пути панелей в службе (0 - не надо)

static Entry2* panel_cur(Panel& p) { return (p.cur >= 0 && p.cur < (int)p.items.size()) ? &p.items[p.cur] : NULL; }
static bool is_marked(const Panel& p, const std::string& n) { return std::find(p.sel.begin(), p.sel.end(), n) != p.sel.end(); }
static bool is_pkg(const Entry2& e) { std::string n = lower(e.name); return e.type == 'f' && n.size() > 4 && n.compare(n.size() - 4, 4, ".pkg") == 0; }

static uint64_t now_us() { return sceKernelGetProcessTime(); }

static void panel_load(int pi, const std::string& path, const std::string& keepName)
{
    Panel& p0 = g_panels[pi];
    int id = ++p0.reqId;
    p0.loading = true;
    redraw();
    auto v = std::make_shared<LsView>();
    auto ok = std::make_shared<bool>(false);
    async_run(pi == 0 ? ASYNC_LS0 : ASYNC_LS1,
        [pi, id, path, v, ok]() { if (g_panels[pi].reqId != id) return; *ok = service_ls(path, *v); },
        [pi, id, keepName, v, ok]() {
            Panel& p = g_panels[pi];
            if (p.reqId != id) return;
            p.loading = false;
            redraw();
            if (!*ok) { p.err = v->err.empty() ? "error" : v->err; return; }
            p.err.clear();
            p.sel.clear();
            p.path = v->path;
            p.danger = v->danger;
            p.freeB = v->freeB; p.totalB = v->totalB;
            p.items.clear();
            p.hasUp = p.path != "/";
            if (p.hasUp) { Entry2 up; up.name = ".."; up.type = 'd'; p.items.push_back(up); }
            std::vector<Entry2> list;
            for (auto& e : v->entries) {
                Entry2 x;
                x.name = e.name; x.type = e.type; x.size = e.size; x.mtime = e.mtime; x.perm = e.perm;
                x.link = e.link; x.td = e.td; x.user = e.user; x.group = e.group;
                list.push_back(x);
            }
            std::sort(list.begin(), list.end(), [](const Entry2& a, const Entry2& b) {
                if (a.isDir() != b.isDir()) return a.isDir();
                return lower(a.name) < lower(b.name);
            });
            for (auto& x : list) p.items.push_back(x);
            p.cur = 0;
            if (!keepName.empty()) for (size_t i = 0; i < p.items.size(); i++) if (p.items[i].name == keepName) { p.cur = (int)i; break; }
            p.scroll = 0;
            if (g_stateReady) g_stateAt = now_us() + 800000ULL;
        });
}

static void panel_refresh(int pi)
{
    Panel& p = g_panels[pi];
    std::string keep = panel_cur(p) ? panel_cur(p)->name : "";
    if (keep == "..") keep.clear();
    panel_load(pi, p.path, keep);
}
static void refresh_all() { panel_refresh(0); panel_refresh(1); }

static void panel_up(int pi)
{
    Panel& p = g_panels[pi];
    if (p.hasUp) panel_load(pi, parent_of(p.path), base_of(p.path));
}

static void open_file(int pi, bool viaCross);

static void panel_enter(int pi)
{
    Panel& p = g_panels[pi];
    Entry2* e = panel_cur(p);
    if (!e) return;
    if (e->name == "..") { panel_up(pi); return; }
    if (e->isDir()) panel_load(pi, join_path(p.path, e->name), "");
    else open_file(pi, true);      // Крест на файле - как щелчок в браузере
}

// ---------------------------------------------------------------- состояние интерфейса

enum Svc { S_CHECK, S_STARTING, S_READY, S_FAILED };
static Svc g_svc = S_CHECK;
static std::string g_svcErr;
static volatile int g_svcStage = 0;       // 0 - ищем службу, 1 - запускаем через BinLoader

enum Focus { F_IMAGE, F_AUDIO, F_VIDEO, F_PANEL, F_BAR, F_MENU, F_CTX, F_CONFIRM, F_TEXT, F_PROGRESS, F_PKGQ, F_DONATE, F_PICK, F_SMBLIST, F_SMBEDIT };
static Focus g_focus = F_PANEL;
static std::vector<Focus> g_fstack;
static void focus_push(Focus f) { g_fstack.push_back(g_focus); g_focus = f; redraw(); }
static void focus_pop() { if (g_fstack.empty()) g_focus = F_PANEL; else { g_focus = g_fstack.back(); g_fstack.pop_back(); } redraw(); }
static void focus_reset() { g_fstack.clear(); g_focus = F_PANEL; redraw(); }

static int g_barCursor = 0;
static int g_menuCursor = 0, g_ctxCursor = 0;

// --- подтверждение
struct Confirm { std::string title, body, yes; std::function<void()> onYes; };
static Confirm g_cf;
static void confirm_open(const std::string& title, const std::string& body, const std::string& yes, std::function<void()> fn)
{
    g_cf.title = title; g_cf.body = body; g_cf.yes = yes; g_cf.onYes = fn;
    focus_push(F_CONFIRM);
}

// --- окно с текстом (сообщения, свойства, помощь, журнал, просмотр файла)
struct TextView { std::string title; std::vector<std::string> lines; int scroll = 0; bool error = false; };
static TextView g_tv;
static void text_open(const std::string& title, const std::string& body, bool error = false)
{
    g_tv.title = title; g_tv.error = error; g_tv.scroll = 0;
    g_tv.lines = wrap_lines(body, 110);
    focus_push(F_TEXT);
}
static void error_open(const std::string& code) { text_open(tr("e_generic", "Error"), tr_err(code), true); }

// --- выбор из списка (корни, результаты поиска, серверы и общие папки SMB)
struct Pick { std::string title, note; std::vector<std::string> items; int cur = 0, scroll = 0; std::function<void(int)> onPick; };
static Pick g_pick;
static void pick_open(const std::string& title, const std::string& note, const std::vector<std::string>& items, std::function<void(int)> fn, int cur = 0)
{
    g_pick.title = title; g_pick.note = note; g_pick.items = items; g_pick.onPick = fn; g_pick.cur = cur; g_pick.scroll = 0;
    focus_push(F_PICK);
}

// --- прогресс заданий копирования/удаления и очередь установки PKG (опрашиваются в фоне)
static JobView g_job;
static PkgQueueView g_pkgq;
static bool g_pollBusy = false;
static std::vector<std::string> g_pkgActive;   // пути PKG, которые сейчас в очереди/устанавливаются
static bool g_pkgBg = false;          // в службе идёт установка PKG (пункт "очередь установки" в меню Треугольника)
static uint64_t g_pollAt = 0, g_progStart = 0;

static Image g_qrSbp, g_qrPaypal;
static bool g_qrSbpOk = false, g_qrPaypalOk = false, g_qrTried = false;

// --- SMB
struct SmbForm {
    SmbConnView c;
    std::string pass;
    bool editing = false;            // правка сохранённого подключения (имя менять нельзя)
    bool hasPass = false;
    std::vector<std::string> shares;
    std::string status;
    bool statusErr = false;
    int cur = 0;
    bool scanning = false;
};
static SmbForm g_sf;
static std::vector<SmbConnView> g_smbList;
static int g_smbCur = 0, g_smbScroll = 0;
static bool g_smbPollBusy = false;
static uint64_t g_smbNextPoll = 0;

// ---------------------------------------------------------------- действия (всё через фон)

// Выполняет fn в фоне; по успеху вызывает onOk в главном потоке, иначе показывает ошибку.
static void act_async(std::function<bool(std::string&)> fn, std::function<void()> onOk)
{
    auto err = std::make_shared<std::string>();
    auto ok = std::make_shared<bool>(false);
    g_busy++;
    redraw();
    async_run(ASYNC_ACT, [fn, err, ok]() { *ok = fn(*err); },
        [err, ok, onOk]() { g_busy--; redraw(); if (*ok) { if (onOk) onOk(); } else error_open(*err); });
}

static bool ime_text(const std::string& title, const std::string& initial, std::string& out);

static std::vector<std::string> targets(Panel& p, bool needFiles = false)
{
    std::vector<std::string> v;
    if (!p.sel.empty()) { for (auto& n : p.sel) v.push_back(n); }
    else { Entry2* e = panel_cur(p); if (e && e->name != "..") v.push_back(e->name); }
    (void)needFiles;
    return v;
}

static void job_progress_open()
{
    g_job = JobView();
    g_progStart = now_us();
    g_pollAt = 0;
    focus_push(F_PROGRESS);
}

static void start_job(const std::string& op, std::vector<std::string> paths, const std::string& dest, bool overwrite, const std::string& mode)
{
    act_async([=](std::string& err) { return service_job_start(op, paths, dest, overwrite, op != "chmod", mode, err); },
              [=]() { job_progress_open(); });
}

static void act_copy_move(bool move)
{
    Panel& src = g_panels[g_active];
    Panel& dst = g_panels[1 - g_active];
    std::vector<std::string> names = targets(src), paths;
    if (names.empty()) return;
    for (auto& n : names) paths.push_back(join_path(src.path, n));
    std::string dest = dst.path;
    std::string verb = move ? tr("move", "Move") : tr("copy", "Copy");
    std::string title = verb + " " + std::to_string(paths.size()) + " " + tr("items", "items") + " -> " + dest + "?";
    std::string op = move ? "move" : "copy";
    confirm_open(title, tr("overwrite", "Overwrite existing files"), verb, [=]() { start_job(op, paths, dest, true, ""); });
}

static void act_delete()
{
    Panel& p = g_panels[g_active];
    std::vector<std::string> names = targets(p), paths;
    if (names.empty()) return;
    for (auto& n : names) paths.push_back(join_path(p.path, n));
    std::string yes = tr("del", "Delete");
    std::string what = paths.size() == 1 ? "\"" + base_of(paths[0]) + "\"" : std::to_string(paths.size()) + " " + tr("items", "items");
    confirm_open(yes + " " + what + "?", tr("delWarn", "This cannot be undone."), yes,
                 [=]() { start_job("delete", paths, "", false, ""); });
}

static void act_mkdir()
{
    std::string name;
    if (!ime_text(tr("newFolder", "New folder name:"), "", name) || name.empty()) return;
    int pi = g_active;
    std::string full = join_path(g_panels[pi].path, name);
    act_async([=](std::string& err) { return service_mkdir(full, err); }, [=]() { panel_refresh(pi); });
}

static void act_chmod()
{
    Entry2* e = panel_cur(g_panels[g_active]);
    if (!e || e->name == "..") return;
    std::string mode;
    if (!ime_text(tr("modeOct", "Permissions (octal, e.g. 755):"), e->perm, mode) || mode.empty()) return;
    std::vector<std::string> paths; paths.push_back(join_path(g_panels[g_active].path, e->name));
    start_job("chmod", paths, "", false, mode);
}

static void act_rename()
{
    int pi = g_active;
    Panel& p = g_panels[pi];
    Entry2* e = panel_cur(p);
    if (!e || e->name == "..") return;
    std::string name;
    if (!ime_text(tr("rename", "Rename"), e->name, name) || name.empty() || name == e->name) return;
    std::string from = join_path(p.path, e->name), to = join_path(p.path, name);
    act_async([=](std::string& err) { return service_rename(from, to, err); }, [=]() { panel_refresh(pi); });
}

static void view_text(const std::string& path, const std::string& name)
{
    auto data = std::make_shared<std::string>();
    auto err = std::make_shared<std::string>();
    auto ok = std::make_shared<bool>(false);
    g_busy++; redraw();
    async_run(ASYNC_ACT, [path, data, err, ok]() { *ok = service_view(path, 400000, *data, *err); },
        [name, data, err, ok]() {
            g_busy--; redraw();
            if (!*ok) { error_open(*err); return; }
            // двоичный файл (есть нулевые байты в начале) - показываем как hex
            bool bin = false;
            for (size_t i = 0; i < data->size() && i < 512; i++) if ((*data)[i] == 0) { bin = true; break; }
            std::string txt;
            if (bin) {
                char b[16];
                for (size_t off = 0; off < data->size() && off < 16384; off += 16) {
                    snprintf(b, sizeof(b), "%06zx  ", off); txt += b;
                    std::string asc;
                    for (size_t i = 0; i < 16; i++) {
                        if (off + i < data->size()) { unsigned char c = (unsigned char)(*data)[off + i]; snprintf(b, sizeof(b), "%02x ", c); txt += b; asc += (c >= 32 && c < 127) ? (char)c : '.'; }
                        else txt += "   ";
                    }
                    txt += " " + asc + "\n";
                }
            } else {
                for (char c : *data) if (c != '\r' && c != '\t') txt += (c < 32 && c != '\n') ? '.' : c; else if (c == '\t') txt += "    ";
            }
            text_open(name, txt);
        });
}

static void act_edit() { text_open(tr("edit", "Edit"), tr("editWeb", "Full editing is available on the web interface (port 8484)."), false); }

static void act_props()
{
    Panel& p = g_panels[g_active];
    Entry2* e = panel_cur(p);
    if (!e || e->name == "..") return;
    const char* typeKey = e->type == 'd' ? "typeDir" : e->type == 'l' ? "typeLink" : e->type == 'f' ? "typeFile" : "typeOther";
    std::string msg = tr("fPath", "Path") + ": " + join_path(p.path, e->name) +
                      "\n" + tr("fType", "Type") + ": " + tr(typeKey, "File");
    if (e->type == 'f') msg += "\n" + tr("fSize", "Size") + ": " + fmt_size(e->size) + " (" + std::to_string((long long)e->size) + " B)";
    msg += "\n" + tr("fMode", "Permissions") + ": " + e->perm;
    if (!e->user.empty()) msg += "\n" + tr("fOwner", "Owner") + ": " + e->user + ":" + e->group;
    std::string dt = fmt_date(e->mtime);
    if (!dt.empty()) msg += "\n" + tr("fTime", "Modified") + ": " + dt;
    if (!e->link.empty()) msg += "\n" + tr("fTarget", "Link target") + ": " + e->link;
    text_open(tr("infoTitle", "Properties"), msg);
}

static void pkgq_open()
{
    g_pkgq = PkgQueueView();
    g_pkgBg = true;
    g_pollAt = 0;
    focus_push(F_PKGQ);
}

static bool pkg_is_active(const std::string& path) { return std::find(g_pkgActive.begin(), g_pkgActive.end(), path) != g_pkgActive.end(); }
static void pkg_note_active(const PkgQueueView& q)
{
    g_pkgActive.clear();
    for (auto& it : q.items) if (it.status == "queued" || it.status == "registering" || it.status == "installing") g_pkgActive.push_back(it.path);
}

static void install_confirm(const std::vector<std::string>& paths, const std::string& dir)
{
    // этот PKG уже ставится - вместо повторной установки показываем окно очереди с прогрессом
    if (paths.size() == 1 && pkg_is_active(paths[0])) { pkgq_open(); return; }
    std::string nm = paths.size() == 1 ? "\"" + base_of(paths[0]) + "\"" : std::to_string(paths.size()) + " " + tr("items", "items");
    std::string body = dir.compare(0, 5, "/smb/") == 0 ? tr("installSmbNote", "") : "";
    confirm_open(tr_sub("installQ", "Install {n}?", { { "n", nm } }), body, tr("install", "Install PKG"),
                 [paths]() { act_async([paths](std::string& err) { return service_pkg_install(paths, err); }, [paths]() { for (auto& p : paths) g_pkgActive.push_back(p); pkgq_open(); }); });
}

static void act_install()
{
    Panel& p = g_panels[g_active];
    std::vector<std::string> names = targets(p), paths;
    for (auto& n : names) {
        for (auto& e : p.items) if (e.name == n && is_pkg(e)) paths.push_back(join_path(p.path, n));
    }
    if (paths.empty()) { text_open(tr("install", "Install PKG"), tr("pkgNone", "No .pkg files selected"), true); return; }
    install_confirm(paths, p.path);
}

// ---------------------------------------------------------------- открытие файлов по типу (картинки, музыка, текст)

static std::string ext_of(const std::string& n)
{
    size_t i = n.find_last_of('.');
    return i == std::string::npos || i + 1 >= n.size() ? "" : lower(n.substr(i + 1));
}
static bool in_list(const std::string& e, std::initializer_list<const char*> l) { for (auto s : l) if (e == s) return true; return false; }
static bool is_image_ext(const std::string& e) { return in_list(e, { "png", "jpg", "jpeg", "bmp", "gif" }); }
static bool is_audio_ext(const std::string& e) { return in_list(e, { "mp3", "ogg", "wav" }); }
static bool is_av_ext(const std::string& e) { return in_list(e, { "mp4", "m4v", "mov", "3gp", "m4a", "aac" }); }   // системный плеер консоли
static bool is_audio_unsupported(const std::string& e) { return in_list(e, { "flac", "wma", "opus", "mid", "midi", "ac3", "at9" }); }
static bool is_video_ext(const std::string& e) { return in_list(e, { "mp4", "mkv", "webm", "avi", "mov", "m4v", "ts", "wmv", "flv", "mpg", "mpeg", "3gp" }); }
static const int64_t MEDIA_MAX = 32LL * 1024 * 1024 - 4096;    // службу читаем целиком в память, потолок ответа 32 МБ

// --- просмотр картинок
struct ImgView {
    Picture pic;
    bool ok = false, loading = false;
    std::string name;
    std::vector<std::string> list;
    int idx = 0;
    int req = 0;
    int origW = 0;
};
static ImgView g_iv;

static void img_load(int idx)
{
    if (idx < 0 || idx >= (int)g_iv.list.size()) return;
    g_iv.idx = idx; g_iv.loading = true; g_iv.ok = false;
    std::string path = g_iv.list[idx];
    g_iv.name = base_of(path);
    int id = ++g_iv.req;
    redraw();
    auto pic = std::make_shared<Picture>();
    auto ok = std::make_shared<bool>(false);
    async_run(ASYNC_ACT,
        [id, path, pic, ok]() {
            if (g_iv.req != id) return;
            std::string data, err;
            if (!service_download(path, data, err)) return;
            if (g_iv.req != id) return;
            *ok = picture_decode(data, FRAME_WIDTH, 1000, *pic);
        },
        [id, pic, ok]() {
            if (g_iv.req != id) return;
            g_iv.loading = false;
            g_iv.ok = *ok;
            if (*ok) g_iv.pic = std::move(*pic);
            redraw();
        });
}

static void img_open(int pi)
{
    Panel& p = g_panels[pi];
    g_iv.list.clear();
    int start = 0;
    for (size_t i = 0; i < p.items.size(); i++) {
        const Entry2& e = p.items[i];
        if (e.name == ".." || e.isDir() || !is_image_ext(ext_of(e.name))) continue;
        if ((int)i == p.cur) start = (int)g_iv.list.size();
        g_iv.list.push_back(join_path(p.path, e.name));
    }
    if (g_iv.list.empty()) return;
    g_iv.pic = Picture();
    focus_push(F_IMAGE);
    img_load(start);
}

// --- музыка
struct AudioUi { std::string name; };
static AudioUi g_au;

static void audio_open(const std::string& path, const std::string& name, const std::string& ext)
{
    auto data = std::make_shared<std::string>();
    auto err = std::make_shared<std::string>();
    auto ok = std::make_shared<bool>(false);
    g_busy++; redraw();
    async_run(ASYNC_ACT, [path, data, err, ok]() { *ok = service_download(path, *data, *err); },
        [name, ext, data, err, ok]() {
            g_busy--; redraw();
            if (!*ok) { error_open(*err); return; }
            if (!audio_start(*data, ext, g_userId)) { text_open(tr("audioTitle", "Player"), tr("audioErr", "Cannot play this file"), true); return; }
            g_au.name = name;
            focus_push(F_AUDIO);
        });
}

static void open_file(int pi, bool viaCross)
{
    Panel& p = g_panels[pi];
    Entry2* e = panel_cur(p);
    if (!e || e->name == ".." || e->isDir()) return;
    std::string path = join_path(p.path, e->name), name = e->name, ext = ext_of(name);
    if (viaCross && ext == "pkg" && e->type != 'o') { install_confirm({ path }, p.path); return; }
    bool media = is_image_ext(ext) || is_audio_ext(ext);
    if (media && e->type == 'f' && e->size > MEDIA_MAX) { text_open(name, tr("appTooBig", "File is too large for the app (limit 32 MB)"), true); return; }
    if (is_image_ext(ext)) { img_open(pi); return; }
    if (is_audio_ext(ext)) { audio_open(path, name, ext); return; }
    if (is_av_ext(ext)) {
        if (!video_open(path, g_userId)) { text_open(name, tr("audioErr", "Cannot play this file"), true); return; }
        g_au.name = name;
        focus_push(F_VIDEO);
        return;
    }
    if (is_video_ext(ext)) { text_open(name, tr("noVideo", "Video playback is not supported in the app. Open the web interface (port 8484) in a browser."), true); return; }
    if (is_audio_unsupported(ext)) { text_open(name, tr("noFormat", "This audio format is not supported in the app (MP3, OGG, WAV only)."), true); return; }
    view_text(path, name);
}

static void act_view()
{
    Panel& p = g_panels[g_active];
    Entry2* e = panel_cur(p);
    if (!e || e->name == ".." || e->isDir()) return;
    open_file(g_active, false);
}

static void act_find()
{
    std::string q;
    if (!ime_text(tr("find", "Find"), "", q) || q.empty()) return;
    int pi = g_active;
    std::string base = g_panels[pi].path;
    auto res = std::make_shared<FindResult>();
    auto err = std::make_shared<std::string>();
    auto ok = std::make_shared<bool>(false);
    g_busy++; redraw();
    async_run(ASYNC_ACT, [base, q, res, err, ok]() { *ok = service_find(base, q, *res, *err); },
        [pi, res, err, ok]() {
            g_busy--; redraw();
            if (!*ok) { error_open(*err); return; }
            if (res->hits.empty()) { text_open(tr("find", "Find"), tr("noResults", "Nothing found")); return; }
            std::vector<std::string> items;
            for (auto& h : res->hits) items.push_back((h.type == 'd' ? "/" : "") + h.path + (h.type == 'f' ? "  (" + fmt_size(h.size) + ")" : ""));
            std::string note = tr_sub("scannedN", "checked {n} items", { { "n", std::to_string(res->scanned) } }) + (res->trunc ? ". " + tr("truncNote", "") : "");
            std::vector<FindHit> hits = res->hits;
            pick_open(tr("find", "Find"), note, items, [pi, hits](int i) {
                if (i < 0 || i >= (int)hits.size()) return;
                const FindHit& h = hits[i];
                if (h.type == 'd') panel_load(pi, h.path, "");
                else panel_load(pi, parent_of(h.path), base_of(h.path));
            });
        });
}

static void act_roots()
{
    int pi = g_active;
    auto roots = std::make_shared<std::vector<RootView>>();
    auto ok = std::make_shared<bool>(false);
    g_busy++; redraw();
    async_run(ASYNC_ACT, [roots, ok]() { std::string v; *ok = service_roots(*roots, v); },
        [pi, roots, ok]() {
            g_busy--; redraw();
            if (!*ok) { error_open("noconn"); return; }
            std::vector<std::string> items, paths;
            for (auto& r : *roots) { items.push_back(r.path + (r.smb ? "  (SMB)" : "")); paths.push_back(r.path); }
            pick_open(tr("roots", "Go to..."), "", items, [pi, paths](int i) { if (i >= 0 && i < (int)paths.size()) panel_load(pi, paths[i], ""); });
        });
}

static void act_log()
{
    auto txt = std::make_shared<std::string>();
    auto ok = std::make_shared<bool>(false);
    g_busy++; redraw();
    async_run(ASYNC_ACT, [txt, ok]() { *ok = service_log(16384, *txt); },
        [txt, ok]() { g_busy--; redraw(); if (!*ok) { error_open("noconn"); return; } text_open(tr("logTitle", "Log"), *txt); g_tv.scroll = std::max(0, (int)g_tv.lines.size() - 24); });
}

// ---------------------------------------------------------------- SMB

static void smb_open_list()
{
    auto list = std::make_shared<std::vector<SmbConnView>>();
    auto err = std::make_shared<std::string>();
    auto ok = std::make_shared<bool>(false);
    g_busy++; redraw();
    async_run(ASYNC_ACT, [list, err, ok]() { *ok = service_smb_list(*list, *err); },
        [list, err, ok]() {
            g_busy--; redraw();
            if (!*ok) { error_open(*err); return; }
            g_smbList = *list;
            g_smbCur = 0; g_smbScroll = 0;
            focus_push(F_SMBLIST);
        });
}

static void smb_edit_open(const SmbConnView* c)
{
    g_sf = SmbForm();
    if (c) { g_sf.c = *c; g_sf.editing = true; g_sf.hasPass = c->hasPass; if (!c->share.empty()) g_sf.shares.push_back(c->share); }
    g_sf.cur = c ? 1 : 0;
    g_sf.status = c && c->share.empty() ? tr("smbPickShare", "") : "";
    focus_push(F_SMBEDIT);
}

static void smb_status(const std::string& s, bool err) { g_sf.status = s; g_sf.statusErr = err; redraw(); }

static void smb_scan_start()
{
    smb_status(tr("smbConnecting", "Connecting..."), false);
    auto err = std::make_shared<std::string>();
    auto ok = std::make_shared<bool>(false);
    async_run(ASYNC_ACT, [err, ok]() { *ok = service_smb_scan_start(*err); },
        [err, ok]() {
            if (g_focus != F_SMBEDIT) return;
            if (!*ok && *err != "busy") { smb_status(tr_err(*err), true); return; }
            g_sf.scanning = true; g_smbNextPoll = 0; redraw();
        });
}

static void smb_poll_step(uint64_t now)
{
    if (!g_sf.scanning || g_focus != F_SMBEDIT || g_smbPollBusy || now < g_smbNextPoll) return;
    g_smbPollBusy = true;
    auto st = std::make_shared<SmbScanView>();
    auto ok = std::make_shared<bool>(false);
    async_run(ASYNC_POLL, [st, ok]() { *ok = service_smb_scan_state(*st); },
        [st, ok]() {
            g_smbPollBusy = false;
            if (!g_sf.scanning) return;
            if (!*ok) { g_sf.scanning = false; smb_status(tr_err("noconn"), true); return; }
            if (st->running) {
                smb_status(tr_sub("smbFinding", "Scanning... {d}/{t}", { { "d", std::to_string(st->done) }, { "t", std::to_string(st->total) } }), false);
                g_smbNextPoll = now_us() + 700000ULL;
                return;
            }
            g_sf.scanning = false;
            if (st->found.empty()) { smb_status(tr("smbFoundNone", "No SMB servers found"), false); return; }
            smb_status(tr_sub("smbFound", "Found servers: {n}", { { "n", std::to_string(st->found.size()) } }), false);
            std::vector<std::string> found = st->found;
            if (g_sf.c.host.empty()) g_sf.c.host = found[0];
            pick_open(tr("smbHost", "Server address"), "", found, [found](int i) { if (i >= 0 && i < (int)found.size()) g_sf.c.host = found[i]; });
        });
}

static void smb_connect()
{
    if (g_sf.c.host.empty()) { smb_status(tr("smbNoHost", "Enter the server address"), true); return; }
    smb_status(tr("smbConnecting", "Connecting..."), false);
    SmbConnView c = g_sf.c; std::string pass = g_sf.pass;
    auto shares = std::make_shared<std::vector<std::string>>();
    auto err = std::make_shared<std::string>();
    auto ok = std::make_shared<bool>(false);
    async_run(ASYNC_ACT, [c, pass, shares, err, ok]() { *ok = service_smb_shares(c, pass, *shares, *err); },
        [shares, err, ok]() {
            if (g_focus != F_SMBEDIT) return;
            if (!*ok) { smb_status(tr_err(*err), true); return; }
            g_sf.shares = *shares;
            bool has = std::find(shares->begin(), shares->end(), g_sf.c.share) != shares->end();
            if (!has) g_sf.c.share = shares->empty() ? "" : (*shares)[0];
            smb_status(tr_sub("smbSharesN", "Shares found: {n}", { { "n", std::to_string(shares->size()) } }), false);
        });
}

static void smb_save()
{
    if (g_sf.c.share.empty()) { smb_status(tr("smbNoShare", "Choose a share"), true); return; }
    smb_status(tr("smbConnecting", "Connecting..."), false);
    SmbConnView c = g_sf.c; std::string pass = g_sf.pass;
    auto err = std::make_shared<std::string>();
    auto ok = std::make_shared<bool>(false);
    async_run(ASYNC_ACT, [c, pass, err, ok]() { *ok = service_smb_save(c, pass, *err); },
        [err, ok]() {
            if (g_focus != F_SMBEDIT) return;
            if (!*ok) { smb_status(tr_err(*err), true); return; }
            focus_pop();          // закрыть форму
            refresh_all();        // в списке корней появилась новая папка
            focus_pop();          // закрыть и список (он устарел); меню SMB откроется заново
            smb_open_list();
        });
}

static void smb_pick_share()
{
    if (g_sf.shares.empty()) {
        std::string v;
        if (ime_text(tr("smbShare", "Share name"), g_sf.c.share, v)) g_sf.c.share = v;
        return;
    }
    std::vector<std::string> items = g_sf.shares;
    items.push_back(tr("smbManual", "Enter manually..."));
    int cur = 0;
    for (size_t i = 0; i < g_sf.shares.size(); i++) if (g_sf.shares[i] == g_sf.c.share) cur = (int)i;
    pick_open(tr("smbShare", "Share name"), "", items, [](int i) {
        if (i < 0) return;
        if (i >= (int)g_sf.shares.size()) { std::string v; if (ime_text(tr("smbShare", "Share name"), g_sf.c.share, v)) g_sf.c.share = v; }
        else g_sf.c.share = g_sf.shares[i];
    }, cur);
}

static void smb_form_activate(int row)
{
    std::string v;
    switch (row) {
    case 0: if (!g_sf.editing && ime_text(tr("smbAlias", "Mount folder name"), g_sf.c.alias, v)) g_sf.c.alias = v; break;
    case 1: if (ime_text(tr("smbHost", "Server address"), g_sf.c.host, v)) g_sf.c.host = v; break;
    case 2: if (ime_text(tr("smbUser", "User"), g_sf.c.user, v)) g_sf.c.user = v; break;
    case 3: if (ime_text(tr("smbPass", "Password"), "", v)) { g_sf.pass = v; g_sf.hasPass = false; } break;
    case 4: if (ime_text(tr("smbDomain", "Domain"), g_sf.c.domain, v)) g_sf.c.domain = v; break;
    case 5: smb_pick_share(); break;
    case 6: smb_scan_start(); break;
    case 7: smb_connect(); break;
    case 8: smb_save(); break;
    case 9: focus_pop(); break;
    }
    redraw();
}

static void smb_delete_conn(const SmbConnView& c)
{
    std::string alias = c.alias;
    confirm_open(tr("smbDel", "Remove"), tr_sub("smbDelQ", "Remove \"{n}\"?", { { "n", alias } }), tr("smbDel", "Remove"),
        [alias]() {
            act_async([alias](std::string& err) { return service_smb_del(alias, err); },
                      []() { refresh_all(); focus_pop(); smb_open_list(); });
        });
}

// ---------------------------------------------------------------- меню (как в вебе)

enum MenuId { M_THEME, M_LANG, M_FIND, M_PROPS, M_INSTALL, M_PKGQ, M_RENAME, M_ROOTS, M_SMB, M_LOG, M_SUPPORT, M_HELP, M_UNSAFE, M_APPEXIT, M_QUIT, M_CLOSE, M_COUNT };

struct MenuItem { MenuId id; const char* key; const char* def; };
static const MenuItem MENU[M_COUNT] = {
    { M_THEME, "theme", "Theme" }, { M_LANG, "lang", "Language" },
    { M_FIND, "find", "Find" }, { M_PROPS, "infoTitle", "Properties" }, { M_INSTALL, "install", "Install PKG" },
    { M_PKGQ, "pkgQueue", "Installation queue" }, { M_RENAME, "rename", "Rename" }, { M_ROOTS, "roots", "Go to..." },
    { M_SMB, "smbTitle", "Network folders (SMB)" }, { M_LOG, "logTitle", "Log" }, { M_SUPPORT, "support", "Support the project" },
    { M_HELP, "help", "Help" }, { M_UNSAFE, "unsafeBtn", "Allow changes in system areas" },
    { M_APPEXIT, "menuAppExit", "Close the app" }, { M_QUIT, "menuStopSvc", "Stop the service and exit" }, { M_CLOSE, "close", "Close" }
};

static std::string theme_name(const std::string& th)
{
    if (th == "dark") return tr("themeDark", "Dark");
    if (th == "light") return tr("themeLight", "Light");
    return tr("themeClassic", "Classic");
}

static void cycle_theme(int dir)
{
    std::string cur = gfx_theme();
    std::string order[3] = { "classic", "dark", "light" };
    int i = cur == "dark" ? 1 : cur == "light" ? 2 : 0;
    std::string nt = order[(i + dir + 3) % 3];
    gfx_set_theme(nt);
    act_async([nt](std::string&) { return service_state_set_theme(nt); }, NULL);
    redraw();
}

static void cycle_lang()
{
    i18n_set(i18n_current() == "ru" ? "en" : "ru");
    redraw();
}

static void set_unsafe(bool on) { g_unsafe = on; service_set_unsafe(on); redraw(); }

// Выход из приложения: останавливаем звук/видео и завершаем процесс без запуска деструкторов статических
// объектов (рабочие потоки ещё живы и обращаются к ним - так выход заканчивался аварией).
static void app_exit()
{
    audio_stop();
    video_close();
    logf_("ui: exit");
    _exit(0);
}

static void menu_run(MenuId id)
{
    switch (id) {
    case M_THEME: cycle_theme(1); return;
    case M_LANG: cycle_lang(); return;
    case M_UNSAFE: set_unsafe(!g_unsafe); return;
    default: break;
    }
    focus_pop();     // как в вебе: меню закрывается, затем выполняется действие
    switch (id) {
    case M_FIND: act_find(); break;
    case M_PROPS: act_props(); break;
    case M_INSTALL: act_install(); break;
    case M_PKGQ: pkgq_open(); break;
    case M_RENAME: act_rename(); break;
    case M_ROOTS: act_roots(); break;
    case M_SMB: smb_open_list(); break;
    case M_LOG: act_log(); break;
    case M_SUPPORT:
        if (!g_qrTried) { g_qrTried = true; g_qrSbpOk = g_qrSbp.load("/app0/assets/images/qr_sbp.png"); g_qrPaypalOk = g_qrPaypal.load("/app0/assets/images/qr_paypal.png"); }
        focus_push(F_DONATE);
        break;
    case M_HELP: text_open(tr("helpTitle", "Keys"), tr("helpPad", "")); break;
    case M_APPEXIT: app_exit(); break;
    case M_QUIT:
        confirm_open(tr("menuStopSvc", "Stop the service and exit"), tr("quitQ", "Shut down ps4commander?"), tr("quit", "Quit"), []() {
            act_async([](std::string&) { service_quit(); return true; }, []() { app_exit(); });
        });
        break;
    default: break;
    }
}

// ---------------------------------------------------------------- нижняя строка F1-F10 и контекстное меню

static const char* KEY_NAMES[10] = { "help", "refresh", "view", "edit", "copy", "move", "mkdir", "del", "chmod", "menu" };
static const char* KEY_DEFAULT[10] = { "Help", "Refresh", "View", "Edit", "Copy", "Move", "MkDir", "Delete", "Chmod", "Menu" };

static void open_menu() { g_menuCursor = 0; focus_push(F_MENU); }

static void run_bar_action(int idx)
{
    switch (idx) {
    case 0: text_open(tr("helpTitle", "Keys"), tr("helpPad", "")); break;
    case 1: refresh_all(); break;
    case 2: act_view(); break;
    case 3: act_edit(); break;
    case 4: act_copy_move(false); break;
    case 5: act_copy_move(true); break;
    case 6: act_mkdir(); break;
    case 7: act_delete(); break;
    case 8: act_chmod(); break;
    case 9: open_menu(); break;
    }
}

// Контекстное меню на файле (Треугольник): Rename / Select / Properties / Cancel (+ Install для .pkg).
static const char* CTX_KEYS[7] = { "rename", "select", "infoTitle", "del", "install", "cancel", "pkgTitle" };
static const char* CTX_DEFS[7] = { "Rename", "Select", "Properties", "Delete", "Install PKG", "Cancel", "Package installation" };
// Пункты: "Install" - только на .pkg; "очередь установки" - только пока что-то устанавливается.
static int ctx_count(bool pkg) { return 5 + (pkg ? 1 : 0) + (g_pkgBg ? 1 : 0); }
static int ctx_id(int i, bool pkg)
{
    int ids[7], n = 0;
    ids[n++] = 0; ids[n++] = 1; ids[n++] = 2; ids[n++] = 3;
    if (pkg) ids[n++] = 4;
    if (g_pkgBg) ids[n++] = 6;
    ids[n++] = 5;
    return (i >= 0 && i < n) ? ids[i] : 5;
}

static void toggle_mark(Panel& p, bool advance)
{
    Entry2* e = panel_cur(p);
    if (!e || e->name == "..") return;
    auto it = std::find(p.sel.begin(), p.sel.end(), e->name);
    if (it == p.sel.end()) p.sel.push_back(e->name); else p.sel.erase(it);
    if (advance && !p.items.empty()) p.cur = (p.cur + 1) % (int)p.items.size();   // как в Total Commander
}

static void ctx_run(int idx)
{
    Entry2* ce = panel_cur(g_panels[g_active]);
    idx = ctx_id(idx, ce && is_pkg(*ce));
    focus_pop();
    switch (idx) {
    case 0: act_rename(); break;
    case 1: toggle_mark(g_panels[g_active], false); break;
    case 2: act_props(); break;
    case 3: act_delete(); break;
    case 4: act_install(); break;
    case 6: pkgq_open(); break;
    default: break;
    }
}

// ---------------------------------------------------------------- отрисовка

static Color dlg_dim() { return T.modern ? T.dim : Color{ 70, 70, 70 }; }
static int rad() { return T.radius; }

static void dlg_box(int w, int h, const std::string& title, int& x, int& y)
{
    x = (FRAME_WIDTH - w) / 2; y = (FRAME_HEIGHT - h) / 2;
    draw_frame(x, y, w, h, rad(), T.dlgBorder, T.dlgBg);
    if (!title.empty()) {
        put_center(FRAME_WIDTH / 2, fit(title, (w - 60) / cell_w()), y + 44, T.dlgFg, T.dlgBg);
        fill(x + 24, y + 58, w - 48, 1, T.dlgBorder);
    }
}

static void dlg_hint(int x, int y, int w, int h, const std::string& s)
{
    (void)y;
    put_center_rich(x + w / 2, s, y + h - 22, dlg_dim(), T.dlgBg);
}

static void draw_row_hl(int x, int y, int w, int rowH, bool hl)
{
    if (hl) fill_rounded(x, y, w, rowH, T.modern ? 10 : 0, T.curBg);
}


static void draw_panel_box(int idx, int x, int w)
{
    Panel& p = g_panels[idx];
    bool act = idx == g_active;
    Color edge = p.danger ? T.warn : (act ? T.accent : T.border);
    draw_frame(x, PANEL_TOP, w, PANEL_H, rad() > 0 ? 18 : 0, edge, T.panel);

    // заголовок: путь (активная панель подсвечена)
    Color hb = act ? T.curBg : (T.modern ? T.surface2 : T.panel);
    Color hf = act ? T.curFg : (T.modern ? T.dim : T.border);
    if (T.modern) {   // шапка повторяет скругление верхних углов панели
        fill_rounded(x + 1, PANEL_TOP + 1, w - 2, HEAD_H, 17, hb);
        fill(x + 1, PANEL_TOP + HEAD_H / 2, w - 2, HEAD_H / 2, hb);
    } else fill(x, PANEL_TOP, w, HEAD_H, hb);
    std::string title = p.path;
    if (p.loading) title += "   " + tr("loading", "Loading...");
    else if (!p.err.empty()) title += "   [" + tr_err(p.err) + "]";
    draw_text(fit(title, (w - 24) / cell_w()), x + 12, PANEL_TOP + 24, p.err.empty() || p.loading ? hf : T.warn, hb);

    // шапка столбцов
    int sizeW = 8 * cell_w() + 10, dateW = 16 * cell_w() + 14, attrW = 4 * cell_w() + 14;
    int colTop = PANEL_TOP + HEAD_H;
    fill(x, colTop + COLH_H - 1, w, 1, T.border);
    int xs = x + w - 10 - attrW - dateW - sizeW, xd = x + w - 10 - attrW - dateW, xa = x + w - 10 - attrW;
    draw_text(tr("name", "Name"), x + 30, colTop + 20, T.dim, T.panel);
    draw_text(tr("size", "Size"), xs + sizeW - 4 * cell_w() - 6, colTop + 20, T.dim, T.panel);
    draw_text(tr("date", "Date"), xd + 4, colTop + 20, T.dim, T.panel);
    draw_text(tr("attr", "Attr"), xa + 4, colTop + 20, T.dim, T.panel);

    // строки
    int rows = (PANEL_H - HEAD_H - COLH_H - FOOT_H) / ROW_H;
    if (p.cur < p.scroll) p.scroll = p.cur;
    if (p.cur >= p.scroll + rows) p.scroll = p.cur - rows + 1;
    if (p.scroll < 0) p.scroll = 0;
    int listTop = colTop + COLH_H;
    int nameChars = (xs - (x + 30) - 8) / cell_w();
    for (int row = 0; row < rows; row++) {
        int i = p.scroll + row;
        if (i >= (int)p.items.size()) break;
        Entry2& e = p.items[i];
        int ry = listTop + row * ROW_H;
        bool isCur = i == p.cur;
        bool marked = e.name != ".." && is_marked(p, e.name);
        Color bg = T.panel, fg = T.text;
        if (isCur) { bg = act ? T.curBg : T.curInactive; fg = act ? T.curFg : T.text; }
        if (isCur) fill_rounded(x + 4, ry, w - 8, ROW_H, T.modern ? 8 : 0, bg);
        else bg = T.panel;
        if (!(isCur && act)) {
            if (e.type == 'l') fg = T.ok;
            if (e.isDir()) fg = T.dir;
            if (marked) fg = T.sel;
        } else if (marked) fg = T.sel;
        int base = ry + ROW_H - 8;
        if (marked) draw_text("*", x + 12, base, T.sel, bg);
        std::string nm = e.name == ".." ? ".." : (e.isDir() ? "/" : "") + e.name + (e.type == 'l' && !e.link.empty() ? " -> " + e.link : "");
        draw_text(fit(nm, nameChars), x + 30, base, fg, bg);
        if (e.name != "..") {
            std::string sz = e.type == 'd' ? tr("dirTag", "<DIR>") : (e.type == 'l' && e.td) ? tr("lnkTag", "<LNK>") : e.type == 'f' ? fmt_size(e.size) : "";
            draw_text(sz, xs + sizeW - count_cp(sz) * cell_w() - 6, base, fg, bg);
            draw_text(fmt_date(e.mtime), xd + 4, base, fg, bg);
            draw_text(e.perm, xa + 4, base, fg, bg);
        } else if (e.name == "..") {
            std::string sz = tr("dirTag", "<DIR>");
            draw_text(sz, xs + sizeW - count_cp(sz) * cell_w() - 6, base, fg, bg);
        }
    }

    // подвал: слева - отмеченное или сведения о файле, справа - свободное место
    int fy = PANEL_TOP + PANEL_H - FOOT_H;
    fill(x, fy, w, 1, T.border);
    std::string left;
    if (!p.sel.empty()) {
        int64_t sz = 0;
        for (auto& e : p.items) if (e.type == 'f' && is_marked(p, e.name)) sz += e.size;
        left = tr("selected", "selected") + ": " + std::to_string(p.sel.size()) + " (" + fmt_size(sz) + ")";
    } else if (Entry2* e = panel_cur(p)) {
        if (e->name != "..") left = (e->type == 'f' ? fmt_size(e->size) + "  " : "") + e->perm + "  " + e->user + ":" + e->group;
        else left = std::to_string(p.items.size() - (p.hasUp ? 1 : 0)) + " " + tr("items", "items");
    }
    std::string right;
    if (p.freeB >= 0) right = fmt_size(p.freeB) + " " + tr("free", "free") + (p.totalB > 0 ? " " + tr("of", "of") + " " + fmt_size(p.totalB) : "");
    int rw = count_cp(right) * cell_w();
    draw_text(fit(left, (w - 40 - rw) / cell_w()), x + 12, fy + 24, T.dim, T.panel);
    draw_text(right, x + w - 12 - rw, fy + 24, T.dim, T.panel);
}

static void draw_topbar()
{
    draw_text("PS4 Commander", MARGIN_X, TOP_BASE, T.dir, T.bg);
    int x = MARGIN_X + 14 * cell_w() + 14;
    std::string addr = "http://" + (service_ip().empty() ? std::string("<IP>") : service_ip()) + ":" + std::to_string(SERVICE_PORT);
    draw_text(addr, x, TOP_BASE, T.modern ? T.text : T.border, T.bg);
    x += count_cp(addr) * cell_w() + 24;
    fill_rounded(x, TOP_BASE - 14, 12, 12, 6, g_svc == S_READY ? T.ok : T.warn);
    x += 28;
    if (g_unsafe) draw_text(tr("unsafeOn", "SYSTEM AREAS UNLOCKED"), x, TOP_BASE, T.warn, T.bg);
    if (g_busy > 0 || g_panels[0].loading || g_panels[1].loading) {
        static const char* spin[4] = { "|", "/", "-", "\\" };
        int f = (int)((now_us() / 150000ULL) % 4);
        draw_text(spin[f], x + 40 * cell_w(), TOP_BASE, T.sel, T.bg);
    }
    std::string mt = tr("menu", "Menu");
    int bw = (count_cp(mt) + 4) * cell_w() + 40;
    int bx = FRAME_WIDTH - MARGIN_X - bw;
    fill_rounded(bx, TOP_BASE - 26, bw, 34, T.modern ? 17 : 0, T.modern ? T.surface2 : T.bg);
    if (!T.modern) { fill(bx, TOP_BASE - 26, bw, 1, T.border); fill(bx, TOP_BASE + 7, bw, 1, T.border); fill(bx, TOP_BASE - 26, 1, 34, T.border); fill(bx + bw - 1, TOP_BASE - 26, 1, 34, T.border); }
    draw_rich("{options} " + mt, bx + 20, TOP_BASE - 2, T.modern ? T.text : T.border, T.modern ? T.surface2 : T.bg);
}

static void draw_bar()
{
    int n = 10, gap = T.modern ? 8 : 2;
    int bw = (FRAME_WIDTH - 2 * MARGIN_X - gap * (n - 1)) / n;
    for (int pass = 0; pass < 2; pass++)      // выбранную кнопку рисуем последней, чтобы рамка не затиралась соседями
    for (int i = 0; i < n; i++) {
        int bx = MARGIN_X + i * (bw + gap);
        bool hl = g_focus == F_BAR && i == g_barCursor;
        if (hl != (pass == 1)) continue;
        // выбранная кнопка должна быть хорошо видна во всех темах: инверсия цветов + рамка
        Color bg = T.barBg, fg = T.barFg, numBg = T.barNum, numFg = T.barNumFg;
        if (hl) {
            if (T.modern) { bg = T.accent; fg = T.accentFg; numBg = T.accentFg; numFg = T.accent; }
            else { bg = Color{ 0, 0, 0 }; fg = T.sel; }
        }
        if (hl && !T.modern) { fill(bx - 3, BAR_TOP - 3, bw + 6, BAR_H + 6, T.sel); }
        if (T.modern) fill_rounded(bx, BAR_TOP, bw, BAR_H, 12, bg); else fill(bx, BAR_TOP, bw, BAR_H, bg);
        if (hl && T.modern) { fill_rounded(bx - 3, BAR_TOP - 3, bw + 6, BAR_H + 6, 15, T.text); fill_rounded(bx, BAR_TOP, bw, BAR_H, 12, bg); }
        char num[4]; snprintf(num, sizeof(num), "%d", i + 1);
        int nw = (int)strlen(num) * cell_w() + 14;
        if (T.modern) fill_rounded(bx + 6, BAR_TOP + 8, nw, BAR_H - 16, 8, numBg); else fill(bx, BAR_TOP, nw, BAR_H, numBg);
        draw_text(num, bx + (T.modern ? 13 : 7), BAR_TOP + BAR_H / 2 + 7, numFg, numBg);
        draw_text(fit(tr(KEY_NAMES[i], KEY_DEFAULT[i]), (bw - nw - 18) / cell_w()), bx + nw + (T.modern ? 14 : 8), BAR_TOP + BAR_H / 2 + 7, fg, bg);
    }
}

// Имя выбранного файла/папки над кнопками - как showName() в веб-версии: "/" у папок, " -> цель" у ссылок.
static void draw_selname()
{
    Panel& p = g_panels[g_active];
    Entry2* e = panel_cur(p);
    if (!e) return;
    std::string nm;
    if (e->name == "..") nm = "..";
    else nm = (e->isDir() ? "/" : "") + e->name + (e->type == 'l' && !e->link.empty() ? " -> " + e->link : "");
    draw_text(fit(nm, (FRAME_WIDTH - 2 * MARGIN_X) / cell_w()), MARGIN_X, NAME_Y, T.dim, T.bg);
}

// Подсказки по кнопкам внизу - одна строка; L1/R1/L2/R2 - обычным текстом (значки обрезались).
static void draw_hints_credit()
{
    std::string sep = "     ", l;
    auto build = [&](const std::string& s) {
        if (g_focus == F_BAR)
            return "L2/R2 " + tr("hint_pick", "choose") + s + "{cross} " + tr("hint_do", "do") + s + "{circle} " + tr("hint_back", "back") +
                   s + "L1/R1 " + tr("hint_swap", "switch panel");
        return "{arrows} " + tr("hint_nav", "move") + s + "{cross} " + tr("hint_enter", "open") + s + "{circle} " + tr("hint_up", "up") +
               s + "{square} " + tr("hint_mark", "mark") + s + "{triangle} " + tr("hint_ctx", "file menu") +
               s + "L1/R1 " + tr("hint_swap", "switch panel") + s + "L2/R2 " + tr("hint_bar", "bottom bar") + s + "{options} " + tr("menu", "Menu");
    };
    l = build(sep);
    if (rich_width(l) > FRAME_WIDTH - 2 * MARGIN_X) l = build("   ");
    if (rich_width(l) > FRAME_WIDTH - 2 * MARGIN_X) l = build("  ");
    put_center_rich(FRAME_WIDTH / 2, l, HINT_Y, T.dim, T.bg);
    std::string credit = "ps4commander v" APP_VERSION " \xC2\xB7 Created by SergioPoverony and Mr.Claude";
    put_center(FRAME_WIDTH / 2, credit, CREDIT_Y, T.dimmer, T.bg);
}

static void draw_menu()
{
    int w = 900, rowH = 38;
    std::string warn = tr("unsafeWarn", "");
    std::vector<std::string> wl = wrap_lines(warn, (w - 100) / cell_w());
    int h = 120 + M_COUNT * rowH + (int)wl.size() * 26 + 30;
    int x, y; dlg_box(w, h, tr("menu", "Menu"), x, y);
    int ry = y + 76;
    for (int i = 0; i < M_COUNT; i++) {
        const MenuItem& it = MENU[i];
        bool hl = i == g_menuCursor;
        draw_row_hl(x + 20, ry - 4, w - 40, rowH - 2, hl);
        Color fg = hl ? T.curFg : T.dlgFg, bg = hl ? T.curBg : T.dlgBg;
        std::string s = tr(it.key, it.def);
        if (it.id == M_THEME) s += ":   < " + theme_name(gfx_theme()) + " >";
        else if (it.id == M_LANG) s += ":   < " + std::string(i18n_current() == "ru" ? "Russian" : "English") + " >";
        else if (it.id == M_UNSAFE) s = std::string(g_unsafe ? "[x] " : "[ ] ") + s;
        else if (it.id == M_QUIT) fg = hl ? T.curFg : (T.modern ? T.warn : Color{ 150, 0, 0 });
        draw_text(s, x + 40, ry + 24, fg, bg);
        ry += rowH;
        if (it.id == M_UNSAFE) {
            for (auto& l : wl) { draw_text(l, x + 60, ry + 16, T.modern ? T.warn : Color{ 150, 0, 0 }, T.dlgBg); ry += 26; }
        }
    }
    dlg_hint(x, y, w, h, "{updown} " + tr("hint_nav", "move") + "    {cross} " + tr("hint_do", "do") + "    {circle} " + tr("hint_back", "back"));
}

static void draw_ctxmenu()
{
    Entry2* e = panel_cur(g_panels[g_active]);
    bool pkg = e && is_pkg(*e);
    int n = ctx_count(pkg);
    int w = 520, h = 100 + n * 48 + 40;
    int x, y; dlg_box(w, h, "", x, y);
    put_center(FRAME_WIDTH / 2, e ? fit(e->name, (w - 40) / cell_w()) : "", y + 44, T.modern ? T.accent : T.dlgFg, T.dlgBg);
    fill(x + 24, y + 58, w - 48, 1, T.dlgBorder);
    for (int i = 0; i < n; i++) {
        int ry = y + 76 + i * 48;
        bool hl = i == g_ctxCursor;
        draw_row_hl(x + 20, ry - 4, w - 40, 42, hl);
        draw_text(tr(CTX_KEYS[ctx_id(i, pkg)], CTX_DEFS[ctx_id(i, pkg)]), x + 40, ry + 24, hl ? T.curFg : T.dlgFg, hl ? T.curBg : T.dlgBg);
    }
}

static void draw_confirm()
{
    int w = 960, h = 300;
    int x, y; dlg_box(w, h, "", x, y);
    int maxc = (w - 80) / cell_w();
    std::vector<std::string> tl = wrap_lines(g_cf.title, maxc);
    int ty = y + 60;
    for (size_t i = 0; i < tl.size() && i < 3; i++) { put_center(FRAME_WIDTH / 2, tl[i], ty, T.dlgFg, T.dlgBg); ty += 34; }
    std::vector<std::string> bl = wrap_lines(g_cf.body, maxc);
    for (size_t i = 0; i < bl.size() && i < 3; i++) { draw_text(bl[i], x + 40, ty + 20, dlg_dim(), T.dlgBg); ty += 28; }
    dlg_hint(x, y, w, h, "{cross} " + g_cf.yes + "          {circle} " + tr("cancel", "Cancel"));
}

static void draw_text_view()
{
    int maxc = 0;
    for (auto& l : g_tv.lines) maxc = std::max(maxc, count_cp(l));
    int w = std::min(1560, std::max(760, maxc * cell_w() + 80));
    int lineH = 26;
    int rows = std::min((int)g_tv.lines.size(), 28);
    if (rows < 3) rows = 3;
    int h = 150 + rows * lineH + 30;
    int x, y; dlg_box(w, h, g_tv.title, x, y);
    int maxScroll = std::max(0, (int)g_tv.lines.size() - rows);
    if (g_tv.scroll > maxScroll) g_tv.scroll = maxScroll;
    if (g_tv.scroll < 0) g_tv.scroll = 0;
    for (int r = 0; r < rows; r++) {
        int i = g_tv.scroll + r;
        if (i >= (int)g_tv.lines.size()) break;
        draw_text(g_tv.lines[i], x + 40, y + 100 + r * lineH, g_tv.error ? (T.modern ? T.warn : Color{ 150, 0, 0 }) : T.dlgFg, T.dlgBg);
    }
    std::string hint = "{circle} " + tr("close", "Close");
    if ((int)g_tv.lines.size() > rows) hint = "{updown} L2/R2 " + tr("hint_nav", "move") + "    " + hint;
    dlg_hint(x, y, w, h, hint);
}

static void draw_pick()
{
    int w = 1200, rowH = 34, vis = 16;
    int n = (int)g_pick.items.size();
    int rows = std::min(n, vis);
    int h = 150 + rows * rowH + (g_pick.note.empty() ? 0 : 30) + 20;
    int x, y; dlg_box(w, h, g_pick.title, x, y);
    if (g_pick.cur < g_pick.scroll) g_pick.scroll = g_pick.cur;
    if (g_pick.cur >= g_pick.scroll + rows) g_pick.scroll = g_pick.cur - rows + 1;
    int ry = y + 78;
    for (int r = 0; r < rows; r++) {
        int i = g_pick.scroll + r;
        bool hl = i == g_pick.cur;
        draw_row_hl(x + 20, ry - 2, w - 40, rowH - 2, hl);
        draw_text(fit(g_pick.items[i], (w - 100) / cell_w()), x + 40, ry + 22, hl ? T.curFg : T.dlgFg, hl ? T.curBg : T.dlgBg);
        ry += rowH;
    }
    if (!g_pick.note.empty()) draw_text(fit(g_pick.note, (w - 80) / cell_w()), x + 40, ry + 24, dlg_dim(), T.dlgBg);
    dlg_hint(x, y, w, h, "{updown} " + tr("hint_nav", "move") + "    {cross} " + tr("hint_pick", "choose") + "    {circle} " + tr("hint_back", "back"));
}

static void draw_bar_progress(int x, int y, int w, double frac)
{
    int bh = 22;
    fill_rounded(x, y, w, bh, T.modern ? 10 : 0, T.modern ? T.border : T.panel);   // дорожка заметно темнее/светлее фона окна
    if (frac > 0) fill_rounded(x, y, std::max(4, (int)(w * std::min(1.0, frac))), bh, T.modern ? 10 : 0, T.modern ? T.accent : T.curBg);
}

static void draw_progress()
{
    int w = 900, h = 260;
    int x, y; dlg_box(w, h, g_job.op.empty() ? tr("loading", "Loading...") : g_job.op, x, y);
    double frac = g_job.total > 0 ? (double)g_job.done / (double)g_job.total : 0;
    draw_text(fit(g_job.cur, (w - 80) / cell_w()), x + 40, y + 110, T.dlgFg, T.dlgBg);
    std::string s = fmt_size(g_job.done) + " / " + fmt_size(g_job.total);
    if (g_job.filesTotal > 0) s += "    " + std::to_string(g_job.filesDone) + " / " + std::to_string(g_job.filesTotal);
    draw_text(s, x + 40, y + 148, dlg_dim(), T.dlgBg);
    draw_bar_progress(x + 40, y + 168, w - 80, frac);
    dlg_hint(x, y, w, h, "{cross} " + tr("cancel", "Cancel"));
}

static std::string pkg_status_text(const std::string& st)
{
    static const struct { const char* st; const char* key; const char* def; } M[] = {
        { "queued", "stQueued", "queued" }, { "registering", "stRegistering", "registering" }, { "installing", "stInstalling", "installing" },
        { "done", "stDone", "installed" }, { "already_installed", "stAlready", "already installed" }, { "failed", "stFailed", "failed" },
        { "cancelled", "stCancelled", "cancelled" } };
    for (auto& m : M) if (st == m.st) return tr(m.key, m.def);
    return tr("stQueued", "queued");
}

static void draw_pkgq()
{
    int w = 1300, rowH = 78;
    int n = std::min((int)g_pkgq.items.size(), 6);
    int h = 190 + std::max(1, n) * rowH;
    int x, y; dlg_box(w, h, tr("pkgTitle", "Package installation"), x, y);
    if (g_pkgq.items.empty()) draw_text(tr("loading", "Loading..."), x + 40, y + 110, dlg_dim(), T.dlgBg);
    int start = std::max(0, (int)g_pkgq.items.size() - n);
    for (int r = 0; r < n; r++) {
        const PkgItemView& it = g_pkgq.items[start + r];
        int ry = y + 96 + r * rowH;
        bool full = it.status == "done" || it.status == "already_installed";
        double frac = full ? 1.0 : (it.status == "installing" && it.total > 0 ? (double)it.done / (double)it.total : 0.0);
        std::string line = it.name + "   " + pkg_status_text(it.status) + (it.status == "installing" && it.total > 0 ? "  " + std::to_string((int)(frac * 100)) + "%" : "");
        draw_text(fit(line, (w - 80) / cell_w()), x + 40, ry + 20, T.dlgFg, T.dlgBg);
        draw_bar_progress(x + 40, ry + 30, w - 80, frac);
        if (it.status == "installing" && it.total > 0) draw_text(fmt_size(it.done) + " / " + fmt_size(it.total), x + 40, ry + 72, dlg_dim(), T.dlgBg);
        else if (!it.msg.empty()) draw_text(fit(it.msg, (w - 80) / cell_w()), x + 40, ry + 72, dlg_dim(), T.dlgBg);
    }
    dlg_hint(x, y, w, h, "{cross} " + tr("cancel", "Cancel") + "          {circle} " + tr("close", "Close"));
}

static void draw_image()
{
    fill(0, 0, FRAME_WIDTH, FRAME_HEIGHT, Color{ 0, 0, 0 });
    Color dim = Color{ 190, 190, 190 };
    if (g_iv.ok && !g_iv.pic.px.empty())
        gfx_blit((FRAME_WIDTH - g_iv.pic.w) / 2, (1000 - g_iv.pic.h) / 2, g_iv.pic.w, g_iv.pic.h, g_iv.pic.px.data());
    else if (g_iv.loading) put_center(FRAME_WIDTH / 2, tr("loading", "Loading..."), 500, dim, Color{ 0, 0, 0 });
    else put_center(FRAME_WIDTH / 2, tr("imgErr", "Cannot display this image"), 500, Color{ 255, 120, 120 }, Color{ 0, 0, 0 });
    std::string cap = fit(g_iv.name, 90) + "   (" + std::to_string(g_iv.idx + 1) + "/" + std::to_string(g_iv.list.size()) + ")";
    if (g_iv.ok) cap += "   " + std::to_string(g_iv.pic.w) + "x" + std::to_string(g_iv.pic.h);
    put_center(FRAME_WIDTH / 2, cap, 1034, dim, Color{ 0, 0, 0 });
    std::string h = "{circle} " + tr("close", "Close");
    if (g_iv.list.size() > 1) h = "{arrows} " + tr("hint_imgnav", "previous / next") + "     " + h;
    put_center_rich(FRAME_WIDTH / 2, h, 1066, dim, Color{ 0, 0, 0 });
}

static std::string fmt_time(double s)
{
    int t = (int)s;
    char b[16];
    snprintf(b, sizeof(b), "%d:%02d", t / 60, t % 60);
    return b;
}

static void draw_audio()
{
    int w = 1000, h = 320;
    int x, y; dlg_box(w, h, tr("audioTitle", "Player"), x, y);
    draw_text(fit(g_au.name, (w - 80) / cell_w()), x + 40, y + 110, T.dlgFg, T.dlgBg);
    int st = audio_state();
    std::string s;
    if (st == 4) s = tr("audioErr", "Cannot play this file");
    else if (st == 3) s = tr("audioDone", "Finished");
    else if (st == 1) s = tr("loading", "Loading...");
    else s = audio_paused() ? tr("audioPaused", "Paused") : tr("audioPlaying", "Playing");
    draw_text(s, x + 40, y + 154, st == 4 ? (T.modern ? T.warn : Color{ 150, 0, 0 }) : dlg_dim(), T.dlgBg);
    double pos = audio_seconds(), tot = audio_total();
    std::string tm = fmt_time(pos) + (tot > 0 ? " / " + fmt_time(tot) : "");
    draw_text(tm, x + w - 40 - count_cp(tm) * cell_w(), y + 154, dlg_dim(), T.dlgBg);
    draw_bar_progress(x + 40, y + 180, w - 80, tot > 0 ? pos / tot : (st == 3 ? 1.0 : 0.0));
    dlg_hint(x, y, w, h, "{cross} " + tr("hint_pause", "pause / play") + "     {arrows} -10 / +10 s     {circle} " + tr("hint_stop", "stop"));
}

static void draw_video()
{
    Color blk = Color{ 0, 0, 0 }, dim = Color{ 190, 190, 190 };
    fill(0, 0, FRAME_WIDTH, FRAME_HEIGHT, blk);
    int st = video_state();
    const Picture& pic = video_picture();
    if (!pic.px.empty()) gfx_blit((FRAME_WIDTH - pic.w) / 2, (940 - pic.h) / 2, pic.w, pic.h, pic.px.data());
    std::string msg;
    Color mc = dim;
    if (st == 1) msg = tr("loading", "Loading...");
    else if (st == 4) { msg = tr("videoErr", "Cannot play this file") + " [" + video_error() + "]"; mc = Color{ 255, 120, 120 }; }
    else if (st == 3) msg = tr("audioDone", "Finished");
    else if (pic.px.empty() && st == 2) msg = video_has_video() ? tr("loading", "Loading...") : tr("audioPlaying", "Playing");
    if (!msg.empty()) put_center(FRAME_WIDTH / 2, msg, 480, mc, blk);
    if (st == 2 && !video_has_video() && !video_note().empty())
        put_center(FRAME_WIDTH / 2, fit(tr_sub("vidNoDecode", "The video track ({c}) is not supported by the console player: audio only.", { { "c", video_note() } }), 140), 530, Color{ 255, 190, 120 }, blk);

    // нижняя панель: имя, время, полоска, подсказка
    double pos = video_seconds(), tot = video_total();
    std::string tm = fmt_time(pos) + (tot > 0 ? " / " + fmt_time(tot) : "");
    if (video_paused()) tm += "   " + tr("audioPaused", "Paused");
    draw_text(fit(g_au.name, 100), 60, 986, dim, blk);
    draw_text(tm, FRAME_WIDTH - 60 - count_cp(tm) * cell_w(), 986, dim, blk);
    int bx = 60, bw = FRAME_WIDTH - 120;
    fill(bx, 998, bw, 8, Color{ 70, 70, 70 });
    if (tot > 0) fill(bx, 998, std::max(4, (int)(bw * std::min(1.0, pos / tot))), 8, Color{ 138, 180, 255 });
    put_center_rich(FRAME_WIDTH / 2, "{cross} " + tr("hint_pause", "pause / play") + "     {arrows} -10 / +10 s     {circle} " + tr("hint_stop", "stop"), 1050, dim, blk);
}

static void draw_donate()
{
    int w = 700, h = 580;
    int x, y; dlg_box(w, h, tr("support", "Support the project"), x, y);
    bool ru = i18n_current() == "ru";
    Image& img = ru ? g_qrSbp : g_qrPaypal;
    bool ok = ru ? g_qrSbpOk : g_qrPaypalOk;
    if (ok && img.pixels) {
        int iw = 420, ih = 420 * img.height / img.width;
        int ix = x + (w - iw) / 2, iy = y + 84;
        for (int yy = 0; yy < ih; yy++) {
            int sy = yy * img.height / ih;
            for (int xx = 0; xx < iw; xx++) {
                int sx = xx * img.width / iw;
                unsigned char* px = img.pixels + (size_t)(sy * img.width + sx) * 4;
                fill(ix + xx, iy + yy, 1, 1, Color{ px[0], px[1], px[2] });
            }
        }
        put_center(FRAME_WIDTH / 2, tr("supportCap", "PayPal"), y + h - 70, dlg_dim(), T.dlgBg);
    } else {
        put_center(FRAME_WIDTH / 2, tr("noQr", "QR code not found in the app"), y + h / 2, dlg_dim(), T.dlgBg);
    }
    dlg_hint(x, y, w, h, "{circle} " + tr("close", "Close"));
}

static void draw_smb_list()
{
    int w = 1300, rowH = 38, vis = 12;
    int n = (int)g_smbList.size() + 1;             // первая строка - "Добавить"
    int rows = std::min(n, vis);
    int h = 170 + rows * rowH + 40;
    int x, y; dlg_box(w, h, tr("smbTitle", "Network folders (SMB)"), x, y);
    if (g_smbCur < g_smbScroll) g_smbScroll = g_smbCur;
    if (g_smbCur >= g_smbScroll + rows) g_smbScroll = g_smbCur - rows + 1;
    int ry = y + 78;
    for (int r = 0; r < rows; r++) {
        int i = g_smbScroll + r;
        bool hl = i == g_smbCur;
        draw_row_hl(x + 20, ry - 2, w - 40, rowH - 2, hl);
        Color fg = hl ? T.curFg : T.dlgFg, bg = hl ? T.curBg : T.dlgBg;
        std::string s;
        if (i == 0) s = "+ " + tr("smbAdd", "Add");
        else { const SmbConnView& c = g_smbList[i - 1]; s = c.alias + "   \\\\" + c.host + "\\" + c.share + (c.user.empty() ? "" : "   (" + c.user + ")"); }
        draw_text(fit(s, (w - 100) / cell_w()), x + 40, ry + 24, fg, bg);
        ry += rowH;
    }
    if (g_smbList.empty()) draw_text(tr("smbNone", "No network folders yet"), x + 40, ry + 28, dlg_dim(), T.dlgBg);
    dlg_hint(x, y, w, h, "{cross} " + tr("smbOpen", "Open") + "    {square} " + tr("smbEdit", "Edit") + "    {triangle} " + tr("smbDel", "Remove") + "    {circle} " + tr("close", "Close"));
}

static void draw_smb_edit()
{
    int w = 1200, rowH = 40;
    const int N = 10;
    int h = 130 + N * rowH + 80;
    int x, y; dlg_box(w, h, tr("smbTitle", "Network folders (SMB)"), x, y);
    std::string pass = g_sf.pass.empty() ? (g_sf.hasPass ? tr("smbPassKeep", "(keep)") : "") : std::string((size_t)count_cp(g_sf.pass), '*');
    struct Row { std::string label, value; bool action; };
    Row rows[N] = {
        { tr("smbAlias", "Mount folder name"), g_sf.c.alias, false },
        { tr("smbHost", "Server address"), g_sf.c.host, false },
        { tr("smbUser", "User"), g_sf.c.user, false },
        { tr("smbPass", "Password"), pass, false },
        { tr("smbDomain", "Domain"), g_sf.c.domain, false },
        { tr("smbShare", "Share name"), g_sf.c.share, false },
        { tr("smbFind", "Find"), "", true },
        { tr("smbShares", "Connect"), "", true },
        { tr("save", "Save"), "", true },
        { tr("cancel", "Cancel"), "", true } };
    int ry = y + 76;
    int labelW = 42 * cell_w();
    for (int i = 0; i < N; i++) {
        bool hl = i == g_sf.cur;
        draw_row_hl(x + 20, ry - 2, w - 40, rowH - 2, hl);
        bool disabled = (i == 0 && g_sf.editing);
        Color fg = hl ? T.curFg : (disabled ? dlg_dim() : T.dlgFg), bg = hl ? T.curBg : T.dlgBg;
        if (rows[i].action) draw_text("[ " + rows[i].label + " ]", x + 40, ry + 26, fg, bg);
        else {
            draw_text(fit(rows[i].label, 41), x + 40, ry + 26, fg, bg);
            draw_text(fit(rows[i].value, (w - 100 - labelW) / cell_w()), x + 40 + labelW, ry + 26, fg, bg);
        }
        ry += rowH;
    }
    if (!g_sf.status.empty()) draw_text(fit(g_sf.status, (w - 80) / cell_w()), x + 40, ry + 26, g_sf.statusErr ? (T.modern ? T.warn : Color{ 150, 0, 0 }) : dlg_dim(), T.dlgBg);
    dlg_hint(x, y, w, h, "{updown} " + tr("hint_nav", "move") + "    {cross} " + tr("hint_do", "do") + "    {circle} " + tr("hint_back", "back"));
}

static void draw_connecting()
{
    fill(0, 0, FRAME_WIDTH, FRAME_HEIGHT, T.bg);
    draw_text("PS4 Commander", MARGIN_X, TOP_BASE, T.dir, T.bg);
    std::string s;
    if (g_svc == S_FAILED) s = tr("svc_failed", "Service not responding. ") + g_svcErr;
    else s = g_svcStage == 0 ? tr("svc_checking", "Looking for the ps4commander service...") : tr("svc_starting", "Starting the service via BinLoader...");
    put_center(FRAME_WIDTH / 2, s, FRAME_HEIGHT / 2, g_svc == S_FAILED ? T.warn : T.dim, T.bg);
    if (g_svc == S_FAILED) put_center_rich(FRAME_WIDTH / 2, "{cross} " + tr("hint_retry", "try again") + "     {circle} " + tr("appExit", "Exit"), FRAME_HEIGHT / 2 + 50, T.dim, T.bg);
    put_center(FRAME_WIDTH / 2, "ps4commander v" APP_VERSION " \xC2\xB7 Created by SergioPoverony and Mr.Claude", CREDIT_Y, T.dimmer, T.bg);
}

static void draw_all()
{
    if (g_svc != S_READY) { draw_connecting(); return; }
    fill(0, 0, FRAME_WIDTH, FRAME_HEIGHT, T.bg);
    draw_topbar();
    int halfW = (FRAME_WIDTH - 2 * MARGIN_X - 16) / 2;
    draw_panel_box(0, MARGIN_X, halfW);
    draw_panel_box(1, MARGIN_X + halfW + 16, halfW);
    draw_selname();
    draw_bar();
    draw_hints_credit();

    switch (g_focus) {
    case F_MENU: draw_menu(); break;
    case F_CTX: draw_ctxmenu(); break;
    case F_CONFIRM: draw_confirm(); break;
    case F_TEXT: draw_text_view(); break;
    case F_PROGRESS: draw_progress(); break;
    case F_PKGQ: draw_pkgq(); break;
    case F_DONATE: draw_donate(); break;
    case F_IMAGE: draw_image(); break;
    case F_AUDIO: draw_audio(); break;
    case F_VIDEO: draw_video(); break;
    case F_PICK: draw_pick(); break;
    case F_SMBLIST: draw_smb_list(); break;
    case F_SMBEDIT: draw_smb_edit(); break;
    default: break;
    }
}

// ---------------------------------------------------------------- геймпад и системная клавиатура

static int g_pad = -1;
static bool g_imeReady = false;
static bool pad_init()
{
    if (scePadInit() != 0) return false;
    OrbisUserServiceInitializeParams param;
    param.priority = ORBIS_KERNEL_PRIO_FIFO_LOWEST;
    sceUserServiceInitialize(&param);
    int userID = 0;
    sceUserServiceGetInitialUser(&userID);
    g_userId = userID;
    g_pad = scePadOpen(userID, 0, 0, NULL);
    return g_pad >= 0;
}

static unsigned pad_read()
{
    if (g_pad < 0) return 0;
    OrbisPadData data;
    memset(&data, 0, sizeof(data));
    scePadReadState(g_pad, &data);
    return (unsigned)data.buttons;
}

static bool ime_text(const std::string& title, const std::string& initial, std::string& out)
{
    if (!g_imeReady) { logf_("ui: ime dialog not ready, skip keyboard"); return false; }

    uint16_t buf[256];
    uint16_t wtitle[64];
    memset(buf, 0, sizeof(buf));
    memset(wtitle, 0, sizeof(wtitle));
    utf8_to_utf16(initial, buf, 256);
    utf8_to_utf16(title, wtitle, 64);

    OrbisImeDialogSetting setting;
    memset(&setting, 0, sizeof(setting));
    setting.userId = (uint32_t)g_userId;
    setting.type = ORBIS_TYPE_DEFAULT;
    setting.supportedLanguages = 0;
    setting.enterLabel = ORBIS_BUTTON_LABEL_DEFAULT;
    setting.inputMethod = ORBIS__DEFAULT;
    setting.filter = NULL;
    setting.option = 0;
    setting.maxTextLength = 255;
    setting.inputTextBuffer = (wchar_t*)buf;
    setting.posx = (float)(FRAME_WIDTH / 2);
    setting.posy = (float)(FRAME_HEIGHT / 2);
    setting.horizontalAlignment = ORBIS_H_CENTER;
    setting.verticalAlignment = ORBIS_V_CENTER;
    setting.placeholder = NULL;
    setting.title = (const wchar_t*)wtitle;

    if (sceImeDialogInit(&setting, NULL) != 0) { logf_("ui: sceImeDialogInit failed"); return false; }

    OrbisDialogStatus st;
    while ((st = sceImeDialogGetStatus()) == ORBIS_DIALOG_STATUS_RUNNING) {
        gfx_present();
        usleep(16000);
    }

    bool ok = false;
    if (st == ORBIS_DIALOG_STATUS_STOPPED) {
        OrbisDialogResult res;
        memset(&res, 0, sizeof(res));
        if (sceImeDialogGetResult(&res) == 0 && res.endstatus == ORBIS_DIALOG_OK) { out = utf16_to_utf8(buf, 256); ok = true; }
    }
    sceImeDialogTerm();
    redraw();
    return ok;
}

// ---------------------------------------------------------------- подключение к службе (в фоне)

struct SvcRes { bool ok = false; std::string err, l, r, theme; };

static void svc_worker(SvcRes& res)
{
    std::vector<RootView> roots; std::string ver;
    if (service_roots(roots, ver) && ver == APP_VERSION) { res.ok = true; }
    else {
        g_svcStage = 1;
        logf_("svc: service missing or version differs (service=%s app=%s), sending payload", ver.c_str(), APP_VERSION);
        std::string err; bool connFailed = false;
        if (!service_send_payload(err, &connFailed)) { res.err = connFailed ? "noloader" : err; return; }
        service_install_autorun(err);
        bool ok = false;
        for (int i = 0; i < 25 && !ok; i++) {
            usleep(400000);
            if (service_roots(roots, ver) && ver == APP_VERSION) ok = true;
        }
        if (!ok) { res.err = "timeout"; return; }
        res.ok = true;
    }
    service_state_get(res.l, res.r, res.theme);
}

static void svc_begin()
{
    g_svc = S_CHECK; g_svcStage = 0; g_svcErr.clear();
    redraw();
    auto res = std::make_shared<SvcRes>();
    async_run(ASYNC_ACT, [res]() { svc_worker(*res); },
        [res]() {
            redraw();
            if (!res->ok) {
                g_svc = S_FAILED;
                g_svcErr = res->err == "noloader" ? tr("svc_noloader", "BinLoader is off (GoldHEN - Servers - Enable Payloader Server).")
                         : res->err == "timeout" ? tr("svc_timeout", "No answer from the service.") : res->err;
                return;
            }
            gfx_set_theme(res->theme);
            g_svc = S_READY;
            g_panels[0].reqId = 0; g_panels[1].reqId = 0;
            panel_load(0, res->l.empty() ? "/data" : res->l, "");
            panel_load(1, res->r.empty() ? "/data" : res->r, "");
            g_stateReady = true;
        });
}

// ---------------------------------------------------------------- инициализация и цикл

bool ui_init()
{
    if (!gfx_init()) return false;
    async_init();

    bool padOk = pad_init();

    bool imeMod = sceSysmoduleLoadModule(ORBIS_SYSMODULE_IME_DIALOG) == 0;
    bool cdMod = sceSysmoduleLoadModuleInternal(ORBIS_SYSMODULE_INTERNAL_COMMON_DIALOG) == 0;
    if (cdMod) cdMod = sceCommonDialogInitialize() == 0;
    g_imeReady = imeMod && cdMod;
    logf_("ui: ime modules ime=%d commonDialog=%d", imeMod ? 1 : 0, cdMod ? 1 : 0);
    logf_("ui: ready, cell width %d, pad %s", cell_w(), padOk ? "ok" : "FAILED");
    g_ready = true;
    svc_begin();
    return true;
}

// Автоповтор удерживаемой кнопки (стрелки): первое срабатывание сразу, затем после паузы - часто.
struct Rep { bool held = false; uint64_t since = 0, last = 0; };
static bool rep_step(Rep& r, bool down, uint64_t now)
{
    if (!down) { r.held = false; return false; }
    if (!r.held) { r.held = true; r.since = now; r.last = now; return true; }
    if (now - r.since < 380000ULL || now - r.last < 55000ULL) return false;
    r.last = now;
    return true;
}

static int wrap_idx(int v, int n) { return n <= 0 ? 0 : ((v % n) + n) % n; }

static void poll_progress(uint64_t now)
{
    bool job = g_focus == F_PROGRESS, pkg = g_focus == F_PKGQ;
    if (!job && !pkg) {
        // в фоне раз в 3 секунды узнаём, идёт ли установка PKG (в т.ч. запущенная из веб-версии)
        static uint64_t nextBg = 0;
        if (g_svc != S_READY || g_pollBusy || now < nextBg || (g_focus != F_PANEL && g_focus != F_CTX && g_focus != F_BAR)) return;
        nextBg = now + 3000000ULL;
        g_pollBusy = true;
        auto q = std::make_shared<PkgQueueView>();
        async_run(ASYNC_POLL, [q]() { service_pkg_state(*q); },
            [q]() { g_pollBusy = false; pkg_note_active(*q); if (q->active != g_pkgBg) { g_pkgBg = q->active; redraw(); } });
        return;
    }
    if (g_pollBusy || now < g_pollAt) return;
    g_pollBusy = true;
    g_pollAt = now + 300000ULL;
    auto jv = std::make_shared<JobView>();
    auto qv = std::make_shared<PkgQueueView>();
    async_run(ASYNC_POLL, [job, jv, qv]() { if (job) service_job_state(*jv); else service_pkg_state(*qv); },
        [job, jv, qv]() {
            g_pollBusy = false;
            redraw();
            if (job && g_focus == F_PROGRESS) {
                g_job = *jv;
                bool running = jv->state == "running" || jv->state == "scan";
                // первые 400 мс не закрываем: в ответе может быть состояние предыдущего задания
                if (!running && now_us() - g_progStart > 400000ULL && jv->ok) {
                    focus_pop();
                    refresh_all();
                    if (jv->errTotal > 0) text_open(tr("errorsTitle", "Errors"), tr("errorsTitle", "Errors") + ": " + std::to_string(jv->errTotal), true);
                }
            } else if (!job && g_focus == F_PKGQ) {
                g_pkgq = *qv;
                g_pkgBg = qv->active;
                pkg_note_active(*qv);
            }
        });
}

void ui_pump()
{
    if (!g_ready) return;
    uint64_t now = now_us();
    if (async_pump()) redraw();

    unsigned b = pad_read();
    static unsigned prev = 0;
    static Rep rUp, rDown, rLeft, rRight;
    unsigned pressed = b & ~prev;
    prev = b;
    bool cross = (pressed & ORBIS_PAD_BUTTON_CROSS) != 0;
    bool circle = (pressed & ORBIS_PAD_BUTTON_CIRCLE) != 0;
    bool square = (pressed & ORBIS_PAD_BUTTON_SQUARE) != 0;
    bool triangle = (pressed & ORBIS_PAD_BUTTON_TRIANGLE) != 0;
    bool options = (pressed & ORBIS_PAD_BUTTON_OPTIONS) != 0;
    bool l1 = (pressed & ORBIS_PAD_BUTTON_L1) != 0;
    bool r1 = (pressed & ORBIS_PAD_BUTTON_R1) != 0;
    bool l2 = (pressed & ORBIS_PAD_BUTTON_L2) != 0;
    bool r2 = (pressed & ORBIS_PAD_BUTTON_R2) != 0;
    bool up = rep_step(rUp, (b & ORBIS_PAD_BUTTON_UP) != 0, now);
    bool down = rep_step(rDown, (b & ORBIS_PAD_BUTTON_DOWN) != 0, now);
    bool left = rep_step(rLeft, (b & ORBIS_PAD_BUTTON_LEFT) != 0, now);
    bool right = rep_step(rRight, (b & ORBIS_PAD_BUTTON_RIGHT) != 0, now);
    if (cross || circle || square || triangle || options || l1 || r1 || l2 || r2 || up || down || left || right) redraw();

    // отложенное сохранение путей панелей в службе
    if (g_stateAt && now >= g_stateAt && g_svc == S_READY) {
        g_stateAt = 0;
        std::string l = g_panels[0].path, r = g_panels[1].path;
        async_run(ASYNC_POLL, [l, r]() { service_state_set_paths(l, r); }, NULL);
    }
    smb_poll_step(now);
    poll_progress(now);
    // индикатор загрузки и прогресс обновляем и без нажатий
    static uint64_t lastTick = 0;
    if (now - lastTick >= 150000ULL && (g_busy > 0 || g_panels[0].loading || g_panels[1].loading || g_svc != S_READY || g_focus == F_AUDIO || g_focus == F_VIDEO || (g_focus == F_IMAGE && g_iv.loading))) { lastTick = now; redraw(); }

    if (g_focus == F_VIDEO && video_poll_frame()) redraw();

    if (g_svc != S_READY) {
        if (g_svc == S_FAILED) { if (cross) svc_begin(); else if (circle) app_exit(); }
        goto present;
    }

    switch (g_focus) {
    case F_PROGRESS:
        if (cross) act_async([](std::string&) { service_job_cancel(); return true; }, NULL);
        break;
    case F_PKGQ:
        if (cross) act_async([](std::string&) { service_pkg_cancel(); return true; }, NULL);
        else if (circle) focus_pop();
        break;
    case F_DONATE:
        if (circle || cross) focus_pop();
        break;
    case F_IMAGE: {
        int n = (int)g_iv.list.size();
        if (circle || triangle) { g_iv.req++; g_iv.pic = Picture(); focus_pop(); break; }
        if ((right || r1 || r2 || cross) && g_iv.idx + 1 < n) img_load(g_iv.idx + 1);
        else if ((left || l1 || l2) && g_iv.idx > 0) img_load(g_iv.idx - 1);
        break;
    }
    case F_VIDEO:
        if (cross) video_pause_toggle();
        else if (circle) { video_close(); focus_pop(); }
        else if (left) video_seek(-10000);
        else if (right) video_seek(10000);
        else if (l2) video_seek(-60000);
        else if (r2) video_seek(60000);
        break;
    case F_AUDIO:
        if (left) audio_seek(-10000);
        else if (right) audio_seek(10000);
        else if (l2) audio_seek(-60000);
        else if (r2) audio_seek(60000);
        else if (cross) audio_pause_toggle();
        else if (circle) { audio_stop(); focus_pop(); }
        break;
    case F_TEXT:
        if (circle || cross) { focus_pop(); break; }
        if (up) g_tv.scroll--;
        if (down) g_tv.scroll++;
        if (l2 || left) g_tv.scroll -= 20;
        if (r2 || right) g_tv.scroll += 20;
        break;
    case F_CONFIRM:
        if (cross) { auto f = g_cf.onYes; focus_pop(); if (f) f(); }
        else if (circle) focus_pop();
        break;
    case F_PICK: {
        int n = (int)g_pick.items.size();
        if (up) g_pick.cur = wrap_idx(g_pick.cur - 1, n);
        if (down) g_pick.cur = wrap_idx(g_pick.cur + 1, n);
        if (left || l2) g_pick.cur = std::max(0, g_pick.cur - 10);
        if (right || r2) g_pick.cur = std::min(n - 1, g_pick.cur + 10);
        if (cross) { auto cb = g_pick.onPick; int i = g_pick.cur; focus_pop(); if (cb) cb(i); }
        else if (circle) focus_pop();
        break;
    }
    case F_CTX: {
        Entry2* e = panel_cur(g_panels[g_active]);
        int n = ctx_count(e && is_pkg(*e));
        if (up) g_ctxCursor = wrap_idx(g_ctxCursor - 1, n);
        if (down) g_ctxCursor = wrap_idx(g_ctxCursor + 1, n);
        if (cross) ctx_run(g_ctxCursor);
        else if (circle || triangle) focus_pop();
        break;
    }
    case F_MENU:
        if (up) g_menuCursor = wrap_idx(g_menuCursor - 1, M_COUNT);
        if (down) g_menuCursor = wrap_idx(g_menuCursor + 1, M_COUNT);
        if ((left || right) && (g_menuCursor == M_THEME)) cycle_theme(right ? 1 : -1);
        else if ((left || right) && g_menuCursor == M_LANG) cycle_lang();
        if (cross) menu_run(MENU[g_menuCursor].id);
        else if (circle || options) focus_pop();
        break;
    case F_SMBLIST: {
        int n = (int)g_smbList.size() + 1;
        if (up) g_smbCur = wrap_idx(g_smbCur - 1, n);
        if (down) g_smbCur = wrap_idx(g_smbCur + 1, n);
        if (circle) { focus_pop(); break; }
        if (cross) {
            if (g_smbCur == 0) smb_edit_open(NULL);
            else { std::string alias = g_smbList[g_smbCur - 1].alias; focus_reset(); panel_load(g_active, "/smb/" + alias, ""); }
        } else if (square && g_smbCur > 0) { SmbConnView c = g_smbList[g_smbCur - 1]; smb_edit_open(&c); }
        else if (triangle && g_smbCur > 0) { SmbConnView c = g_smbList[g_smbCur - 1]; smb_delete_conn(c); }
        break;
    }
    case F_SMBEDIT:
        if (up) g_sf.cur = wrap_idx(g_sf.cur - 1, 10);
        if (down) g_sf.cur = wrap_idx(g_sf.cur + 1, 10);
        if (cross) smb_form_activate(g_sf.cur);
        else if (circle) { g_sf.scanning = false; focus_pop(); }
        break;
    case F_BAR:
        if (l2 || left) g_barCursor = wrap_idx(g_barCursor - 1, 10);
        else if (r2 || right) g_barCursor = wrap_idx(g_barCursor + 1, 10);
        else if (l1 || r1) { g_active = 1 - g_active; g_focus = F_PANEL; }
        else if (options) open_menu();
        else if (circle || up) g_focus = F_PANEL;
        else if (cross) { g_focus = F_PANEL; run_bar_action(g_barCursor); }
        break;
    case F_PANEL:
    default: {
        Panel& p = g_panels[g_active];
        int n = (int)p.items.size();
        int rows = (PANEL_H - HEAD_H - COLH_H - FOOT_H) / ROW_H;
        if (options) { open_menu(); break; }
        if (l1 || r1) { g_active = 1 - g_active; break; }
        if (l2 || r2) { g_focus = F_BAR; break; }
        if (up && n) p.cur = wrap_idx(p.cur - 1, n);
        else if (down && n) p.cur = wrap_idx(p.cur + 1, n);
        else if (left && n) p.cur = std::max(0, p.cur - (rows - 1));
        else if (right && n) p.cur = std::min(n - 1, p.cur + (rows - 1));
        else if (cross) panel_enter(g_active);
        else if (circle) panel_up(g_active);
        else if (square) toggle_mark(p, true);
        else if (triangle) { Entry2* e = panel_cur(p); if (e && e->name != "..") { g_ctxCursor = 0; focus_push(F_CTX); } }
        break;
    }
    }

present:
    if (g_redraw > 0) {
        draw_all();
        gfx_present();
        g_redraw--;
        static bool hidden = false;
        if (!hidden) { hidden = true; int r = sceSystemServiceHideSplashScreen(); logf_("ui: HideSplashScreen = 0x%08X", (unsigned)r); }
    }
}
