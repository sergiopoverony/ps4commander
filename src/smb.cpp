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
#include <fcntl.h>
#include <mutex>
#include <sys/stat.h>
#include <unistd.h>

extern "C" {
#include <smb2/smb2.h>
#include <smb2/libsmb2.h>
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

void smb_shutdown() {
    std::vector<Ctx*> all;
    {
        std::lock_guard<std::mutex> l(g_mx);
        for (auto& p : g_pools) for (Ctx* x : p.second.ctxs) if (!x->busy) all.push_back(x);
        g_pools.clear();
    }
    for (Ctx* x : all) { drop_ctx(x); delete x; }
}
