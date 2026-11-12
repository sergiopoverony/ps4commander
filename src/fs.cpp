#include "fs.h"
#include "json.h"
#include "smb.h"
#include "vfs.h"
#include <algorithm>
#include <chrono>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dirent.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <sys/types.h>
#include <unistd.h>

std::string fs_errname(int e) {
    switch (e) {
        case EACCES: return "EACCES";
        case EPERM: return "EPERM";
        case ENOENT: return "ENOENT";
        case EEXIST: return "EEXIST";
        case ENOTDIR: return "ENOTDIR";
        case EISDIR: return "EISDIR";
        case ENOTEMPTY: return "ENOTEMPTY";
        case EROFS: return "EROFS";
        case ENOSPC: return "ENOSPC";
        case EXDEV: return "EXDEV";
        case EBUSY: return "EBUSY";
        case EIO: return "EIO";
        case EINVAL: return "EINVAL";
        case ENAMETOOLONG: return "ENAMETOOLONG";
        case EMFILE: return "EMFILE";
        case EFBIG: return "EFBIG";
        case ECONNREFUSED: return "ECONNREFUSED";
        case ETIMEDOUT: return "ETIMEDOUT";
        case EHOSTUNREACH: return "EHOSTUNREACH";
        case ENETUNREACH: return "ENETUNREACH";
        case ECONNRESET: return "ECONNRESET";
        case ENOTCONN: return "ENOTCONN";
        default: { char b[24]; snprintf(b, sizeof(b), "E%d", e); return b; }
    }
}

std::string fs_normalize(const std::string& p) {
    if (p.empty() || p[0] != '/' || p.find('\0') != std::string::npos || p.size() > 4000) return "";
    std::vector<std::string> parts;
    size_t i = 0;
    while (i <= p.size()) {
        size_t j = p.find('/', i);
        if (j == std::string::npos) j = p.size();
        std::string c = p.substr(i, j - i);
        if (c.empty() || c == ".") {}
        else if (c == "..") { if (parts.empty()) return ""; parts.pop_back(); }
        else parts.push_back(c);
        i = j + 1;
    }
    if (parts.empty()) return "/";
    std::string o;
    for (auto& c : parts) { o += '/'; o += c; }
    return o;
}

std::string fs_join(const std::string& dir, const std::string& name) {
    return dir == "/" ? "/" + name : dir + "/" + name;
}
std::string fs_basename(const std::string& n) {
    size_t p = n.rfind('/');
    return p == std::string::npos ? n : n.substr(p + 1);
}
std::string fs_parent(const std::string& n) {
    size_t p = n.rfind('/');
    if (p == std::string::npos || p == 0) return "/";
    return n.substr(0, p);
}
bool fs_inside(const std::string& dir, const std::string& child) {
    if (dir == "/") return true;
    if (child == dir) return true;
    return child.size() > dir.size() && child.compare(0, dir.size(), dir) == 0 && child[dir.size()] == '/';
}

static std::vector<std::string> comps(const std::string& n) {
    std::vector<std::string> v;
    size_t i = 1;
    while (i < n.size()) {
        size_t j = n.find('/', i);
        if (j == std::string::npos) j = n.size();
        v.push_back(n.substr(i, j - i));
        i = j + 1;
    }
    return v;
}

int fs_danger(const std::string& n) {
    if (n == "/") return 2;
    std::vector<std::string> c = comps(n);
    if (c.size() <= 1) return 2;                                   // /data, /user, /system, ...
    if (c.size() == 2 && (c[0] == "mnt" || c[0] == "user")) return 2;
    if (c[0] == "smb") return c.size() <= 2 ? 2 : 0;                  // /smb и корень шары защищены, внутри шары обычные файлы  // /mnt/usb0, /user/home, /user/data
    static const char* sys[] = {"system", "system_ex", "system_data", "preinst", "preinst2", "dev", "boot", nullptr};
    for (int k = 0; sys[k]; ++k)
        if (c[0] == sys[k]) return 1;
    return 0;
}

#ifdef HOST_TEST
// Тесты на ПК работают только внутри каталога из FM_ROOT: случайно тронуть настоящие файлы нельзя.
static std::string host_root() {
    const char* r = getenv("FM_ROOT");
    return r ? fs_normalize(r) : std::string();
}
static bool jail_ok(const std::string& n) {
    std::string r = host_root();
    return smb_is_path(n) || (!r.empty() && r != "/" && fs_inside(r, n));
}
#endif

std::string fs_check_read(const std::string& n) {
#ifdef HOST_TEST
    if (!jail_ok(n)) return "outside_root";
#else
    (void)n;
#endif
    return "";
}

