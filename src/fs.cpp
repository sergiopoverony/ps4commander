#include "fs.h"
#include "json.h"
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
    if (c.size() == 2 && (c[0] == "mnt" || c[0] == "user")) return 2;  // /mnt/usb0, /user/home, /user/data
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
    return !r.empty() && r != "/" && fs_inside(r, n);
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
    return "[{\"p\":\"" + json_esc(host_root()) + "\",\"mnt\":false}]";
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
    return o + "]";
#endif
}

std::string fs_ls_json(const std::string& n, std::string& err) {
#ifdef HOST_TEST
    if (!jail_ok(n)) { err = "outside_root"; return ""; }
#endif
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
    int fd = open(n.c_str(), O_RDONLY);
    if (fd < 0) { err = fs_errname(errno); return false; }
    struct stat st;
    if (fstat(fd, &st) != 0 || !S_ISREG(st.st_mode)) { err = S_ISDIR(st.st_mode) ? "EISDIR" : "not_regular"; close(fd); return false; }
    size = (long long)st.st_size;
    out.assign((size_t)len, '\0');
    size_t got = 0;
    while (got < (size_t)len) {
        ssize_t r = pread(fd, &out[got], (size_t)len - got, (off_t)(off + (long long)got));
        if (r < 0 && errno == EINTR) continue;
        if (r < 0) { err = fs_errname(errno); close(fd); return false; }
        if (r == 0) break;
        got += (size_t)r;
    }
    out.resize(got);
    close(fd);
    return true;
}

static bool valid_name(const std::string& nm) {
    return !nm.empty() && nm != "." && nm != ".." && nm.find('/') == std::string::npos && nm.size() <= 255;
}

bool fs_mkdir(const std::string& n, bool unsafe, std::string& err) {
    err = fs_check_modify(n, unsafe, true);
    if (!err.empty()) return false;
    if (n == "/" || !valid_name(fs_basename(n))) { err = "bad_name"; return false; }
    if (mkdir(n.c_str(), 0777) != 0) { err = fs_errname(errno); return false; }
    return true;
}

bool fs_rename(const std::string& from, const std::string& to, bool unsafe, std::string& err) {
    err = fs_check_modify(from, unsafe, false);
    if (err.empty()) err = fs_check_modify(to, unsafe, true);
    if (!err.empty()) return false;
    if (!valid_name(fs_basename(to))) { err = "bad_name"; return false; }
    if (fs_inside(from, to) && from != to) { err = "into_self"; return false; }
    struct stat st;
    if (from != to && lstat(to.c_str(), &st) == 0) { err = "EEXIST"; return false; }
    if (rename(from.c_str(), to.c_str()) != 0) { err = fs_errname(errno); return false; }
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
