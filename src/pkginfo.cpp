// SPDX-License-Identifier: GPL-3.0-or-later
// Разбор PKG и правила планирования установки перенесены из ps4installer (src/pkg.cpp), чтение файла заменено на VFS.
#include "pkginfo.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <deque>
#include <map>
#include <set>

#include "fs.h"
#include "log.h"
#include "smb.h"
#include "vfs.h"

#ifndef HOST_TEST
extern "C" {
#include <ps4/kernel.h>
}
#endif

enum { PKG_TYPE_GD = 0x1A, PKG_TYPE_AC = 0x1B, PKG_TYPE_AL = 0x1C, PKG_TYPE_DP = 0x1E };

static uint32_t be32(const uint8_t* p) {
    return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3];
}

static uint64_t be64(const uint8_t* p) {
    return (uint64_t)be32(p) << 32 | be32(p + 4);
}

bool pi_parse_header(const uint8_t* buf, size_t len, PkgInfo* out, std::string* err) {
    if (len < 0x438) {
        *err = "файл слишком короткий для PKG";
        return false;
    }
    if (be32(buf) != 0x7F434E54) {  // "\x7FCNT"
        *err = "нет сигнатуры PKG";
        return false;
    }
    char cid[0x25];
    memcpy(cid, buf + 0x40, 0x24);
    cid[0x24] = 0;
    std::string content_id(cid);
    // формат: XXNNNN-CUSAnnnnn_00-XXXXXXXXXXXXXXXX (дефисы на позициях 6 и 19)
    if (content_id.size() != 36 || content_id[6] != '-' || content_id[19] != '-') {
        *err = "некорректный content id";
        return false;
    }
    for (char c : content_id) {
        bool ok = (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' || c == '_';
        if (!ok) {
            *err = "некорректный content id";
            return false;
        }
    }
    out->content_id = content_id;
    out->title_id = content_id.substr(7, 9);
    out->content_type = be32(buf + 0x74);
    out->flags = be32(buf + 0x78);
    out->pkg_size = be64(buf + 0x430);

    const uint32_t patch_mask = 0x00100000 | 0x40000000 | 0x41000000 | 0x60000000;
    out->is_patch = (out->flags & patch_mask) != 0;
    switch (out->content_type) {
        case PKG_TYPE_GD: out->kind = out->is_patch ? "patch" : "game"; break;
        case PKG_TYPE_AC: out->kind = "dlc"; break;
        case PKG_TYPE_AL: out->kind = "dlc_nodata"; break;
        case PKG_TYPE_DP: out->kind = "delta_patch"; break;
        default:
            *err = "неподдерживаемый тип пакета";
            return false;
    }
    return true;
}

static uint32_t le32(const uint8_t* p) {
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

static uint16_t le16(const uint8_t* p) { return (uint16_t)(p[0] | p[1] << 8); }

// Разбор param.sfo (формат PSF): ищем TITLE, APP_VER/VERSION. Любая неувязка - просто выходим.
static void parse_sfo(const std::vector<uint8_t>& d, PkgInfo* out) {
    if (d.size() < 20 || d[0] != 0 || d[1] != 'P' || d[2] != 'S' || d[3] != 'F') {
        return;
    }
    uint32_t key_off = le32(&d[8]), data_off = le32(&d[12]), n = le32(&d[16]);
    if (n > 256) {
        return;
    }
    std::string version;
    for (uint32_t i = 0; i < n; i++) {
        size_t e = 20 + (size_t)i * 16;
        if (e + 16 > d.size()) {
            return;
        }
        size_t k = (size_t)key_off + le16(&d[e]);
        uint32_t len = le32(&d[e + 4]);
        size_t v = (size_t)data_off + le32(&d[e + 12]);
        if (k >= d.size() || v >= d.size() || len > 4096) {
            continue;
        }
        std::string key;
        for (size_t j = k; j < d.size() && d[j]; j++) {
            key += (char)d[j];
        }
        std::string val;
        for (size_t j = v; j < d.size() && j < v + len && d[j]; j++) {
            val += (char)d[j];
        }
        if (key == "TITLE") {
            out->title = val;
        } else if (key == "APP_VER") {
            out->app_ver = val;
        } else if (key == "VERSION") {
            version = val;
        }
    }
    if (out->app_ver.empty()) {
        out->app_ver = version;
    }
    int a = 0, b = 0;
    if (sscanf(out->app_ver.c_str(), "%d.%d", &a, &b) == 2) {
        out->ver_num = a * 100 + b;
    }
}

// Слова из строки: буквы, цифры и байты UTF-8 (кириллица), остальное - разделители.
static std::vector<std::string> words_of(const std::string& s) {
    std::vector<std::string> w;
    std::string cur;
    for (unsigned char c : s) {
        if (isalnum(c) || c >= 0x80) {
            cur += (char)(c < 0x80 ? tolower(c) : c);
        } else if (!cur.empty()) {
            w.push_back(cur);
            cur.clear();
        }
    }
    if (!cur.empty()) {
        w.push_back(cur);
    }
    return w;
}

static bool has_word(const std::vector<std::string>& w, const char* a, const char* b = nullptr,
                     const char* c = nullptr, const char* d = nullptr) {
    for (const std::string& x : w) {
        if (x == a || (b && x == b) || (c && x == c) || (d && x == d)) {
            return true;
        }
    }
    return false;
}

void pi_name_tags(PkgInfo* p) {
    // имя файла без папок и расширения + имя ближайшей папки
    std::string path = p->path;
    size_t slash = path.rfind('/');
    std::string file = slash == std::string::npos ? path : path.substr(slash + 1);
    std::string dir;
    if (slash != std::string::npos && slash > 0) {
        size_t s2 = path.rfind('/', slash - 1);
        dir = path.substr(s2 == std::string::npos ? 0 : s2 + 1, slash - (s2 == std::string::npos ? 0 : s2 + 1));
    }
    size_t dot = file.rfind('.');
    if (dot != std::string::npos) {
        file.resize(dot);
    }
    std::vector<std::string> w = words_of(file);
    std::vector<std::string> wd = words_of(dir);
    w.insert(w.end(), wd.begin(), wd.end());
    bool back_port = false;
    for (size_t i = 0; i + 1 < w.size(); i++) {
        back_port = back_port || (w[i] == "back" && w[i + 1] == "port");
    }
    p->backport = back_port || has_word(w, "backport", "backports", "bp");
    p->rus = has_word(w, "rus", "ru", "russian", "rusifikator") ||
             has_word(w, "русификатор", "рус", "русский", "русская") ||
             has_word(w, "Русификатор", "РУС", "Русский", "Русская");
    p->dlc_name = has_word(w, "dlc", "dlcs", "expansion", "addon");
}

// base_titles - игры (без пометки русификатора), которые есть в этой же пачке: если у пакета с
// пометкой RUS есть такая игра, он точно не основа, даже если в заголовке записан как "game".
static int rank_of(const PkgInfo& p, const std::map<std::string, bool>& base_titles) {
    bool has_base = base_titles.count(p.title_id) > 0;
    bool extra = p.kind != "game" || has_base;
    if (p.rus && extra) {
        return 3;
    }
    if (p.kind == "dlc" || p.kind == "dlc_nodata" || p.dlc_name) {
        return 2;
    }
    if (p.is_patch || p.kind == "delta_patch") {
        return 1;
    }
    return 0;
}

std::vector<PkgInfo> pi_plan(const std::vector<PkgInfo>& in, int fw_major, std::vector<PkgSkip>* skipped) {
    std::map<std::string, bool> base_titles;
    for (const PkgInfo& p : in) {
        if (p.kind == "game" && !p.is_patch && !p.rus) {
            base_titles[p.title_id] = true;
        }
    }
    auto rank_of = [&](const PkgInfo& p) { return ::rank_of(p, base_titles); };
    std::vector<PkgInfo> keep;
    for (const PkgInfo& p : in) {
        if (p.backport && (fw_major < 0 || fw_major > 9)) {
            if (skipped) {
                skipped->push_back({p, "backport_fw"});
            }
            continue;
        }
        keep.push_back(p);
    }
    // из обычных патчей одной игры оставляем самый новый (если версия известна)
    std::map<std::string, int> newest;
    for (const PkgInfo& p : keep) {
        if (rank_of(p) == 1 && p.kind == "patch" && p.ver_num >= 0) {
            int& v = newest[p.title_id];
            v = std::max(v, p.ver_num);
        }
    }
    std::vector<PkgInfo> kept2;
    for (const PkgInfo& p : keep) {
        if (rank_of(p) == 1 && p.kind == "patch" && p.ver_num >= 0 && p.ver_num < newest[p.title_id]) {
            if (skipped) {
                char vb[32];
                snprintf(vb, sizeof(vb), "newer_patch:%d.%02d", newest[p.title_id] / 100, newest[p.title_id] % 100);
                skipped->push_back({p, vb});
            }
            continue;
        }
        kept2.push_back(p);
    }
    // порядок игр: по размеру самой большой "основы" (если игры в списке нет - по самому большому файлу)
    std::map<std::string, uint64_t> anchor;
    for (const PkgInfo& p : kept2) {
        uint64_t& a = anchor[p.title_id];
        if (rank_of(p) == 0) {
            a = std::max<uint64_t>(a, p.file_size | (1ULL << 62));  // основа важнее любого другого файла
        } else {
            a = std::max<uint64_t>(a, p.file_size);
        }
    }
    std::vector<PkgInfo> out = kept2;
    std::stable_sort(out.begin(), out.end(), [&](const PkgInfo& a, const PkgInfo& b) {
        if (a.title_id != b.title_id) {
            uint64_t x = anchor[a.title_id], y = anchor[b.title_id];
            if (x != y) {
                return x > y;
            }
            return a.title_id < b.title_id;
        }
        int ra = rank_of(a), rb = rank_of(b);
        if (ra != rb) {
            return ra < rb;
        }
        if (ra == 1 && a.ver_num >= 0 && b.ver_num >= 0 && a.ver_num != b.ver_num) {
            return a.ver_num < b.ver_num;
        }
        if (a.file_size != b.file_size) {
            return a.file_size > b.file_size;
        }
        return a.path < b.path;
    });
    return out;
}

// ---- чтение через VFS ----

static bool read_full(VFile& f, uint64_t off, void* buf, size_t n) {
    size_t got = 0;
    while (got < n) {
        long r = v_pread(f, off + got, (char*)buf + got, n - got);
        if (r <= 0) return false;
        got += (size_t)r;
    }
    return true;
}

static void read_sfo(VFile& f, uint64_t file_size, const uint8_t* hdr, PkgInfo* out) {
    uint32_t count = be32(hdr + 0x10), table = be32(hdr + 0x18);
    if (count == 0 || count > 4096 || table == 0 || table >= file_size) return;
    std::vector<uint8_t> tab((size_t)count * 32);
    if (!read_full(f, table, tab.data(), tab.size())) return;
    for (uint32_t i = 0; i < count; i++) {
        const uint8_t* e = &tab[(size_t)i * 32];
        if (be32(e) != 0x1000) continue;
        uint32_t off = be32(e + 16), size = be32(e + 20);
        if (size == 0 || size > 0x10000 || (uint64_t)off + size > file_size) return;
        std::vector<uint8_t> d(size);
        if (!read_full(f, off, d.data(), size)) return;
        parse_sfo(d, out);
        return;
    }
}

bool pi_read(const std::string& path, uint64_t file_size, PkgInfo& out, std::string& err) {
    VFile f;
    int oe = v_open_read(path, f);
    if (oe) { err = fs_errname(oe); return false; }
    uint8_t buf[0x1000];
    size_t n = 0;
    while (n < sizeof(buf)) {
        long r = v_pread(f, n, buf + n, sizeof(buf) - n);
        if (r <= 0) break;
        n += (size_t)r;
    }
    out = PkgInfo();
    out.path = path;
    out.file_size = file_size;
    std::string e;
    bool ok = pi_parse_header(buf, n, &out, &e);
    if (!ok) { err = "not_pkg"; v_close(f); return false; }
    read_sfo(f, file_size, buf, &out);
    v_close(f);
    pi_name_tags(&out);
    return true;
}

int pi_fw_major() {
#ifdef HOST_TEST
    return 9;
#else
    uint32_t v = kernel_get_fw_version() >> 24;   // 0x09 -> 9, 0x13 -> 13 (BCD)
    return (int)((v >> 4) * 10 + (v & 15));
#endif
}

// ---- поиск связанных пакетов ----

namespace {

struct Ent { std::string name; bool dir; uint64_t size; };

int list_ents(const std::string& dir, std::vector<Ent>& out) {
    out.clear();
    if (smb_is_path(dir)) {
        std::vector<SmbEnt> e;
        int r = smb_list_dir(dir, e);
        if (r) return r;
        for (auto& x : e) out.push_back({x.name, x.st.dir, x.st.size});
        return 0;
    }
    std::vector<std::string> names;
    int r = v_list(dir, names);
    if (r) return r;
    for (auto& n : names) {
        VStat st;
        if (v_lstat(fs_join(dir, n), st) != 0) continue;
        out.push_back({n, st.dir, st.size});
    }
    return 0;
}

bool skip_dir(const std::string& n) {
    return n.empty() || n[0] == '.' || n == "$RECYCLE.BIN" || n == "System Volume Information" || n == "disc";
}
bool is_pkg_name(const std::string& n) { return n.size() > 4 && strcasecmp(n.c_str() + n.size() - 4, ".pkg") == 0; }

struct Scan {
    std::string title_id;
    std::vector<PkgInfo> found;
    int headers = 0;
    bool truncated = false;
    time_t deadline;
    std::set<std::string> seen_dirs;
};
const int kMaxHeaders = 400, kMaxDepth = 3, kMaxSecs = 25;

void scan_dir(Scan& sc, const std::string& root, const std::string& skip) {
    std::deque<std::pair<std::string, int>> q;
    q.push_back({root, 0});
    while (!q.empty()) {
        auto [dir, depth] = q.front();
        q.pop_front();
        if (dir == skip || !sc.seen_dirs.insert(dir).second) continue;
        std::vector<Ent> ents;
        if (list_ents(dir, ents) != 0) continue;
        std::sort(ents.begin(), ents.end(), [](const Ent& a, const Ent& b) { return a.name < b.name; });
        for (auto& e : ents) {
            if (e.dir) { if (depth < kMaxDepth && !skip_dir(e.name)) q.push_back({fs_join(dir, e.name), depth + 1}); continue; }
            if (!is_pkg_name(e.name) || e.size < 0x1000) continue;
            if (sc.headers >= kMaxHeaders || time(nullptr) > sc.deadline) { sc.truncated = true; return; }
            sc.headers++;
            PkgInfo p; std::string err;
            if (!pi_read(fs_join(dir, e.name), e.size, p, err)) continue;
            if (p.title_id == sc.title_id) sc.found.push_back(p);
        }
    }
}

std::string esc(const std::string& s) {
    std::string o;
    for (unsigned char c : s) {
        if (c == '"' || c == '\\') { o += '\\'; o += (char)c; }
        else if (c < 0x20) { char b[8]; snprintf(b, sizeof(b), "\\u%04x", c); o += b; }
        else o += (char)c;
    }
    return o;
}

int rank_of(const PkgInfo& p, bool has_base) {
    bool extra = p.kind != "game" || has_base;
    if (p.rus && extra) return 3;
    if (p.kind == "dlc" || p.kind == "dlc_nodata" || p.dlc_name) return 2;
    if (p.is_patch || p.kind == "delta_patch") return 1;
    return 0;
}

void item_json(std::string& o, const PkgInfo& p, int rank, bool checked, const std::string& why, bool self) {
    if (o.back() != '[') o += ',';
    o += "{\"p\":\"" + esc(p.path) + "\",\"n\":\"" + esc(fs_basename(p.path)) + "\",\"k\":\"" + p.kind + "\",\"r\":" + std::to_string(rank) +
         ",\"v\":\"" + esc(p.app_ver) + "\",\"t\":\"" + esc(p.title) + "\",\"s\":" + std::to_string((unsigned long long)p.file_size) +
         ",\"c\":" + (checked ? "true" : "false") + ",\"self\":" + (self ? "true" : "false") + ",\"why\":\"" + esc(why) + "\"" +
         ",\"bp\":" + (p.backport ? "true" : "false") + ",\"rus\":" + (p.rus ? "true" : "false") + "}";
}

}  // namespace

std::string pi_related_json(const std::string& path, std::string& err) {
    VStat st;
    int e = v_lstat(path, st);
    if (e) { err = fs_errname(e); return ""; }
    PkgInfo self;
    if (!pi_read(path, st.size, self, err)) {
        if (err == "not_pkg") { err.clear(); return "{\"items\":[],\"tid\":\"\",\"title\":\"\",\"trunc\":false,\"scanned\":0}"; }   // не разобрался: окно выбора не нужно, ошибку покажет сама установка
        return "";
    }
    Scan sc;
    sc.title_id = self.title_id;
    sc.deadline = time(nullptr) + kMaxSecs;
    std::string dir = fs_parent(path), parent = fs_parent(dir);
    scan_dir(sc, dir, "");
    // Соседние папки: на уровень выше, но не выше корня диска или шары (там лежит всё подряд).
    int comps = 0;
    for (char c : parent) if (c == '/') comps++;
    bool smb = smb_is_path(parent);
    if (parent != dir && comps >= (smb ? 3 : 2)) scan_dir(sc, parent, dir);

    // себя ставим первым в список, дубликаты (тот же Content ID, тип и размер) отбрасываем
    std::vector<PkgInfo> all;
    std::set<std::string> keys;
    auto add = [&](const PkgInfo& p) {
        std::string k = p.content_id + "/" + std::to_string(p.content_type) + "/" + std::to_string((unsigned long long)p.file_size) + "/" + p.app_ver + (p.is_patch ? "p" : "");
        if (keys.insert(k).second) all.push_back(p);
    };
    add(self);
    for (auto& p : sc.found) if (p.path != self.path) add(p);

    std::vector<PkgSkip> skipped;
    std::vector<PkgInfo> ok;
    for (auto& p : all) {
        if (p.incomplete() && p.path != self.path) { skipped.push_back({p, "incomplete"}); continue; }
        ok.push_back(p);
    }
    std::vector<PkgSkip> skipped2;
    std::vector<PkgInfo> plan = pi_plan(ok, pi_fw_major(), &skipped2);
    skipped.insert(skipped.end(), skipped2.begin(), skipped2.end());
    // пакет, на который нажал пользователь, ставится всегда
    bool self_planned = false;
    for (auto& p : plan) if (p.path == self.path) self_planned = true;
    std::string forced_why;
    if (!self_planned) {
        for (auto it = skipped.begin(); it != skipped.end(); ++it)
            if (it->pkg.path == self.path) { forced_why = it->reason; skipped.erase(it); break; }
        plan.push_back(self);
    }
    bool has_base = false;
    for (auto& p : plan) if (p.kind == "game" && !p.is_patch && !p.rus) has_base = true;
    if (!self_planned)
        std::stable_sort(plan.begin(), plan.end(), [&](const PkgInfo& a, const PkgInfo& b) {
            int ra = rank_of(a, has_base), rb = rank_of(b, has_base);
            if (ra != rb) return ra < rb;
            if (ra == 1 && a.ver_num != b.ver_num) return a.ver_num < b.ver_num;
            return a.file_size > b.file_size;
        });
    std::string o = "{\"title\":\"" + esc(self.title) + "\",\"tid\":\"" + esc(self.title_id) + "\",\"trunc\":" + (sc.truncated ? "true" : "false") +
                    ",\"scanned\":" + std::to_string(sc.headers) + ",\"items\":[";
    for (auto& p : plan) item_json(o, p, rank_of(p, has_base), true, p.path == self.path ? forced_why : "", p.path == self.path);
    for (auto& s : skipped) item_json(o, s.pkg, rank_of(s.pkg, has_base), false, s.reason, false);
    o += "]}";
    return o;
}
