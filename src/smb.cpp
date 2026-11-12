#include "smb.h"
#include "json.h"
#include "log.h"
#include "platform.h"

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <arpa/inet.h>
#include <atomic>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <thread>
#include <mutex>
#include <sys/stat.h>
#include <unistd.h>

extern "C" {
#include <smb2/smb2.h>
#include <smb2/libsmb2.h>
#include <smb2/libsmb2-share-enum.h>
void smb2_free_data(struct smb2_context* smb2, void* ptr);
}

namespace {

struct Ctx {
    struct smb2_context* c = nullptr;
    bool busy = false;
    std::chrono::steady_clock::time_point last;
};
struct Pool {
    std::vector<Ctx*> ctxs;
};

std::mutex g_mx;
std::condition_variable g_cv;
std::vector<SmbConn> g_conns;
std::vector<std::pair<std::string, Pool>> g_pools;
const size_t kMaxCtx = 4;
const int kTimeoutSec = 20;

std::string cfg_path() { return std::string(DATA_DIR) + "/smb.json"; }

Pool& pool_of(const std::string& alias) {
    for (auto& p : g_pools) if (p.first == alias) return p.second;
    g_pools.push_back({alias, Pool()});
    return g_pools.back().second;
}

bool valid_alias(const std::string& a) {
    if (a.empty() || a.size() > 32) return false;
    for (char c : a) if (!(isalnum((unsigned char)c) || c == '_' || c == '-' || c == '.')) return false;
    return a != "." && a != "..";
}
bool valid_host(const std::string& h) {
    if (h.empty() || h.size() > 255) return false;
    for (char c : h) if (!(isalnum((unsigned char)c) || c == '.' || c == '-' || c == ':' || c == '_' || c == '[' || c == ']')) return false;
    return true;
}
bool valid_share(const std::string& s) {
    if (s.empty() || s.size() > 80) return false;
    for (char c : s) if (c == '/' || c == '\\' || (unsigned char)c < 32) return false;
    return true;
}

// Создаёт и подключает контекст. Возвращает errno (0 при успехе).
int make_ctx(const SmbConn& cn, struct smb2_context** out) {
    struct smb2_context* c = smb2_init_context();
    if (!c) return ENOMEM;
    smb2_set_security_mode(c, SMB2_NEGOTIATE_SIGNING_ENABLED);
    smb2_set_timeout(c, kTimeoutSec);
    if (!cn.pass.empty()) smb2_set_password(c, cn.pass.c_str());
    if (!cn.domain.empty()) smb2_set_domain(c, cn.domain.c_str());
    int r = smb2_connect_share(c, cn.host.c_str(), cn.share.c_str(), cn.user.c_str());
    if (r < 0) {
        app_log("smb: подключение %s/%s: %s (%d)", cn.host.c_str(), cn.share.c_str(), smb2_get_error(c), r);
        smb2_destroy_context(c);
        return -r;
    }
    *out = c;
    return 0;
}

void drop_ctx(Ctx* x) {
    if (x->c) { smb2_disconnect_share(x->c); smb2_destroy_context(x->c); x->c = nullptr; }
}

// Берёт свободное подключение (или создаёт новое). Блокирует до 30 с, если все заняты.
int acquire(const std::string& alias, Ctx** out, bool fresh = false) {
    SmbConn cn;
    {
        std::unique_lock<std::mutex> l(g_mx);
        bool found = false;
        for (auto& c : g_conns) if (c.alias == alias) { cn = c; found = true; }
        if (!found) return ENOENT;
        Pool& p = pool_of(alias);
        auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
        for (;;) {
            if (!fresh) {
                for (Ctx* x : p.ctxs) if (!x->busy && x->c) { x->busy = true; *out = x; return 0; }
            }
            // убрать мёртвые свободные
            for (size_t i = 0; i < p.ctxs.size();) {
                if (!p.ctxs[i]->busy && !p.ctxs[i]->c) { delete p.ctxs[i]; p.ctxs.erase(p.ctxs.begin() + (long)i); } else ++i;
            }
            if (p.ctxs.size() < kMaxCtx) break;
            if (fresh) {  // нужен новый: выкинем один свободный
                bool dropped = false;
                for (size_t i = 0; i < p.ctxs.size(); i++) if (!p.ctxs[i]->busy) { Ctx* x = p.ctxs[i]; p.ctxs.erase(p.ctxs.begin() + (long)i); l.unlock(); drop_ctx(x); delete x; l.lock(); dropped = true; break; }
                if (dropped) continue;
            }
            if (g_cv.wait_until(l, deadline) == std::cv_status::timeout) return EBUSY;
        }
        // место зарезервировано: вставляем занятую запись до подключения
        Ctx* x = new Ctx();
        x->busy = true;
        p.ctxs.push_back(x);
        *out = x;
    }
    Ctx* x = *out;
    struct smb2_context* c = nullptr;
    int r = make_ctx(cn, &c);
    std::lock_guard<std::mutex> l(g_mx);
    if (r != 0) {
        Pool& p = pool_of(alias);
        p.ctxs.erase(std::remove(p.ctxs.begin(), p.ctxs.end(), x), p.ctxs.end());
        delete x;
        g_cv.notify_all();
        return r;
    }
    x->c = c;
    x->last = std::chrono::steady_clock::now();
    return 0;
}

void release(const std::string& alias, Ctx* x, bool bad) {
    if (bad) {
        drop_ctx(x);
    }
    std::lock_guard<std::mutex> l(g_mx);
    x->busy = false;
    x->last = std::chrono::steady_clock::now();
    (void)alias;
    g_cv.notify_all();
}

// Код ошибки по ответу libsmb2: сбой разбора/сокета означает, что подключение испорчено (ECONNRESET).
int fail_code(struct smb2_context* c, int r) {
    const char* m = smb2_get_error(c);
    if (m && (strstr(m, "smb2_service failed") || strstr(m, "Socket") || strstr(m, "socket") || strstr(m, "Connection") || strstr(m, "Broken pipe"))) return ECONNRESET;
    return r < 0 ? -r : EIO;
}

bool conn_error(int e) { return e == ECONNRESET || e == EPIPE || e == ETIMEDOUT || e == ENOTCONN || e == ECONNABORTED || e == EIO || e == ENETRESET; }

// Выполняет операцию на подключении; при обрыве соединения один раз повторяет на новом.
template <class F>
int with_conn(const std::string& alias, F fn) {
    for (int attempt = 0; attempt < 2; attempt++) {
        Ctx* x = nullptr;
        int r = acquire(alias, &x, attempt > 0);
        if (r != 0) return r;
        int e = fn(x->c);
        bool bad = conn_error(e);
        release(alias, x, bad);
        if (!bad) return e;
        if (attempt == 1) return e;
    }
    return EIO;
}

SmbStat to_stat(const struct smb2_stat_64& s) {
    SmbStat o;
    o.dir = s.smb2_type == SMB2_TYPE_DIRECTORY;
    o.link = s.smb2_type == SMB2_TYPE_LINK;
    o.size = s.smb2_size;
    o.mtime = (int64_t)s.smb2_mtime;
    return o;
}

}  // namespace

