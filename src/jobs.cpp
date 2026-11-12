#include "jobs.h"
#include "fs.h"
#include "json.h"
#include "log.h"
#include "platform.h"
#include "smb.h"
#include "vfs.h"
#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dirent.h>
#include <fcntl.h>
#include <mutex>
#include <sys/stat.h>
#include <thread>
#include <unistd.h>

namespace {

typedef std::chrono::steady_clock Clock;

struct State {
    long long id = 0;
    std::string op, state = "idle", cur;
    long long done_bytes = 0, total_bytes = 0, files_done = 0, files_total = 0;
    std::vector<std::pair<std::string, std::string>> errors;
    long long err_total = 0;
    Clock::time_point t0, t1;
};

std::mutex g_mx;
State g_st;
std::atomic<bool> g_cancel{false};
std::atomic<bool> g_busy{false};
std::atomic<bool> g_thread_alive{false};
Clock::time_point g_last_tick;

bool cancelled() { return g_cancel.load(); }

void set_cur(const std::string& p) {
    std::lock_guard<std::mutex> l(g_mx);
    g_st.cur = p;
}
void add_total(long long files, long long bytes) {
    std::lock_guard<std::mutex> l(g_mx);
    g_st.files_total += files;
    g_st.total_bytes += bytes;
}
void add_done(long long files, long long bytes) {
    std::lock_guard<std::mutex> l(g_mx);
    g_st.files_done += files;
    g_st.done_bytes += bytes;
}
void add_error(const std::string& path, const std::string& e) {
    std::lock_guard<std::mutex> l(g_mx);
    ++g_st.err_total;
    if (g_st.errors.size() < 100) g_st.errors.push_back({path, e});
    app_log("ошибка: %s: %s", path.c_str(), e.c_str());
}
void tick() {
    auto now = Clock::now();
    if (now - g_last_tick > std::chrono::seconds(8)) {
        g_last_tick = now;
        keep_awake_tick();
    }
}

bool list_dir(const std::string& path, std::vector<std::string>& names, int& err) {
    err = v_list(path, names);
    return err == 0;
}

// ---- подсчёт для прогресса ----
void count_tree(const std::string& p, long long& files, long long& bytes) {
    if (cancelled()) return;
    VStat st;
    if (v_lstat(p, st) != 0) return;
    if (st.dir) {
        ++files;
        std::vector<std::string> names;
        int e = 0;
        if (!list_dir(p, names, e)) return;
        for (auto& n : names) count_tree(fs_join(p, n), files, bytes);
    } else {
        ++files;
        if (st.reg) bytes += (long long)st.size;
    }
}

// ---- удаление ----
bool is_perm_err(int e) { return e == EACCES || e == EPERM || e == EROFS; }

// Не вышло из-за прав: открываем права на сам объект и (если это не защищённый каталог) на его родителя.
void relax_perms(const std::string& p) {
    if (smb_is_path(p)) return;
    v_chmod(p, 0777);
    std::string par = fs_parent(p);
    if (fs_danger(par) < 2) v_chmod(par, 0777);
}

void delete_tree(const std::string& p) {
    if (cancelled()) return;
    set_cur(p);
    tick();
    VStat st;
    int le = v_lstat(p, st);
    if (le) { add_error(p, fs_errname(le)); return; }
    if (st.dir) {
        std::vector<std::string> names;
        int e = 0;
        if (!list_dir(p, names, e) && is_perm_err(e)) {
            relax_perms(p);
            names.clear();
            list_dir(p, names, e);
        }
        for (auto& n : names) {
            if (cancelled()) return;
            delete_tree(fs_join(p, n));
        }
        if (cancelled()) return;
        int er = v_rmdir(p);
        if (er != 0) {
            if (is_perm_err(er)) { relax_perms(p); er = v_rmdir(p); if (er == 0) { add_done(1, 0); return; } }
            add_error(p, fs_errname(er));
        } else add_done(1, 0);
    } else {
        long long sz = st.reg ? (long long)st.size : 0;
        int er = v_unlink(p);
        if (er != 0) {
            if (is_perm_err(er)) { relax_perms(p); er = v_unlink(p); if (er == 0) { add_done(1, sz); return; } }
            add_error(p, fs_errname(er));
        } else add_done(1, sz);
    }
}

// ---- копирование ----
bool copy_file(const std::string& src, const std::string& dst, uint32_t mode) {
    VFile in, out;
    int e = v_open_read(src, in);
    if (e) { add_error(src, fs_errname(e)); return false; }
    e = v_open_write(dst, out, 0600);
    if (e) { v_close(in); add_error(dst, fs_errname(e)); return false; }
    std::vector<char> buf(1 << 20);
    bool ok = true;
    for (;;) {
        if (cancelled()) { ok = false; break; }
        long r = v_read(in, buf.data(), buf.size());
        if (r < 0) { add_error(src, fs_errname((int)-r)); ok = false; break; }
        if (r == 0) break;
        size_t off = 0;
        while (off < (size_t)r) {
            long w = v_write(out, buf.data() + off, (size_t)r - off);
            if (w <= 0) { add_error(dst, fs_errname(w < 0 ? (int)-w : EIO)); ok = false; break; }
            off += (size_t)w;
        }
        if (!ok) break;
        add_done(0, r);
        tick();
#ifdef HOST_TEST
        if (const char* slow = getenv("FM_SLOW_MS")) usleep((useconds_t)atoi(slow) * 1000);  // только для теста отмены
#endif
    }
    v_close(in);
    v_close(out);
    if (!ok) { v_unlink(dst); return false; }
    v_chmod(dst, mode & 07777);
    add_done(1, 0);
    return true;
}

void copy_tree(const std::string& src, const std::string& dst, bool overwrite) {
    if (cancelled()) return;
    set_cur(src);
    tick();
    VStat st;
    int le = v_lstat(src, st);
    if (le) { add_error(src, fs_errname(le)); return; }
    VStat dt;
    bool dst_exists = v_lstat(dst, dt) == 0;
    if (st.dir) {
        if (dst_exists && !dt.dir) { add_error(dst, "EEXIST"); return; }
        if (!dst_exists) { int me = v_mkdir(dst, 0777); if (me) { add_error(dst, fs_errname(me)); return; } }
        add_done(1, 0);
        std::vector<std::string> names;
        int e = 0;
        if (!list_dir(src, names, e)) { add_error(src, fs_errname(e)); return; }
        for (auto& n : names) {
            if (cancelled()) return;
            copy_tree(fs_join(src, n), fs_join(dst, n), overwrite);
        }
        if (!dst_exists) v_chmod(dst, st.mode & 07777);
    } else if (st.lnk) {
        if (smb_is_path(src) || smb_is_path(dst)) { add_error(src, "unsupported"); return; }
        char tgt[1024];
        ssize_t r = readlink(src.c_str(), tgt, sizeof(tgt) - 1);
        if (r <= 0) { add_error(src, fs_errname(errno)); return; }
        tgt[r] = 0;
        if (dst_exists) {
            if (!overwrite || dt.dir) { add_error(dst, "EEXIST"); return; }
            unlink(dst.c_str());
        }
        if (symlink(tgt, dst.c_str()) != 0) add_error(dst, fs_errname(errno));
        else add_done(1, 0);
    } else if (st.reg) {
        if (dst_exists) {
            if (!overwrite || dt.dir) {
                add_error(dst, "EEXIST");
                add_done(1, (long long)st.size);
                return;
            }
        }
        copy_file(src, dst, st.mode);
    } else {
        add_error(src, "unsupported");
    }
}

// ---- смена прав ----
void chmod_tree(const std::string& p, uint32_t mode, bool recursive) {
    if (cancelled()) return;
    set_cur(p);
    tick();
    VStat st;
    int le = v_lstat(p, st);
    if (le) { add_error(p, fs_errname(le)); return; }
    if (st.lnk) { add_done(1, 0); return; }
    int ce = v_chmod(p, mode);
    if (ce) add_error(p, fs_errname(ce));
    else add_done(1, 0);
    if (recursive && st.dir) {
        std::vector<std::string> names;
        int e = 0;
        if (!list_dir(p, names, e)) { add_error(p, fs_errname(e)); return; }
        for (auto& n : names) chmod_tree(fs_join(p, n), mode, recursive);
    }
}

void finish(const char* state) {
    std::lock_guard<std::mutex> l(g_mx);
    g_st.state = state;
    g_st.t1 = Clock::now();
    g_st.cur.clear();
}

void worker(JobSpec spec) {
    g_last_tick = Clock::now();
    app_log("задание %s: %zu объектов", spec.op.c_str(), spec.srcs.size());
    {
        std::lock_guard<std::mutex> l(g_mx);
        g_st.state = "scan";
    }
    if (spec.op == "copy" || spec.op == "delete" || spec.op == "chmod" || spec.op == "size") {
        long long files = 0, bytes = 0;
        for (auto& s : spec.srcs) count_tree(s, files, bytes);
        add_total(files, bytes);
    }
    {
        std::lock_guard<std::mutex> l(g_mx);
        if (g_st.state == "scan") g_st.state = "running";
    }
    keep_awake_tick();
    if (spec.op == "size") {
        // Подсчёт размера: всё уже посчитано на этапе подготовки, итог в files_total / total_bytes.
        add_done(0, 0);
    }
    for (auto& s : spec.srcs) {
        if (cancelled() || spec.op == "size") break;
        std::string dst = fs_join(spec.dest, fs_basename(s));
        if (spec.op == "delete") delete_tree(s);
        else if (spec.op == "chmod") chmod_tree(s, (uint32_t)spec.mode, spec.recursive);
        else if (spec.op == "copy") copy_tree(s, dst, spec.overwrite);
        else if (spec.op == "move") {
            if (s == dst) continue;
            VStat dt;
            bool exists = v_lstat(dst, dt) == 0;
            if (exists && !spec.overwrite) { add_error(dst, "EEXIST"); continue; }
            set_cur(s);
            int rne = exists ? EEXIST : v_rename(s, dst);
            if (!exists && rne == 0) {
                long long f = 0, b = 0;
                count_tree(dst, f, b);
                add_total(f, b);
                add_done(f, b);
                continue;
            }
            int e = rne;
            if (!exists && e != EXDEV) { add_error(s, fs_errname(e)); continue; }
            // Другой раздел (или перезапись): копируем, затем удаляем исходник.
            long long f = 0, b = 0;
            count_tree(s, f, b);
            add_total(f, b);
            size_t before;
            {
                std::lock_guard<std::mutex> l(g_mx);
                before = g_st.errors.size();
            }
            long long err_before;
            {
                std::lock_guard<std::mutex> l(g_mx);
                err_before = g_st.err_total;
            }
            (void)before;
            copy_tree(s, dst, spec.overwrite);
            bool clean;
            {
                std::lock_guard<std::mutex> l(g_mx);
                clean = g_st.err_total == err_before;
            }
            if (!cancelled() && clean) delete_tree(s);
        }
    }
    bool was_cancel = cancelled();
    long long errs;
    {
        std::lock_guard<std::mutex> l(g_mx);
        errs = g_st.err_total;
    }
    finish(was_cancel ? "cancelled" : errs ? "done_errors" : "done");
    app_log("задание %s завершено: %s", spec.op.c_str(), was_cancel ? "отменено" : errs ? "с ошибками" : "успешно");
    g_busy = false;
    g_thread_alive = false;
}

}  // namespace

