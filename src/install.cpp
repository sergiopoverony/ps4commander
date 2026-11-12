#include "install.h"
#include "netfs.h"
#include "fs.h"
#include "vfs.h"
#include "json.h"
#include "log.h"
#include "platform.h"
#include "smb.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <deque>
#include <mutex>
#include <sys/mman.h>
#include <cerrno>
#include <dirent.h>
#include <sys/statvfs.h>
#include <sys/stat.h>
#include <thread>
#include <strings.h>
#include <unistd.h>

namespace {

struct Item {
    std::string path, name, cid;
    std::string status = "queued";  // queued | registering | installing | done | already_installed | failed | cancelled
    std::string msg;
    std::string src;        // явный источник для системы: http-адрес или путь (пусто - сам файл)
    std::string stage;      // временная локальная копия pkg с сетевой папки
    uint64_t total = 0, done = 0;
};

struct BgftParam {  // sizeof == 0x68
    int userId;
    int entitlementType;
    const char* id;
    const char* contentUrl;
    const char* contentExUrl;
    const char* contentName;
    const char* iconPath;
    const char* skuId;
    uint32_t option;
    const char* playgoScenarioId;
    const char* releaseDate;
    const char* packageType;
    const char* packageSubType;
    unsigned long packageSize;
};
struct BgftParamEx { BgftParam params; unsigned int slot; };
struct BgftProgress {
    unsigned int bits; int errorResult;
    unsigned long length, transferred, lengthTotal, transferredTotal;
    unsigned int numIndex, numTotal, restSec, restSecTotal;
    int preparingPercent, localCopyPercent;
};
struct BgftInit { void* heap; size_t heapSize; };
static_assert(sizeof(BgftParam) == 0x68, "BgftParam");

std::mutex g_mu;
std::condition_variable g_cv;
std::deque<Item> g_items;  // все элементы текущей и прошлых пачек (ограничено)
std::thread g_thr;
bool g_thr_started = false;
std::atomic<bool> g_stop{false}, g_cancel{false};
std::string g_bgft_info;

const size_t kMaxItems = 200;
#ifdef HOST_TEST
const int HOST_SLEEP_MS = 50, STALL_TICKS = 100;
#else
const int HOST_SLEEP_MS = 1000, STALL_TICKS = 180;
#endif

bool is_active(const Item& i) { return i.status == "queued" || i.status == "registering" || i.status == "copying" || i.status == "installing"; }
bool any_active() { for (auto& i : g_items) if (is_active(i)) return true; return false; }

// путь для BGFT: /data/... -> /user/data/...
[[maybe_unused]] std::string bgft_path(const std::string& p) { return p.compare(0, 6, "/data/") == 0 ? "/user" + p : p; }

bool read_header(const std::string& path, std::string& cid, uint64_t& size, std::string& err) {
    VFile f;
    int oe = v_open_read(path, f);
    if (oe) { err = fs_errname(oe); return false; }
    unsigned char h[0x64];
    size_t n = 0;
    while (n < sizeof(h)) {
        long r = v_read(f, h + n, sizeof(h) - n);
        if (r <= 0) break;
        n += (size_t)r;
    }
    v_close(f);
    if (n < sizeof(h) || h[0] != 0x7F || h[1] != 'C' || h[2] != 'N' || h[3] != 'T') { err = "not_pkg"; return false; }
    cid.assign((const char*)h + 0x40, 36);
    for (char c : cid) if (!((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' || c == '_')) { err = "not_pkg"; return false; }
    VStat st;
    size = v_lstat(path, st) == 0 ? st.size : 0;
    return true;
}

// Адрес для BGFT: локальные файлы по пути, файлы с сетевых папок - через наш же веб-сервер (он читает SMB и умеет Range).
[[maybe_unused]] std::string pct(const std::string& s) {
    static const char* hex = "0123456789ABCDEF";
    std::string o;
    for (unsigned char c : s) {
        if (isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~' || c == '/') o += (char)c;
        else { o += '%'; o += hex[c >> 4]; o += hex[c & 15]; }
    }
    return o;
}
// Система хранит путь к pkg в буферах по 96 байт: более длинный обрезается, и пакет ищется под несуществующим именем.
// Поэтому для длинных путей отдаём ей короткую символическую ссылку (как делает PS4Mount).
const size_t kSysPathMax = 95;
std::string source_url(const Item& it) {
    if (it.src.compare(0, 7, "http://") == 0) return it.src;
    std::string p = bgft_path(!it.src.empty() ? it.src : it.stage.empty() ? it.path : it.stage);
    if (p.size() <= kSysPathMax) return p;
    std::string dir = std::string(DATA_DIR) + "/links";
    mkdir(dir.c_str(), 0777);
    std::string link = dir + "/" + it.cid + ".pkg";
    unlink(link.c_str());
    if (symlink(p.c_str(), link.c_str()) != 0) {
        app_log("BGFT: не удалось создать короткую ссылку %s (%s), путь длиной %d может быть обрезан системой", link.c_str(), strerror(errno), (int)p.size());
        return p;
    }
    std::string url = bgft_path(link);
    app_log("BGFT: путь длиной %d заменён короткой ссылкой %s", (int)p.size(), url.c_str());
    return url;
}

// ---- временная копия pkg с сетевой папки ----
// Системный установщик читает файл только по локальному пути (адрес http://127.0.0.1 он отклоняет: 0x8099006A),
// поэтому пакет с шары сначала копируется на диск и после установки удаляется.
const uint64_t kUsbMargin = 64ull << 20, kInternalMargin = 256ull << 20;

#ifndef HOST_TEST
bool is_mount(const std::string& dir) {
    struct stat a, b;
    return stat(dir.c_str(), &a) == 0 && stat("/mnt", &b) == 0 && a.st_dev != b.st_dev;
}
#endif
uint64_t free_bytes(const std::string& dir) {
    struct statvfs v;
    if (statvfs(dir.c_str(), &v) != 0) return 0;
    return (uint64_t)v.f_bavail * (uint64_t)v.f_frsize;
}
std::string internal_stage_dir() { return std::string(DATA_DIR) + "/stage"; }

// Куда положить копию: сначала подключённая флешка (место на внутреннем диске нужно для самой установки), иначе внутренний диск.
bool pick_stage_dir(uint64_t size, std::string& dir) {
#ifndef HOST_TEST
    for (int i = 0; i < 8; i++) {
        std::string m = "/mnt/usb" + std::to_string(i);
        if (is_mount(m) && free_bytes(m) >= size + kUsbMargin) { dir = m + "/ps4commander_tmp"; return true; }
    }
#endif
    if (free_bytes(DATA_DIR) >= 2 * size + kInternalMargin) { dir = internal_stage_dir(); return true; }
    return false;
}

void remove_dir_files(const std::string& dir) {
    DIR* d = opendir(dir.c_str());
    if (!d) return;
    while (struct dirent* e = readdir(d)) {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
        unlink((dir + "/" + e->d_name).c_str());
    }
    closedir(d);
    rmdir(dir.c_str());
}
// Остатки от прошлого запуска (копии, которые не успели удалить).
void cleanup_stale_stage() {
    remove_dir_files(internal_stage_dir());
    for (int i = 0; i < 8; i++) remove_dir_files("/mnt/usb" + std::to_string(i) + "/ps4commander_tmp");
}

void stage_remove(Item* it) {
    if (it->stage.empty()) return;
    unlink(it->stage.c_str());
    std::string dir = fs_parent(it->stage);
    rmdir(dir.c_str());   // удалится, только если пустой
    std::lock_guard<std::mutex> l(g_mu);
    it->stage.clear();
}

// Копирует pkg с шары. false - не удалось или отмена (it->status уже выставлен).
bool stage_in(Item* it);

#ifdef HOST_TEST
// Заглушки: имитация BGFT для проверок на ПК.
bool bgft_load() { g_bgft_info = "host-test stub"; return true; }
int bgft_register(const Item& it, int* task) {
    app_log("BGFT: источник %s", source_url(it).c_str());
    if (it.path.find("busy") != std::string::npos) return (int)0x80990015;
    if (it.path.find("dup") != std::string::npos) return (int)0x80990088;
    if (it.path.find("fail") != std::string::npos) return (int)0x80990099;
    *task = 1; return 0;
}
int bgft_start(int) { return 0; }
int bgft_progress(int, uint64_t total, uint64_t* done, int* err) {
    static uint64_t d = 0;
    d += total / 4 + 1; if (d > total) d = total;
    *done = d; *err = 0;
    bool fin = d >= total;
    if (fin) d = 0;
    return fin ? 1 : 0;
}
#else
extern "C" {
int sceKernelLoadStartModule(const char* name, size_t argc, const void* argv, unsigned flags, void* opt, int* res);
int sceKernelDlsym(int handle, const char* name, void** addr);
}
typedef int (*init_fn)(BgftInit*);
typedef int (*reg_fn)(BgftParamEx*, int*);
typedef int (*start_fn)(int);
typedef int (*prog_fn)(int, BgftProgress*);
init_fn g_init; reg_fn g_reg; start_fn g_start; prog_fn g_prog;
bool g_loaded = false, g_ok = false;

void* find_sym(int h, const char* a, const char* b) {
    void* p = nullptr;
    if (sceKernelDlsym(h, a, &p) == 0 && p) return p;
    p = nullptr;
    if (b && sceKernelDlsym(h, b, &p) == 0 && p) return p;
    return nullptr;
}

bool bgft_load() {
    if (g_loaded) return g_ok;
    g_loaded = true;
    static const char* dirs[] = {"/system/common/lib/", "/system/priv/lib/", "/system_ex/common_ex/lib/", "/system_ex/priv_ex/lib/"};
    int h = -1;
    for (const char* d : dirs) {
        std::string p = std::string(d) + "libSceBgft.sprx";
        int r = sceKernelLoadStartModule(p.c_str(), 0, nullptr, 0, nullptr, nullptr);
        if (r > 0) { h = r; break; }
        if (r != (int)0x80020002 && r < 0 && h < 0) app_log("BGFT: %s -> 0x%x", p.c_str(), (unsigned)r);
    }
    if (h <= 0) { app_log("BGFT: libSceBgft.sprx не загружена"); return false; }
    g_init = (init_fn)find_sym(h, "sceBgftServiceIntInit", "sceBgftInitialize");
    g_reg = (reg_fn)find_sym(h, "sceBgftServiceIntDownloadRegisterTaskByStorageEx", "sceBgftDownloadRegisterTaskByStorageEx");
    g_start = (start_fn)find_sym(h, "sceBgftServiceDownloadStartTask", "sceBgftDownloadStartTask");
    g_prog = (prog_fn)find_sym(h, "sceBgftServiceDownloadGetProgress", "sceBgftDownloadGetProgress");
    app_log("BGFT: init=%d reg=%d start=%d progress=%d", !!g_init, !!g_reg, !!g_start, !!g_prog);
    if (!g_init || !g_reg || !g_start || !g_prog) return false;
    // метка на случай падения init: следующий запуск этой версии пропустит init
    std::string marker = std::string(DATA_DIR) + "/bgft_init.marker";
    if (access(marker.c_str(), F_OK) == 0) {
        app_log("BGFT: init пропущен (метка прошлого падения)");
    } else {
        FILE* m = fopen(marker.c_str(), "w"); if (m) { fputs("1", m); fclose(m); }
        void* heap = mmap(nullptr, 1 << 20, PROT_READ | PROT_WRITE, MAP_ANON | MAP_PRIVATE, -1, 0);
        if (heap == MAP_FAILED) { unlink(marker.c_str()); app_log("BGFT: mmap не удался"); return false; }
        memset(heap, 0, 1 << 20);
        BgftInit ip = {heap, 1 << 20};
        int r = g_init(&ip);
        unlink(marker.c_str());
        app_log("BGFT: init -> 0x%x", (unsigned)r);
        if (r != 0 && (unsigned)r != 0x80990001u) return false;
    }
    g_ok = true;
    g_bgft_info = "ok";
    return true;
}

int bgft_register(const Item& it, int* task) {
    std::string url = source_url(it);
    app_log("BGFT: источник %s (%d симв.)", url.c_str(), (int)url.size());
    BgftParamEx pe;
    memset(&pe, 0, sizeof(pe));
    pe.params.entitlementType = 5;
    pe.params.id = it.cid.c_str();
    pe.params.contentUrl = url.c_str();
    pe.params.contentName = it.cid.c_str();
    pe.params.iconPath = "";
    pe.params.playgoScenarioId = "0";
    pe.params.option = 0x8;
    pe.slot = 0;
    return g_reg(&pe, task);
}
int bgft_start(int task) { return g_start(task); }
int bgft_progress(int task, uint64_t, uint64_t* done, int* err) {
    BgftProgress p;
    memset(&p, 0, sizeof(p));
    int r = g_prog(task, &p);
    if (r != 0) { *err = r; return r; }
    *err = p.errorResult;
    *done = p.transferred;
    return p.length > 0 && p.transferred >= p.length ? 1 : 0;
}
#endif

void set(Item* it, const char* st, const std::string& msg = "") {
    std::lock_guard<std::mutex> l(g_mu);
    it->status = st;
    it->msg = msg;
}
char hexbuf[32];
std::string hx(int v) { snprintf(hexbuf, sizeof(hexbuf), "0x%08X", (unsigned)v); return hexbuf; }

bool stage_in(Item* it) {
    std::string dir;
    if (!pick_stage_dir(it->total, dir)) { set(it, "failed", "no_space"); app_log("установка %s: нет места для временной копии (%lld байт)", it->name.c_str(), (long long)it->total); return false; }
    mkdir(fs_parent(dir).c_str(), 0777);
    mkdir(dir.c_str(), 0777);
    std::string dst = dir + "/" + it->cid + ".pkg";
    set(it, "copying");
    { std::lock_guard<std::mutex> l(g_mu); it->done = 0; it->stage = dst; }
    app_log("установка %s: копирование с сетевой папки в %s", it->name.c_str(), dst.c_str());
    VFile in, out;
    int e = v_open_read(it->path, in);
    if (e) { set(it, "failed", fs_errname(e)); stage_remove(it); return false; }
    e = v_open_write(dst, out, 0666);
    if (e) { v_close(in); set(it, "failed", fs_errname(e)); stage_remove(it); return false; }
    std::vector<char> buf(1 << 20);
    uint64_t total = 0;
    bool ok = true;
    int tk = 0;
    for (;;) {
        if (g_cancel || g_stop) { ok = false; set(it, "cancelled"); break; }
        long r = v_read(in, buf.data(), buf.size());
        if (r < 0) { set(it, "failed", fs_errname((int)-r)); ok = false; break; }
        if (r == 0) break;
        size_t off = 0;
        while (off < (size_t)r) {
            long w = v_write(out, buf.data() + off, (size_t)r - off);
            if (w <= 0) { set(it, "failed", fs_errname(w < 0 ? (int)-w : EIO)); ok = false; break; }
            off += (size_t)w;
        }
        if (!ok) break;
        total += (uint64_t)r;
        { std::lock_guard<std::mutex> l(g_mu); it->done = total; }
        if (++tk % 8 == 0) keep_awake_tick();
    }
    v_close(in);
    v_close(out);
    if (ok && it->total && total != it->total) { set(it, "failed", "short_copy"); ok = false; }
    if (!ok) { stage_remove(it); return false; }
    { std::lock_guard<std::mutex> l(g_mu); it->done = 0; }
    return true;
}

// Способы отдать pkg с шары системному установщику (по порядку; отклонённый система способ больше не пробуем):
//  1) папка FUSE (на флешке, а без неё в /data/ps4commander/mnt) - без копирования, 2) временная копия на диск.
// (Адрес http://<консоль>:8484 система отклоняет - 0x8099006A, на консоли проверено, поэтому такого способа нет.)
enum { M_FUSE = 2 };
std::atomic<int> g_bad{0};
const int kSourceRefused = 1;

int run_item_inner(Item* it, bool final_try);
void run_item(Item* it) {
    if (!smb_is_path(it->path)) { run_item_inner(it, true); return; }
    auto clear_src = [&] { std::lock_guard<std::mutex> l(g_mu); it->src.clear(); };
    if (g_cancel || g_stop) { set(it, "cancelled"); return; }
    if (!(g_bad & M_FUSE)) {
        std::string root, e;
        if (netfs_root(root, e)) {
            { std::lock_guard<std::mutex> l(g_mu); it->src = netfs_map(root, it->path); }
            app_log("установка %s: способ 1 - папка FUSE %s", it->name.c_str(), it->src.c_str());
            int r = run_item_inner(it, false);
            clear_src();
            if (r != kSourceRefused) return;
            g_bad |= M_FUSE;
            app_log("установка %s: система не приняла папку FUSE", it->name.c_str());
        } else {
            app_log("установка %s: FUSE недоступен (%s)", it->name.c_str(), e.c_str());
            if (e != "no_usb" && e != "reload") g_bad |= M_FUSE;
        }
    }
    if (g_cancel || g_stop) { set(it, "cancelled"); return; }
    app_log("установка %s: способ 2 - временная копия на диск", it->name.c_str());
    if (!stage_in(it)) return;
    run_item_inner(it, true);
    stage_remove(it);
}

int run_item_inner(Item* it, bool final_try) {
    int task = 0, r = 0;
    set(it, "registering");
    for (int k = 0; k < 30; k++) {
        r = bgft_register(*it, &task);
        if ((unsigned)r != 0x80990015u || g_cancel || g_stop) break;
        for (int s = 0; s < 3 && !g_cancel && !g_stop; s++) std::this_thread::sleep_for(std::chrono::milliseconds(HOST_SLEEP_MS));
    }
    if (g_cancel || g_stop) { set(it, "cancelled"); return 0; }
    if ((unsigned)r == 0x80990088u) { set(it, "already_installed"); return 0; }
    if ((unsigned)r == 0x8099006Au && !final_try) { app_log("установка %s: регистрация %s (источник отклонён)", it->name.c_str(), hx(r).c_str()); return kSourceRefused; }
    if (r != 0) { set(it, "failed", "register " + hx(r)); app_log("установка %s: регистрация %s", it->name.c_str(), hx(r).c_str()); return 0; }
    r = bgft_start(task);
    if (r != 0) { set(it, "failed", "start " + hx(r)); return 0; }
    set(it, "installing");
    uint64_t last = 0;
    int stall = 0;
    while (!g_cancel && !g_stop) {
        std::this_thread::sleep_for(std::chrono::milliseconds(HOST_SLEEP_MS));
        keep_awake_tick();
        uint64_t d = 0; int err = 0;
        int fin = bgft_progress(task, it->total, &d, &err);
        if (err != 0) { set(it, "failed", hx(err)); app_log("установка %s: ошибка %s", it->name.c_str(), hx(err).c_str()); notify("ps4commander: ошибка установки %s %s", hx(err).c_str(), it->cid.c_str()); return 0; }
        { std::lock_guard<std::mutex> l(g_mu); it->done = d; }
        if (fin > 0) { set(it, "done"); return 0; }
        if (d != last) { last = d; stall = 0; } else if (++stall >= STALL_TICKS) { set(it, "failed", "stalled"); return 0; }
    }
    set(it, "cancelled");
    return 0;
}

void worker() {
    for (;;) {
        Item* it = nullptr;
        {
            std::unique_lock<std::mutex> l(g_mu);
            g_cv.wait(l, [] { if (g_stop) return true; for (auto& i : g_items) if (i.status == "queued") return true; return false; });
            if (g_stop) return;
            for (auto& i : g_items) if (i.status == "queued") { it = &i; break; }
        }
        if (g_cancel) { set(it, "cancelled"); }
        else run_item(it);
        bool fin; bool bad = false; std::string last;
        {
            std::lock_guard<std::mutex> l(g_mu);
            fin = !any_active();
            if (fin) g_cancel = false;
            for (auto& i : g_items) if (i.status == "failed") bad = true;
            last = it->name;
        }
        if (fin && !bad) notify("ps4commander: установка завершена (%s)", last.c_str());
    }
}

}  // namespace

bool install_start(const std::vector<std::string>& paths, std::string& err) {
    if (paths.empty()) { err = "bad_path"; return false; }
    std::vector<Item> add;
    for (auto& p : paths) {
        if (p.size() < 5 || strcasecmp(p.c_str() + p.size() - 4, ".pkg") != 0) { err = "not_pkg"; return false; }
        std::string e = fs_check_read(p);
        if (!e.empty()) { err = e; return false; }
        Item it;
        it.path = p; it.name = fs_basename(p);
        if (!read_header(p, it.cid, it.total, err)) return false;
        add.push_back(it);
    }
    if (!bgft_load()) { err = "no_bgft"; return false; }
    std::lock_guard<std::mutex> l(g_mu);
    if (!any_active()) { while (g_items.size() + add.size() > kMaxItems && !g_items.empty()) g_items.pop_front(); }
    else if (g_items.size() + add.size() > kMaxItems) { err = "busy"; return false; }
    for (auto& i : add) g_items.push_back(i);
    if (!g_thr_started) { cleanup_stale_stage(); g_thr_started = true; g_thr = std::thread(worker); }
    g_cv.notify_all();
    return true;
}

std::string install_json() {
    std::lock_guard<std::mutex> l(g_mu);
    std::string o = "{\"active\":" + std::string(any_active() ? "true" : "false") + ",\"bgft\":\"" + json_esc(g_bgft_info) + "\",\"items\":[";
    bool first = true;
    for (auto& i : g_items) {
        if (!first) o += ",";
        first = false;
        o += "{\"n\":\"" + json_esc(i.name) + "\",\"cid\":\"" + json_esc(i.cid) + "\",\"st\":\"" + i.status + "\",\"msg\":\"" + json_esc(i.msg) +
             "\",\"total\":" + std::to_string(i.total) + ",\"done\":" + std::to_string(i.done) + "}";
    }
    return o + "]}";
}

void install_cancel() {
    std::lock_guard<std::mutex> l(g_mu);
    g_cancel = true;
    for (auto& i : g_items) if (i.status == "queued") i.status = "cancelled";
    g_cv.notify_all();
}

void install_shutdown() {
    { std::lock_guard<std::mutex> l(g_mu); g_stop = true; g_cv.notify_all(); }
    if (g_thr_started && g_thr.joinable()) g_thr.join();
}