struct SmbFile {
    std::string alias;
    Ctx* x = nullptr;
    struct smb2fh* fh = nullptr;
    bool bad = false;
    bool can_read_all = false;
    uint64_t size = 0;  // только для чтения: за конец файла не читаем (некоторые серверы отвечают на это ошибкой, ломающей подключение)
};

bool smb_is_path(const std::string& p) { return p == "/smb" || p.compare(0, 5, "/smb/") == 0; }

bool smb_split(const std::string& p, std::string& alias, std::string& inner) {
    if (p.compare(0, 5, "/smb/") != 0) return false;
    size_t s = p.find('/', 5);
    alias = p.substr(5, s == std::string::npos ? std::string::npos : s - 5);
    inner = s == std::string::npos ? "" : p.substr(s + 1);
    return !alias.empty();
}

bool smb_same_share(const std::string& a, const std::string& b) {
    std::string x, y, i1, i2;
    return smb_split(a, x, i1) && smb_split(b, y, i2) && x == y;
}

// ---- настройки ----
void smb_load_config() {
    std::lock_guard<std::mutex> l(g_mx);
    g_conns.clear();
    FILE* f = fopen(cfg_path().c_str(), "rb");
    if (!f) return;
    std::string s;
    char b[4096];
    size_t n;
    while ((n = fread(b, 1, sizeof(b), f)) > 0 && s.size() < (1 << 20)) s.append(b, n);
    fclose(f);
    JsonObj j = json_parse(s);
    if (!j.ok) return;
    const std::vector<std::string>* arr = j.get_arr("c");
    if (!arr) return;
    for (auto& item : *arr) {
        JsonObj o = json_parse(item);
        if (!o.ok) continue;
        SmbConn c;
        c.alias = o.get_str("a"); c.host = o.get_str("h"); c.share = o.get_str("s");
        c.user = o.get_str("u"); c.pass = o.get_str("p"); c.domain = o.get_str("d");
        if (valid_alias(c.alias) && valid_host(c.host) && valid_share(c.share)) g_conns.push_back(c);
    }
}

