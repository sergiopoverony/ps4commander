#include "zip.h"
#include "fs.h"
#include "log.h"
#include "platform.h"

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <ctime>
#include <dirent.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

namespace {

const uint64_t K32 = 0xFFFFFFFFull;
const size_t kMaxEntries = 300000;

uint32_t crc_table[256];
bool crc_ready = false;
void crc_init() {
    for (uint32_t i = 0; i < 256; i++) {
        uint32_t c = i;
        for (int k = 0; k < 8; k++) c = c & 1 ? 0xEDB88320u ^ (c >> 1) : c >> 1;
        crc_table[i] = c;
    }
    crc_ready = true;
}
uint32_t crc_update(uint32_t crc, const unsigned char* p, size_t n) {
    crc = ~crc;
    while (n--) crc = crc_table[(crc ^ *p++) & 0xFF] ^ (crc >> 8);
    return ~crc;
}

void p16(std::string& o, uint32_t v) { o.push_back((char)(v & 0xFF)); o.push_back((char)((v >> 8) & 0xFF)); }
void p32(std::string& o, uint32_t v) { p16(o, v & 0xFFFF); p16(o, (v >> 16) & 0xFFFF); }
void p64(std::string& o, uint64_t v) { p32(o, (uint32_t)(v & K32)); p32(o, (uint32_t)(v >> 32)); }

void dos_time(long long t, uint32_t& dt, uint32_t& dd) {
    time_t tt = (time_t)t;
    struct tm tmv;
    memset(&tmv, 0, sizeof(tmv));
    localtime_r(&tt, &tmv);
    int y = tmv.tm_year + 1900;
    if (y < 1980) { y = 1980; tmv.tm_mon = 0; tmv.tm_mday = 1; tmv.tm_hour = tmv.tm_min = tmv.tm_sec = 0; }
    dt = (uint32_t)((tmv.tm_hour << 11) | (tmv.tm_min << 5) | (tmv.tm_sec / 2));
    dd = (uint32_t)(((y - 1980) << 9) | ((tmv.tm_mon + 1) << 5) | tmv.tm_mday);
}

bool big(const ZipEntry& e) { return !e.dir && e.size >= K32; }

// Локальный заголовок: флаги 0x0808 (дескриптор данных + UTF-8) для файла, 0x0800 для каталога.
std::string local_header(const ZipEntry& e) {
    std::string o;
    uint32_t dt, dd;
    dos_time(e.mtime, dt, dd);
    bool z = big(e);
    p32(o, 0x04034b50);
    p16(o, z ? 45 : 20);
    p16(o, e.dir ? 0x0800 : 0x0808);
    p16(o, 0);
    p16(o, dt); p16(o, dd);
    p32(o, 0);                           // CRC: в дескрипторе
    p32(o, z ? (uint32_t)K32 : 0);       // сжатый размер
    p32(o, z ? (uint32_t)K32 : 0);       // исходный размер
    p16(o, (uint32_t)e.rel.size());
    p16(o, z ? 20 : 0);
    o += e.rel;
    if (z) { p16(o, 1); p16(o, 16); p64(o, 0); p64(o, 0); }
    return o;
}
size_t local_len(const ZipEntry& e) { return 30 + e.rel.size() + (big(e) ? 20 : 0); }
size_t desc_len(const ZipEntry& e) { return e.dir ? 0 : (big(e) ? 24 : 16); }

std::string central_header(const ZipEntry& e, uint32_t crc, uint64_t off) {
    std::string o;
    uint32_t dt, dd;
    dos_time(e.mtime, dt, dd);
    bool zs = big(e), zo = off >= K32;
    bool z = zs || zo;
    p32(o, 0x02014b50);
    p16(o, (3 << 8) | 45);
    p16(o, z ? 45 : 20);
    p16(o, e.dir ? 0x0800 : 0x0808);
    p16(o, 0);
    p16(o, dt); p16(o, dd);
    p32(o, crc);
    p32(o, zs ? (uint32_t)K32 : (uint32_t)e.size);
    p32(o, zs ? (uint32_t)K32 : (uint32_t)e.size);
    p16(o, (uint32_t)e.rel.size());
    p16(o, z ? ((zs ? 16 : 0) + (zo ? 8 : 0) + 4) : 0);
    p16(o, 0); p16(o, 0); p16(o, 0);
    uint32_t ext = ((e.dir ? 0040000u : 0100000u) | (e.mode & 07777)) << 16;
    if (e.dir) ext |= 0x10;
    p32(o, ext);
    p32(o, zo ? (uint32_t)K32 : (uint32_t)off);
    o += e.rel;
    if (z) {
        p16(o, 1);
        p16(o, (zs ? 16 : 0) + (zo ? 8 : 0));
        if (zs) { p64(o, e.size); p64(o, e.size); }
        if (zo) p64(o, off);
    }
    return o;
}
size_t central_len(const ZipEntry& e, uint64_t off) {
    bool zs = big(e), zo = off >= K32;
    return 46 + e.rel.size() + ((zs || zo) ? (zs ? 16 : 0) + (zo ? 8 : 0) + 4 : 0);
}

bool walk(const std::string& abs, const std::string& rel, ZipPlan& plan, std::string& err, int depth) {
    struct stat st;
    if (lstat(abs.c_str(), &st) != 0) { err = fs_errname(errno); return false; }
    if (S_ISLNK(st.st_mode) || (!S_ISREG(st.st_mode) && !S_ISDIR(st.st_mode))) { plan.skipped++; return true; }
    if (plan.entries.size() >= kMaxEntries) { err = "too_many"; return false; }
    ZipEntry e;
    e.abs = abs;
    e.mode = st.st_mode & 07777;
    e.mtime = (long long)st.st_mtime;
    if (S_ISDIR(st.st_mode)) {
        e.dir = true;
        e.rel = rel + "/";
        plan.entries.push_back(e);
        if (depth > 64) { plan.skipped++; return true; }
        DIR* d = opendir(abs.c_str());
        if (!d) { plan.skipped++; return true; }
        std::vector<std::string> kids;
        while (struct dirent* de = readdir(d)) {
            if (!strcmp(de->d_name, ".") || !strcmp(de->d_name, "..")) continue;
            kids.push_back(de->d_name);
        }
        closedir(d);
        std::sort(kids.begin(), kids.end());
        for (auto& k : kids) if (!walk(fs_join(abs, k), rel + "/" + k, plan, err, depth + 1)) return false;
    } else {
        e.rel = rel;
        e.size = (uint64_t)st.st_size;
        plan.entries.push_back(e);
    }
    return true;
}

}  // namespace