std::string fs_check_modify(const std::string& n, bool unsafe, bool as_container) {
#ifdef HOST_TEST
    if (!jail_ok(n)) return "outside_root";
#endif
    int d = fs_danger(n);
    if (d == 2 && !as_container) return "protected";
    if (d == 1 && !unsafe) return "unsafe";
    return "";
}

std::string fs_roots_json() {
#ifdef HOST_TEST
    std::string o = "[{\"p\":\"" + json_esc(host_root()) + "\",\"mnt\":false}";
    for (auto& c : smb_list()) o += ",{\"p\":\"/smb/" + json_esc(c.alias) + "\",\"mnt\":false,\"smb\":true}";
    return o + "]";
#else
    static const char* cand[] = {"/", "/data", "/user", "/user/home", "/mnt/usb0", "/mnt/usb1", "/mnt/usb2", "/mnt/usb3",
                                 "/mnt/usb4", "/mnt/usb5", "/mnt/usb6", "/mnt/usb7", "/mnt/ext0", "/mnt/ext1",
                                 "/system_data", "/system", "/system_ex", "/preinst", nullptr};
    std::string o = "[";
    bool first = true;
    for (int k = 0; cand[k]; ++k) {
        struct stat st;
        if (stat(cand[k], &st) != 0 || !S_ISDIR(st.st_mode)) continue;
        if (!first) o += ',';
        first = false;
        // mnt: каталог - отдельная файловая система (диск реально подключён)
        bool mnt = false;
        if (!strncmp(cand[k], "/mnt/", 5)) { struct stat pst; mnt = stat("/mnt", &pst) == 0 && pst.st_dev != st.st_dev; }
        o += "{\"p\":\"" + std::string(cand[k]) + "\",\"mnt\":" + (mnt ? "true" : "false") + "}";
    }
    for (auto& c : smb_list()) {
        if (!first) o += ',';
        first = false;
        o += "{\"p\":\"/smb/" + json_esc(c.alias) + "\",\"mnt\":false,\"smb\":true}";
    }
    return o + "]";
#endif
}

std::string fs_ls_json(const std::string& n, std::string& err) {
#ifdef HOST_TEST
    if (!jail_ok(n)) { err = "outside_root"; return ""; }
#endif
    if (smb_is_path(n)) {
        std::vector<SmbEnt> ents;
        if (n == "/smb") {
            for (auto& c : smb_list()) { SmbEnt e; e.name = c.alias; e.st.dir = true; ents.push_back(e); }
        } else {
            int e = smb_list_dir(n, ents);
            if (e) { err = fs_errname(e); return ""; }
        }
        std::sort(ents.begin(), ents.end(), [](const SmbEnt& a, const SmbEnt& b) { return a.name < b.name; });
        const size_t kMaxS = 20000;
        std::string it;
        bool tr = false;
        size_t cnt = 0;
        for (auto& e : ents) {
            if (cnt >= kMaxS) { tr = true; break; }
            char buf[200];
            snprintf(buf, sizeof(buf), "\",\"t\":\"%c\",\"s\":%lld,\"m\":%lld,\"p\":\"%03o\",\"u\":0,\"g\":0}", e.st.dir ? 'd' : e.st.link ? 'l' : 'f',
                     (long long)e.st.size, (long long)e.st.mtime, e.st.dir ? 0755 : 0644);
            if (cnt) it += ',';
            it += "{\"n\":\"" + json_esc(e.name) + buf;
            ++cnt;
        }
        uint64_t tot = 0, av = 0;
        long long fb = -1, tb = -1;
        if (n != "/smb" && smb_space(n, tot, av) == 0) { fb = (long long)av; tb = (long long)tot; }
        char tl[200];
        snprintf(tl, sizeof(tl), "],\"free\":%lld,\"total\":%lld,\"danger\":%d,\"trunc\":%s}", fb, tb, fs_danger(n), tr ? "true" : "false");
        return "{\"path\":\"" + json_esc(n) + "\",\"parent\":\"" + json_esc(fs_parent(n)) + "\",\"entries\":[" + it + tl;
    }
    DIR* d = opendir(n.c_str());
    if (!d) { err = fs_errname(errno); return ""; }
    const size_t kMax = 20000;
    std::string items;
    size_t count = 0;
    bool trunc = false;
    while (struct dirent* e = readdir(d)) {
        const char* nm = e->d_name;
        if (!strcmp(nm, ".") || !strcmp(nm, "..")) continue;
        if (count >= kMax) { trunc = true; break; }
        std::string full = fs_join(n, nm);
        struct stat st;
        if (lstat(full.c_str(), &st) != 0) {
            // Запись есть, а прочитать её свойства нельзя: показываем как «неизвестную».
            if (count) items += ',';
            items += "{\"n\":\"" + json_esc(nm) + "\",\"t\":\"o\",\"s\":0,\"m\":0,\"p\":\"000\",\"u\":0,\"g\":0}";
            ++count;
            continue;
        }
        char type = S_ISDIR(st.st_mode) ? 'd' : S_ISLNK(st.st_mode) ? 'l' : S_ISREG(st.st_mode) ? 'f' : 'o';
        char buf[200];
        snprintf(buf, sizeof(buf), "\",\"t\":\"%c\",\"s\":%lld,\"m\":%lld,\"p\":\"%03o\",\"u\":%u,\"g\":%u", type,
                 (long long)st.st_size, (long long)st.st_mtime, (unsigned)(st.st_mode & 07777), (unsigned)st.st_uid,
                 (unsigned)st.st_gid);
        if (count) items += ',';
        items += "{\"n\":\"" + json_esc(nm) + buf;
        if (type == 'l') {
            char tgt[1024];
            ssize_t r = readlink(full.c_str(), tgt, sizeof(tgt) - 1);
            if (r > 0) { tgt[r] = 0; items += ",\"l\":\"" + json_esc(tgt) + "\""; }
            struct stat ts;
            if (stat(full.c_str(), &ts) == 0 && S_ISDIR(ts.st_mode)) items += ",\"td\":1";
        }
        items += '}';
        ++count;
    }
    closedir(d);
    // Сетевые папки живут в виртуальном каталоге /smb: показываем его в корне рядом с /user, /data и т.д.
#ifdef HOST_TEST
    bool is_top = n == host_root();
#else
    bool is_top = n == "/";
#endif
    if (is_top && items.find("\"n\":\"smb\",") == std::string::npos && count < kMax) {
        char vb[160];
        snprintf(vb, sizeof(vb), "\",\"t\":\"d\",\"s\":0,\"m\":%lld,\"p\":\"755\",\"u\":0,\"g\":0}", (long long)time(nullptr));
        if (count) items += ',';
        items += "{\"n\":\"smb" + std::string(vb);
        ++count;
    }
    long long freeb = -1, totalb = -1;
    struct statvfs sv;
    if (statvfs(n.c_str(), &sv) == 0) {
        freeb = (long long)sv.f_bavail * (long long)sv.f_frsize;
        totalb = (long long)sv.f_blocks * (long long)sv.f_frsize;
    }
    char tail[200];
    snprintf(tail, sizeof(tail), "],\"free\":%lld,\"total\":%lld,\"danger\":%d,\"trunc\":%s}", freeb, totalb, fs_danger(n),
             trunc ? "true" : "false");
    return "{\"path\":\"" + json_esc(n) + "\",\"parent\":\"" + json_esc(fs_parent(n)) + "\",\"entries\":[" + items + tail;
}