bool job_start(const JobSpec& spec, std::string& err) {
    if (spec.op != "copy" && spec.op != "move" && spec.op != "delete" && spec.op != "chmod" && spec.op != "size") { err = "bad_op"; return false; }
    if (spec.srcs.empty() || spec.srcs.size() > 5000) { err = "bad_path"; return false; }
    bool two = spec.op == "copy" || spec.op == "move";
    if (two) {
        err = fs_check_modify(spec.dest, spec.unsafe, true);
        if (!err.empty()) return false;
        if (smb_is_path(spec.dest)) {
            VStat st;
            if (v_lstat(spec.dest, st) != 0 || !st.dir) { err = "bad_dest"; return false; }
        } else {
            struct stat st;
            if (stat(spec.dest.c_str(), &st) != 0 || !S_ISDIR(st.st_mode)) { err = "bad_dest"; return false; }
        }
    }
    for (auto& s : spec.srcs) {
        if (s.empty() || s[0] != '/') { err = "bad_path"; return false; }
        err = (spec.op == "copy" || spec.op == "size") ? fs_check_read(s) : fs_check_modify(s, spec.unsafe, false);
        if (!err.empty()) return false;
        if (two) {
            std::string dst = fs_join(spec.dest, fs_basename(s));
            if (spec.op == "copy" && dst == s) { err = "same_path"; return false; }
            VStat st;
            if (v_lstat(s, st) == 0 && st.dir && fs_inside(s, dst) && dst != s) { err = "into_self"; return false; }
        }
    }
    bool exp = false;
    if (!g_busy.compare_exchange_strong(exp, true)) { err = "busy"; return false; }
    {
        std::lock_guard<std::mutex> l(g_mx);
        long long id = g_st.id + 1;
        g_st = State();
        g_st.id = id;
        g_st.op = spec.op;
        g_st.state = "scan";
        g_st.t0 = g_st.t1 = Clock::now();
    }
    g_cancel = false;
    g_thread_alive = true;
    try {
        std::thread(worker, spec).detach();
    } catch (...) {
        g_thread_alive = false;
        g_busy = false;
        finish("done_errors");
        err = "no_thread";
        return false;
    }
    return true;
}