bool zip_plan(const std::string& base, const std::vector<std::string>& names, ZipPlan& plan, std::string& err) {
    plan = ZipPlan();
    if (base.empty() || names.empty()) { err = "bad_path"; return false; }
    for (auto& n : names) {
        if (n.empty() || n == "." || n == ".." || n.find('/') != std::string::npos) { err = "bad_path"; return false; }
        std::string abs = fs_join(base, n);
        std::string c = fs_check_read(abs);
        if (!c.empty()) { err = c; return false; }
        if (!walk(abs, n, plan, err, 0)) return false;
    }
    if (plan.entries.empty()) { err = "ENOENT"; return false; }
    uint64_t off = 0, cd = 0;
    for (auto& e : plan.entries) {
        cd += central_len(e, off);
        off += local_len(e) + e.size + desc_len(e);
    }
    uint64_t n = plan.entries.size();
    uint64_t tail = 22;
    if (n >= 0xFFFF || cd >= K32 || off >= K32) tail += 56 + 20;
    plan.total = off + cd + tail;
    return true;
}

bool zip_write(const ZipPlan& plan, const std::function<bool(const char*, size_t)>& sink) {
    if (!crc_ready) crc_init();
    std::vector<uint32_t> crcs(plan.entries.size(), 0);
    std::vector<uint64_t> offs(plan.entries.size(), 0);
    uint64_t pos = 0;
    std::vector<char> buf(256 * 1024);
    auto last_tick = std::chrono::steady_clock::now();
    auto out = [&](const char* p, size_t n) {
        if (!sink(p, n)) return false;
        pos += n;
        return true;
    };
    for (size_t i = 0; i < plan.entries.size(); i++) {
        const ZipEntry& e = plan.entries[i];
        offs[i] = pos;
        std::string lh = local_header(e);
        if (!out(lh.data(), lh.size())) return false;
        if (e.dir) continue;
        int f = open(e.abs.c_str(), O_RDONLY);
        uint64_t left = e.size;
        uint32_t crc = 0;
        if (f < 0) app_log("zip: не открыть %s (%s), записано нулями", e.abs.c_str(), fs_errname(errno).c_str());
        while (left > 0) {
            size_t want = (size_t)std::min<uint64_t>(left, buf.size());
            ssize_t r = f >= 0 ? read(f, buf.data(), want) : 0;
            if (r < 0 && errno == EINTR) continue;
            if (r <= 0) { memset(buf.data(), 0, want); r = (ssize_t)want; }  // файл стал короче: дополняем нулями
            crc = crc_update(crc, (const unsigned char*)buf.data(), (size_t)r);
            if (!out(buf.data(), (size_t)r)) { if (f >= 0) close(f); return false; }
            left -= (uint64_t)r;
            auto now = std::chrono::steady_clock::now();
            if (now - last_tick > std::chrono::seconds(5)) { keep_awake_tick(); last_tick = now; }
        }
        if (f >= 0) close(f);
        crcs[i] = crc;
        std::string dsc;
        p32(dsc, 0x08074b50);
        p32(dsc, crc);
        if (big(e)) { p64(dsc, e.size); p64(dsc, e.size); } else { p32(dsc, (uint32_t)e.size); p32(dsc, (uint32_t)e.size); }
        if (!out(dsc.data(), dsc.size())) return false;
    }
    uint64_t cd_start = pos, cd_size = 0;
    for (size_t i = 0; i < plan.entries.size(); i++) {
        std::string ch = central_header(plan.entries[i], crcs[i], offs[i]);
        cd_size += ch.size();
        if (!out(ch.data(), ch.size())) return false;
    }
    uint64_t n = plan.entries.size();
    std::string t;
    bool z = n >= 0xFFFF || cd_size >= K32 || cd_start >= K32;
    if (z) {
        uint64_t z64_pos = pos;
        p32(t, 0x06064b50); p64(t, 44); p16(t, (3 << 8) | 45); p16(t, 45); p32(t, 0); p32(t, 0);
        p64(t, n); p64(t, n); p64(t, cd_size); p64(t, cd_start);
        p32(t, 0x07064b50); p32(t, 0); p64(t, z64_pos); p32(t, 1);
    }
    p32(t, 0x06054b50); p16(t, 0); p16(t, 0);
    p16(t, z ? 0xFFFF : (uint32_t)n); p16(t, z ? 0xFFFF : (uint32_t)n);
    p32(t, z ? (uint32_t)K32 : (uint32_t)cd_size); p32(t, z ? (uint32_t)K32 : (uint32_t)cd_start);
    p16(t, 0);
    return out(t.data(), t.size());
}