static bool save_config_locked() {
    std::string o = "{\"c\":[";
    for (size_t i = 0; i < g_conns.size(); i++) {
        auto& c = g_conns[i];
        if (i) o += ",";
        o += "\"" + json_esc("{\"a\":\"" + json_esc(c.alias) + "\",\"h\":\"" + json_esc(c.host) + "\",\"s\":\"" + json_esc(c.share) +
                              "\",\"u\":\"" + json_esc(c.user) + "\",\"p\":\"" + json_esc(c.pass) + "\",\"d\":\"" + json_esc(c.domain) + "\"}") + "\"";
    }
    o += "]}";
    std::string tmp = cfg_path() + ".tmp";
    int fd = open(tmp.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (fd < 0) return false;
    bool ok = write(fd, o.data(), o.size()) == (ssize_t)o.size();
    ok = close(fd) == 0 && ok;
    if (!ok) { unlink(tmp.c_str()); return false; }
    return rename(tmp.c_str(), cfg_path().c_str()) == 0;
}

std::vector<SmbConn> smb_list() {
    std::lock_guard<std::mutex> l(g_mx);
    return g_conns;
}
bool smb_get(const std::string& alias, SmbConn& out) {
    std::lock_guard<std::mutex> l(g_mx);
    for (auto& c : g_conns) if (c.alias == alias) { out = c; return true; }
    return false;
}

std::string smb_conns_json() {
    std::lock_guard<std::mutex> l(g_mx);
    std::string o = "[";
    for (size_t i = 0; i < g_conns.size(); i++) {
        auto& c = g_conns[i];
        if (i) o += ",";
        o += "{\"alias\":\"" + json_esc(c.alias) + "\",\"host\":\"" + json_esc(c.host) + "\",\"share\":\"" + json_esc(c.share) +
             "\",\"user\":\"" + json_esc(c.user) + "\",\"domain\":\"" + json_esc(c.domain) + "\",\"has_pass\":" + (c.pass.empty() ? "false" : "true") + "}";
    }
    return o + "]";
}

bool smb_save(SmbConn c, std::string& err) {
    if (!valid_alias(c.alias)) { err = "bad_alias"; return false; }
    if (!valid_host(c.host)) { err = "bad_host"; return false; }
    if (!valid_share(c.share)) { err = "bad_share"; return false; }
    if (c.user.size() > 128 || c.pass.size() > 256 || c.domain.size() > 128) { err = "bad_value"; return false; }
    {
        std::lock_guard<std::mutex> l(g_mx);
        if (c.pass.empty()) for (auto& o : g_conns) if (o.alias == c.alias) c.pass = o.pass;
    }
    struct smb2_context* t = nullptr;
    int r = make_ctx(c, &t);
    if (r != 0) { err = r == EACCES || r == EPERM ? "smb_auth" : r == ENOENT ? "smb_share" : "E" + std::string(r == ECONNREFUSED ? "CONNREFUSED" : r == ETIMEDOUT ? "TIMEDOUT" : r == EHOSTUNREACH ? "HOSTUNREACH" : r == ENETUNREACH ? "NETUNREACH" : "IO"); return false; }
    smb2_disconnect_share(t);
    smb2_destroy_context(t);
    std::vector<Ctx*> old;
    {
        std::lock_guard<std::mutex> l(g_mx);
        bool found = false;
        for (auto& o : g_conns) if (o.alias == c.alias) { o = c; found = true; }
        if (!found) g_conns.push_back(c);
        if (!save_config_locked()) { err = "write_failed"; return false; }
        // старые подключения этого имени больше не годятся (мог смениться хост/пароль)
        Pool& p = pool_of(c.alias);
        for (size_t i = 0; i < p.ctxs.size();) {
            if (!p.ctxs[i]->busy) { old.push_back(p.ctxs[i]); p.ctxs.erase(p.ctxs.begin() + (long)i); } else ++i;
        }
    }
    for (Ctx* x : old) { drop_ctx(x); delete x; }
    app_log("smb: сохранено подключение %s (%s/%s)", c.alias.c_str(), c.host.c_str(), c.share.c_str());
    return true;
}

bool smb_remove(const std::string& alias) {
    std::vector<Ctx*> old;
    {
        std::lock_guard<std::mutex> l(g_mx);
        auto it = std::remove_if(g_conns.begin(), g_conns.end(), [&](const SmbConn& c) { return c.alias == alias; });
        if (it == g_conns.end()) return false;
        g_conns.erase(it, g_conns.end());
        save_config_locked();
        Pool& p = pool_of(alias);
        for (size_t i = 0; i < p.ctxs.size();) {
            if (!p.ctxs[i]->busy) { old.push_back(p.ctxs[i]); p.ctxs.erase(p.ctxs.begin() + (long)i); } else ++i;
        }
    }
    for (Ctx* x : old) { drop_ctx(x); delete x; }
    return true;
}

// ---- операции ----
int smb_stat(const std::string& path, SmbStat& st) {
    std::string a, in;
    if (!smb_split(path, a, in)) return ENOENT;
    if (in.empty()) { st = SmbStat(); st.dir = true; return 0; }
    return with_conn(a, [&](struct smb2_context* c) {
        struct smb2_stat_64 s;
        memset(&s, 0, sizeof(s));
        int r = smb2_stat(c, in.c_str(), &s);
        if (r < 0) return fail_code(c, r);
        st = to_stat(s);
        return 0;
    });
}

int smb_list_dir(const std::string& path, std::vector<SmbEnt>& out) {
    std::string a, in;
    if (!smb_split(path, a, in)) return ENOENT;
    return with_conn(a, [&](struct smb2_context* c) {
        out.clear();
        struct smb2dir* d = smb2_opendir(c, in.c_str());
        if (!d) {
            const char* m = smb2_get_error(c);
            int e = EIO;
            if (m && strstr(m, "NO_SUCH_FILE")) e = ENOENT; else if (m && (strstr(m, "ACCESS_DENIED"))) e = EACCES;
            else if (m && (strstr(m, "NOT_A_DIRECTORY"))) e = ENOTDIR; else if (m && strstr(m, "OBJECT_NAME_NOT_FOUND")) e = ENOENT;
            else if (m && strstr(m, "OBJECT_PATH_NOT_FOUND")) e = ENOENT;
            else if (m && (strstr(m, "reset") || strstr(m, "connection") || strstr(m, "Connection") || strstr(m, "Socket"))) e = ECONNRESET;
            return e;
        }
        while (struct smb2dirent* de = smb2_readdir(c, d)) {
            if (!de->name || !strcmp(de->name, ".") || !strcmp(de->name, "..")) continue;
            SmbEnt e;
            e.name = de->name;
            e.st = to_stat(de->st);
            out.push_back(e);
        }
        smb2_closedir(c, d);
        return 0;
    });
}

static int simple_op(const std::string& path, int (*op)(struct smb2_context*, const char*)) {
    std::string a, in;
    if (!smb_split(path, a, in) || in.empty()) return EPERM;
    return with_conn(a, [&](struct smb2_context* c) { int r = op(c, in.c_str()); return r < 0 ? fail_code(c, r) : 0; });
}
int smb_mkdir(const std::string& path) { return simple_op(path, smb2_mkdir); }
int smb_rmdir(const std::string& path) { return simple_op(path, smb2_rmdir); }
int smb_unlink(const std::string& path) { return simple_op(path, smb2_unlink); }

int smb_rename(const std::string& from, const std::string& to) {
    std::string a, in, b, in2;
    if (!smb_split(from, a, in) || !smb_split(to, b, in2) || in.empty() || in2.empty()) return EPERM;
    if (a != b) return EXDEV;
    return with_conn(a, [&](struct smb2_context* c) { int r = smb2_rename(c, in.c_str(), in2.c_str()); return r < 0 ? fail_code(c, r) : 0; });
}

int smb_space(const std::string& path, uint64_t& total, uint64_t& avail) {
    std::string a, in;
    if (!smb_split(path, a, in)) return ENOENT;
    return with_conn(a, [&](struct smb2_context* c) {
        struct smb2_statvfs v;
        memset(&v, 0, sizeof(v));
        int r = smb2_statvfs(c, in.c_str(), &v);
        if (r < 0) return fail_code(c, r);
        total = v.f_blocks * (uint64_t)v.f_frsize;
        avail = v.f_bavail * (uint64_t)v.f_frsize;
        return 0;
    });
}

int smb_open(const std::string& path, bool write, SmbFile** out) {
    std::string a, in;
    if (!smb_split(path, a, in) || in.empty()) return EISDIR;
    for (int attempt = 0; attempt < 2; attempt++) {
        Ctx* x = nullptr;
        int r = acquire(a, &x, attempt > 0);
        if (r != 0) return r;
        struct smb2fh* fh = smb2_open(x->c, in.c_str(), write ? (O_WRONLY | O_CREAT | O_TRUNC) : O_RDONLY);
        if (fh) {
            SmbFile* f = new SmbFile();
            f->alias = a; f->x = x; f->fh = fh;
            if (!write) {
                struct smb2_stat_64 s64;
                memset(&s64, 0, sizeof(s64));
                if (smb2_fstat(x->c, fh, &s64) == 0) { f->size = s64.smb2_size; f->can_read_all = true; }
            }
            *out = f;
            return 0;
        }
        const char* m = smb2_get_error(x->c);
        int e = EIO;
        bool bad = false;
        if (m && strstr(m, "NAME_NOT_FOUND")) e = ENOENT;
        else if (m && strstr(m, "PATH_NOT_FOUND")) e = ENOENT;
        else if (m && strstr(m, "ACCESS_DENIED")) e = EACCES;
        else if (m && strstr(m, "SHARING_VIOLATION")) e = EBUSY;
        else if (m && strstr(m, "IS_A_DIRECTORY")) e = EISDIR;
        else if (m && strstr(m, "DISK_FULL")) e = ENOSPC;
        else bad = true;
        release(a, x, bad);
        if (!bad || attempt == 1) return e;
    }
    return EIO;
}

long smb_read(SmbFile* f, uint64_t off, void* buf, size_t n) {
    uint32_t mx = smb2_get_max_read_size(f->x->c);
    if (mx == 0 || mx > (1u << 20)) mx = 1u << 20;
    size_t want = n < mx ? n : mx;
    if (f->can_read_all) {
        if (off >= f->size) return 0;
        if (want > f->size - off) want = (size_t)(f->size - off);
    }
    int r = smb2_pread(f->x->c, f->fh, (uint8_t*)buf, (uint32_t)want, off);
    if (r < 0) { int e = fail_code(f->x->c, r); if (conn_error(e)) f->bad = true; return -(long)e; }
    return r;
}

long smb_write(SmbFile* f, uint64_t off, const void* buf, size_t n) {
    uint32_t mx = smb2_get_max_write_size(f->x->c);
    if (mx == 0 || mx > (1u << 20)) mx = 1u << 20;
    size_t want = n < mx ? n : mx;
    int r = smb2_pwrite(f->x->c, f->fh, (const uint8_t*)buf, (uint32_t)want, off);
    if (r < 0) { int e = fail_code(f->x->c, r); if (conn_error(e)) f->bad = true; return -(long)e; }
    return r;
}

void smb_close(SmbFile* f) {
    if (!f) return;
    if (f->fh && !f->bad) smb2_close(f->x->c, f->fh);
    release(f->alias, f->x, f->bad);
    delete f;
}

// ---- сканирование сети ----
namespace {
std::mutex g_scan_mx;
bool g_scan_running = false, g_scan_cancel = false;
unsigned g_scan_done = 0, g_scan_total = 0;
std::vector<std::string> g_scan_found;
std::thread g_scan_thr;

std::string ip_str(unsigned a) {
    char b[32];
    snprintf(b, sizeof(b), "%u.%u.%u.%u", (a >> 24) & 255, (a >> 16) & 255, (a >> 8) & 255, a & 255);
    return b;
}

void scan_worker(std::vector<unsigned> hosts) {
    const size_t kBatch = 64;
    auto last_tick = std::chrono::steady_clock::now();
    for (size_t i = 0; i < hosts.size(); i += kBatch) {
        {
            std::lock_guard<std::mutex> l(g_scan_mx);
            if (g_scan_cancel) break;
        }
        size_t n = std::min(kBatch, hosts.size() - i);
        std::vector<struct pollfd> pf;
        std::vector<unsigned> who;
        for (size_t k = 0; k < n; k++) {
            int s = socket(AF_INET, SOCK_STREAM, 0);
            if (s < 0) continue;
            fcntl(s, F_SETFL, fcntl(s, F_GETFL, 0) | O_NONBLOCK);
            struct sockaddr_in sa;
            memset(&sa, 0, sizeof(sa));
            sa.sin_family = AF_INET;
            sa.sin_port = htons(445);
            sa.sin_addr.s_addr = htonl(hosts[i + k]);
            int r = connect(s, (struct sockaddr*)&sa, sizeof(sa));
            if (r != 0 && errno != EINPROGRESS) { close(s); continue; }
            struct pollfd p;
            p.fd = s; p.events = POLLOUT; p.revents = 0;
            pf.push_back(p);
            who.push_back(hosts[i + k]);
        }
        // ждём до 700 мс, пока соединения завершатся
        auto t0 = std::chrono::steady_clock::now();
        std::vector<bool> fin(pf.size(), false);
        for (;;) {
            int left = 700 - (int)std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count();
            if (left <= 0) break;
            int pr = poll(pf.data(), (nfds_t)pf.size(), left);
            if (pr <= 0) break;
            bool all = true;
            for (size_t k = 0; k < pf.size(); k++) {
                if (fin[k]) continue;
                if (pf[k].revents) {
                    int err = 0;
                    socklen_t el = sizeof(err);
                    getsockopt(pf[k].fd, SOL_SOCKET, SO_ERROR, &err, &el);
                    fin[k] = true;
                    pf[k].fd = -pf[k].fd - 1;   // poll пропускает отрицательные дескрипторы; настоящий восстановим при закрытии
                    if (err == 0) {
                        std::lock_guard<std::mutex> l(g_scan_mx);
                        g_scan_found.push_back(ip_str(who[k]));
                    }
                } else all = false;
            }
            if (all) break;
        }
        for (size_t k = 0; k < pf.size(); k++) close(pf[k].fd < 0 ? -pf[k].fd - 1 : pf[k].fd);
        {
            std::lock_guard<std::mutex> l(g_scan_mx);
            g_scan_done += (unsigned)n;
        }
        if (std::chrono::steady_clock::now() - last_tick > std::chrono::seconds(5)) { keep_awake_tick(); last_tick = std::chrono::steady_clock::now(); }
    }
    std::lock_guard<std::mutex> l(g_scan_mx);
    std::sort(g_scan_found.begin(), g_scan_found.end(), [](const std::string& a, const std::string& b) {
        unsigned x[4] = {0}, y[4] = {0};
        sscanf(a.c_str(), "%u.%u.%u.%u", &x[0], &x[1], &x[2], &x[3]);
        sscanf(b.c_str(), "%u.%u.%u.%u", &y[0], &y[1], &y[2], &y[3]);
        for (int k = 0; k < 4; k++) if (x[k] != y[k]) return x[k] < y[k];
        return false;
    });
    g_scan_running = false;
}
}  // namespace

bool smb_scan_start(std::string& err) {
    unsigned ip = 0, mask = 0;
    if (!local_subnet(ip, mask)) { err = "no_net"; return false; }
    // слишком большую сеть ограничиваем до /22 (1022 адреса), иначе берём /24 вокруг своего адреса
    unsigned hostbits = 0;
    for (unsigned m = ~mask; m; m >>= 1) hostbits++;
    if (hostbits > 10) { mask = 0xFFFFFF00u; }
    unsigned net = ip & mask, bcast = net | ~mask;
    std::vector<unsigned> hosts;
    if (mask == 0xFFFFFFFFu) hosts.push_back(ip);
    else for (unsigned a = net + 1; a < bcast; a++) if (a != ip) hosts.push_back(a);
    if (hosts.empty()) { err = "no_net"; return false; }
    std::lock_guard<std::mutex> l(g_scan_mx);
    if (g_scan_running) { err = "busy"; return false; }
    if (g_scan_thr.joinable()) g_scan_thr.join();
    g_scan_running = true; g_scan_cancel = false;
    g_scan_done = 0; g_scan_total = (unsigned)hosts.size();
    g_scan_found.clear();
    g_scan_thr = std::thread(scan_worker, hosts);
    return true;
}

std::string smb_scan_json() {
    std::lock_guard<std::mutex> l(g_scan_mx);
    std::string o = "{\"running\":" + std::string(g_scan_running ? "true" : "false") + ",\"done\":" + std::to_string(g_scan_done) +
                    ",\"total\":" + std::to_string(g_scan_total) + ",\"found\":[";
    for (size_t i = 0; i < g_scan_found.size(); i++) o += (i ? "," : "") + std::string("\"") + g_scan_found[i] + "\"";
    return o + "]}";
}

bool smb_list_shares(const SmbConn& c0, std::vector<std::string>& out, std::string& err) {
    SmbConn c = c0;
    if (!valid_host(c.host)) { err = "bad_host"; return false; }
    if (c.user.size() > 128 || c.pass.size() > 256 || c.domain.size() > 128) { err = "bad_value"; return false; }
    c.share = "IPC$";
    struct smb2_context* ctx = nullptr;
    int r = make_ctx(c, &ctx);
    if (r != 0) { err = r == EACCES || r == EPERM ? "smb_auth" : "E" + std::string(r == ECONNREFUSED ? "CONNREFUSED" : r == ETIMEDOUT ? "TIMEDOUT" : r == EHOSTUNREACH ? "HOSTUNREACH" : "IO"); return false; }
    struct smb2_share_enum_reply* rep = smb2_share_enum_sync(ctx, SMB2_SHARE_INFO_1);
    if (!rep) {
        app_log("smb: список папок %s: %s", c.host.c_str(), smb2_get_error(ctx));
        smb2_disconnect_share(ctx); smb2_destroy_context(ctx);
        err = "smb_enum";
        return false;
    }
    out.clear();
    for (uint32_t i = 0; i < rep->entries_read; i++) {
        const struct smb2_share_info_1& s = rep->share_info.info_1[i];
        if (!s.netname) continue;
        if ((s.type & 0xFFFF) != SMB2_SHARE_TYPE_DISKTREE || (s.type & SMB2_SHARE_TYPE_HIDDEN)) continue;
        size_t nl = strlen(s.netname);
        if (nl == 0 || s.netname[nl - 1] == '$') continue;   // скрытые и служебные (IPC$, ADMIN$, C$)
        out.push_back(s.netname);
    }
    smb2_free_data(ctx, rep);
    smb2_disconnect_share(ctx); smb2_destroy_context(ctx);
    std::sort(out.begin(), out.end());
    return true;
}

void smb_shutdown() {
    {
        std::lock_guard<std::mutex> l(g_scan_mx);
        g_scan_cancel = true;
    }
    if (g_scan_thr.joinable()) g_scan_thr.join();
    std::vector<Ctx*> all;
    {
        std::lock_guard<std::mutex> l(g_mx);
        for (auto& p : g_pools) for (Ctx* x : p.second.ctxs) if (!x->busy) all.push_back(x);
        g_pools.clear();
    }
    for (Ctx* x : all) { drop_ctx(x); delete x; }
}
