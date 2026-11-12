#include "install.h"
#include "fs.h"
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
#include <sys/stat.h>
#include <thread>
#include <strings.h>
#include <unistd.h>

namespace {

struct Item {
    std::string path, name, cid;
    std::string status = "queued";  // queued | registering | installing | done | already_installed | failed | cancelled
    std::string msg;
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

bool is_active(const Item& i) { return i.status == "queued" || i.status == "registering" || i.status == "installing"; }
bool any_active() { for (auto& i : g_items) if (is_active(i)) return true; return false; }

// путь для BGFT: /data/... -> /user/data/...
[[maybe_unused]] std::string bgft_path(const std::string& p) { return p.compare(0, 6, "/data/") == 0 ? "/user" + p : p; }

bool read_header(const std::string& path, std::string& cid, uint64_t& size, std::string& err) {
    FILE* f = fopen(path.c_str(), "rb");
    if (!f) { err = "not_found"; return false; }
    unsigned char h[0x64];
    size_t n = fread(h, 1, sizeof(h), f);
    fclose(f);
    if (n < sizeof(h) || h[0] != 0x7F || h[1] != 'C' || h[2] != 'N' || h[3] != 'T') { err = "not_pkg"; return false; }
    cid.assign((const char*)h + 0x40, 36);
    for (char c : cid) if (!((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' || c == '_')) { err = "not_pkg"; return false; }
    struct stat st;
    size = stat(path.c_str(), &st) == 0 ? (uint64_t)st.st_size : 0;
    return true;
}

#ifdef HOST_TEST
// Заглушки: имитация BGFT для проверок на ПК.
bool bgft_load() { g_bgft_info = "host-test stub"; return true; }
int bgft_register(const Item& it, int* task) {
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
    std::string url = bgft_path(it.path);
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

void run_item(Item* it) {
    int task = 0, r = 0;
    set(it, "registering");
    for (int k = 0; k < 30; k++) {
        r = bgft_register(*it, &task);
        if ((unsigned)r != 0x80990015u || g_cancel || g_stop) break;
        for (int s = 0; s < 3 && !g_cancel && !g_stop; s++) std::this_thread::sleep_for(std::chrono::milliseconds(HOST_SLEEP_MS));
    }
    if (g_cancel || g_stop) { set(it, "cancelled"); return; }
    if ((unsigned)r == 0x80990088u) { set(it, "already_installed"); return; }
    if (r != 0) { set(it, "failed", "register " + hx(r)); app_log("установка %s: регистрация %s", it->name.c_str(), hx(r).c_str()); return; }
    r = bgft_start(task);
    if (r != 0) { set(it, "failed", "start " + hx(r)); return; }
    set(it, "installing");
    uint64_t last = 0;
    int stall = 0;
    while (!g_cancel && !g_stop) {
        std::this_thread::sleep_for(std::chrono::milliseconds(HOST_SLEEP_MS));
        keep_awake_tick();
        uint64_t d = 0; int err = 0;
        int fin = bgft_progress(task, it->total, &d, &err);
        if (err != 0) { set(it, "failed", hx(err)); app_log("установка %s: ошибка %s", it->name.c_str(), hx(err).c_str()); notify("ps4commander: ошибка установки %s %s", hx(err).c_str(), it->cid.c_str()); return; }
        { std::lock_guard<std::mutex> l(g_mu); it->done = d; }
        if (fin > 0) { set(it, "done"); return; }
        if (d != last) { last = d; stall = 0; } else if (++stall >= STALL_TICKS) { set(it, "failed", "stalled"); return; }
    }
    set(it, "cancelled");
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
        if (smb_is_path(p)) { err = "smb_pkg"; return false; }
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
    if (!g_thr_started) { g_thr_started = true; g_thr = std::thread(worker); }
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