bool fs_read_range(const std::string& n, long long off, long long len, std::string& out, long long& size, std::string& err) {
#ifdef HOST_TEST
    if (!jail_ok(n)) { err = "outside_root"; return false; }
#endif
    if (off < 0) off = 0;
    if (len < 0) len = 0;
    if (len > (1 << 20)) len = 1 << 20;
    VStat vs;
    int e = v_lstat(n, vs);
    if (e) { err = fs_errname(e); return false; }
    if (vs.dir) { err = "EISDIR"; return false; }
    VFile f;
    e = v_open_read(n, f);
    if (e) { err = fs_errname(e); return false; }
    size = (long long)vs.size;
    out.assign((size_t)len, '\0');
    size_t got = 0;
    while (got < (size_t)len) {
        long r = v_pread(f, (uint64_t)(off + (long long)got), &out[got], (size_t)len - got);
        if (r < 0) { err = fs_errname((int)-r); v_close(f); return false; }
        if (r == 0) break;
        got += (size_t)r;
    }
    out.resize(got);
    v_close(f);
    return true;
}

static bool valid_name(const std::string& nm) {
    return !nm.empty() && nm != "." && nm != ".." && nm.find('/') == std::string::npos && nm.size() <= 255;
}

bool fs_mkdir(const std::string& n, bool unsafe, std::string& err) {
    err = fs_check_modify(n, unsafe, true);
    if (!err.empty()) return false;
    if (n == "/" || !valid_name(fs_basename(n))) { err = "bad_name"; return false; }
    int e = v_mkdir(n, 0777);
    if (e) { err = fs_errname(e); return false; }
    return true;
}

bool fs_rename(const std::string& from, const std::string& to, bool unsafe, std::string& err) {
    err = fs_check_modify(from, unsafe, false);
    if (err.empty()) err = fs_check_modify(to, unsafe, true);
    if (!err.empty()) return false;
    if (!valid_name(fs_basename(to))) { err = "bad_name"; return false; }
    if (fs_inside(from, to) && from != to) { err = "into_self"; return false; }
    VStat st;
    if (from != to && v_lstat(to, st) == 0) { err = "EEXIST"; return false; }
    int e = v_rename(from, to);
    if (e) { err = fs_errname(e); return false; }
    return true;
}