std::string job_json() {
    std::lock_guard<std::mutex> l(g_mx);
    bool running = g_st.state == "scan" || g_st.state == "running";
    auto end = running ? Clock::now() : g_st.t1;
    long long secs = g_st.id ? (long long)std::chrono::duration_cast<std::chrono::seconds>(end - g_st.t0).count() : 0;
    char head[320];
    snprintf(head, sizeof(head),
             "{\"id\":%lld,\"op\":\"%s\",\"state\":\"%s\",\"done\":%lld,\"total\":%lld,\"files_done\":%lld,\"files_total\":%lld,"
             "\"secs\":%lld,\"err_total\":%lld,\"cur\":\"",
             g_st.id, g_st.op.c_str(), g_st.state.c_str(), g_st.done_bytes, g_st.total_bytes, g_st.files_done, g_st.files_total,
             secs, g_st.err_total);
    std::string o = head;
    o += json_esc(g_st.cur) + "\",\"errors\":[";
    for (size_t i = 0; i < g_st.errors.size(); ++i) {
        if (i) o += ',';
        o += "{\"p\":\"" + json_esc(g_st.errors[i].first) + "\",\"e\":\"" + json_esc(g_st.errors[i].second) + "\"}";
    }
    return o + "]}";
}

void job_cancel() { g_cancel = true; }

void job_shutdown() {
    g_cancel = true;
    for (int i = 0; i < 200 && g_thread_alive.load(); ++i) std::this_thread::sleep_for(std::chrono::milliseconds(50));
}