static char lc(char c) { return (c >= 'A' && c <= 'Z') ? (char)(c - 'A' + 'a') : c; }

bool fs_glob(const std::string& pat, const std::string& str) {
    size_t p = 0, s = 0, star = std::string::npos, mark = 0;
    while (s < str.size()) {
        if (p < pat.size() && (pat[p] == '?' || lc(pat[p]) == lc(str[s]))) { ++p; ++s; }
        else if (p < pat.size() && pat[p] == '*') { star = p++; mark = s; }
        else if (star != std::string::npos) { p = star + 1; s = ++mark; }
        else return false;
    }
    while (p < pat.size() && pat[p] == '*') ++p;
    return p == pat.size();
}

static bool contains_ci(const std::string& hay, const std::string& needle) {
    if (needle.empty()) return true;
    if (needle.size() > hay.size()) return false;
    for (size_t i = 0; i + needle.size() <= hay.size(); ++i) {
        size_t k = 0;
        while (k < needle.size() && lc(hay[i + k]) == lc(needle[k])) ++k;
        if (k == needle.size()) return true;
    }
    return false;
}

std::string fs_find_json(const std::string& root, const std::string& pattern, int max_results, int max_secs, std::string& err) {
#ifdef HOST_TEST
    if (!jail_ok(root)) { err = "outside_root"; return ""; }
#endif
    if (pattern.empty() || pattern.size() > 200) { err = "bad_pattern"; return ""; }
    bool glob = pattern.find('*') != std::string::npos || pattern.find('?') != std::string::npos;
    auto t0 = std::chrono::steady_clock::now();
    std::vector<std::string> stack{root};
    std::string items;
    int found = 0;
    long scanned = 0;
    bool trunc = false, any = false;
    while (!stack.empty() && !trunc) {
        std::string dir = stack.back();
        stack.pop_back();
        if (smb_is_path(dir)) {
            std::vector<SmbEnt> ents;
            int se = dir == "/smb" ? 0 : smb_list_dir(dir, ents);
            if (dir == "/smb") for (auto& c : smb_list()) { SmbEnt x; x.name = c.alias; x.st.dir = true; ents.push_back(x); }
            if (se) { if (!any && dir == root) { err = fs_errname(se); return ""; } any = true; continue; }
            any = true;
            for (auto& x : ents) {
                ++scanned;
                if (std::chrono::steady_clock::now() - t0 > std::chrono::seconds(max_secs) || scanned > 400000) { trunc = true; break; }
                std::string full = fs_join(dir, x.name);
                if (glob ? fs_glob(pattern, x.name) : contains_ci(x.name, pattern)) {
                    char buf[64];
                    snprintf(buf, sizeof(buf), "\",\"t\":\"%c\",\"s\":%lld}", x.st.dir ? 'd' : x.st.link ? 'l' : 'f', (long long)x.st.size);
                    if (found) items += ',';
                    items += "{\"p\":\"" + json_esc(full) + buf;
                    if (++found >= max_results) { trunc = true; break; }
                }
                if (x.st.dir) stack.push_back(full);
            }
            continue;
        }
        DIR* d = opendir(dir.c_str());
        if (!d) { if (!any && dir == root) { err = fs_errname(errno); return ""; } any = true; continue; }
        any = true;
        while (struct dirent* e = readdir(d)) {
            const char* nm = e->d_name;
            if (!strcmp(nm, ".") || !strcmp(nm, "..")) continue;
            ++scanned;
            if ((scanned & 255) == 0 &&
                std::chrono::steady_clock::now() - t0 > std::chrono::seconds(max_secs)) { trunc = true; break; }
            if (scanned > 400000) { trunc = true; break; }
            std::string full = fs_join(dir, nm);
            struct stat st;
            if (lstat(full.c_str(), &st) != 0) continue;
            bool hit = glob ? fs_glob(pattern, nm) : contains_ci(nm, pattern);
            if (hit) {
                char type = S_ISDIR(st.st_mode) ? 'd' : S_ISLNK(st.st_mode) ? 'l' : 'f';
                char buf[64];
                snprintf(buf, sizeof(buf), "\",\"t\":\"%c\",\"s\":%lld}", type, (long long)st.st_size);
                if (found) items += ',';
                items += "{\"p\":\"" + json_esc(full) + buf;
                if (++found >= max_results) { trunc = true; break; }
            }
            if (S_ISDIR(st.st_mode)) stack.push_back(full);
        }
        closedir(d);
    }
    char tail[96];
    snprintf(tail, sizeof(tail), "],\"trunc\":%s,\"scanned\":%ld}", trunc ? "true" : "false", scanned);
    return "{\"results\":[" + items + tail;
}
