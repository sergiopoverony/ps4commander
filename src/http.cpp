#include "http.h"
#include "assets_gen.h"
#include "fs.h"
#include "install.h"
#include "smb.h"
#include "vfs.h"
#include "jobs.h"
#include "json.h"
#include "log.h"
#include "zip.h"
#include "platform.h"
#include "version.h"

#include <algorithm>
#include <arpa/inet.h>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <netinet/in.h>
#include <csignal>
#include <poll.h>
#include <string>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <thread>
#include <unistd.h>

static const int kMaxClients = 8;
static const size_t kMaxHeader = 16 * 1024;
static const size_t kMaxBody = 256 * 1024;  // тело JSON (список файлов для задания)
static std::atomic<int> g_clients{0};
static std::atomic<bool> g_quit{false};

static std::string quit_flag_path() { return std::string(DATA_DIR) + "/quit.flag"; }

static bool send_all(int fd, const char* p, size_t n) {
    size_t off = 0;
    while (off < n) {
        #ifdef MSG_NOSIGNAL
        ssize_t r = send(fd, p + off, n - off, MSG_NOSIGNAL);
#else
        ssize_t r = send(fd, p + off, n - off, 0);
#endif
        if (r < 0 && errno == EINTR) continue;
        if (r <= 0) return false;
        off += (size_t)r;
    }
    return true;
}

static const char* reason(int code) {
    return code == 200 ? "OK" : code == 400 ? "Bad Request" : code == 403 ? "Forbidden" : code == 404 ? "Not Found"
         : code == 206 ? "Partial Content" : code == 416 ? "Range Not Satisfiable" : code == 405 ? "Method Not Allowed" : code == 409 ? "Conflict" : code == 411 ? "Length Required" : "Error";
}

static void send_head(int fd, int code, const char* ctype, long long len, bool cache, const std::string& extra = "") {
    char head[2048];
    int n = snprintf(head, sizeof(head),
                     "HTTP/1.1 %d %s\r\nContent-Type: %s\r\nContent-Length: %lld\r\nConnection: close\r\n"
                     "X-Content-Type-Options: nosniff\r\nCache-Control: %s\r\n%s\r\n",
                     code, reason(code), ctype, len, cache ? "max-age=86400" : "no-store", extra.c_str());
    send_all(fd, head, (size_t)n);
}
static void respond(int fd, int code, const char* ctype, const char* body, size_t len, bool cache = false) {
    send_head(fd, code, ctype, (long long)len, cache);
    send_all(fd, body, len);
}
static void respond_json(int fd, int code, const std::string& s) { respond(fd, code, "application/json; charset=utf-8", s.data(), s.size()); }
static void respond_err(int fd, int code, const std::string& e) { respond_json(fd, code, "{\"error\":\"" + json_esc(e) + "\"}"); }
static int err_status(const std::string& e) {
    if (e == "ENOENT") return 404;
    if (e == "busy") return 409;
    if (e == "EACCES" || e == "EPERM" || e == "protected" || e == "unsafe" || e == "outside_root") return 403;
    return 400;
}

static std::string url_decode(const std::string& s) {
    std::string o;
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '%' && i + 2 < s.size() + 0 && isxdigit((unsigned char)s[i + 1]) && isxdigit((unsigned char)s[i + 2])) {
            o += (char)strtol(s.substr(i + 1, 2).c_str(), nullptr, 16);
            i += 2;
        } else if (s[i] == '+') o += ' ';
        else o += s[i];
    }
    return o;
}
static std::string qget(const std::string& q, const char* key) {
    size_t i = 0, kl = strlen(key);
    while (i < q.size()) {
        size_t j = q.find('&', i);
        if (j == std::string::npos) j = q.size();
        if (j - i > kl && q.compare(i, kl, key) == 0 && q[i + kl] == '=') return url_decode(q.substr(i + kl + 1, j - i - kl - 1));
        i = j + 1;
    }
    return "";
}
static std::string pct_encode(const std::string& s) {
    static const char* hex = "0123456789ABCDEF";
    std::string o;
    for (unsigned char c : s) {
        if (isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') o += (char)c;
        else { o += '%'; o += hex[c >> 4]; o += hex[c & 15]; }
    }
    return o;
}

// Защита от подмены DNS (DNS rebinding): имя узла в заголовке Host должно быть IP-адресом или localhost.
static bool host_ok(const std::string& lower_head) {
    size_t p = lower_head.find("\r\nhost:");
    if (p == std::string::npos) return true;  // HTTP/1.0 без Host
    p += 7;
    size_t e = lower_head.find("\r\n", p);
    std::string h = lower_head.substr(p, e == std::string::npos ? std::string::npos : e - p);
    size_t a = h.find_first_not_of(" \t");
    if (a == std::string::npos) return false;
    h = h.substr(a);
    if (h[0] == '[') return true;  // IPv6-литерал
    size_t c = h.find(':');
    if (c != std::string::npos) h.resize(c);
    while (!h.empty() && (h.back() == ' ' || h.back() == '\t')) h.pop_back();
    if (h == "localhost") return true;
    if (h.empty()) return false;
    for (char ch : h) if (!(ch >= '0' && ch <= '9') && ch != '.') return false;
    return true;
}

static std::string path_param(const std::string& q, std::string& err) {
    std::string raw = qget(q, "path");
    std::string n = fs_normalize(raw);
    if (n.empty()) err = "bad_path";
    return n;
}

static const char* inline_type(const std::string& name) {
    size_t d = name.rfind('.');
    if (d == std::string::npos) return nullptr;
    std::string e = name.substr(d + 1);
    for (char& c : e) c = (char)tolower((unsigned char)c);
    // Только безопасные медиа-типы: HTML и SVG не отдаём, чтобы файл не выполнял скрипты на нашем адресе.
    if (e == "png") return "image/png";
    if (e == "jpg" || e == "jpeg") return "image/jpeg";
    if (e == "gif") return "image/gif";
    if (e == "webp") return "image/webp";
    if (e == "bmp") return "image/bmp";
    if (e == "mp3") return "audio/mpeg";
    if (e == "ogg") return "audio/ogg";
    if (e == "wav") return "audio/wav";
    if (e == "mp4" || e == "m4v") return "video/mp4";
    if (e == "webm") return "video/webm";
    return nullptr;
}

// Разбор "bytes=a-b" / "bytes=a-" / "bytes=-n". false - заголовок не годится или диапазон за пределами файла.
static bool parse_range(const std::string& h, uint64_t total, uint64_t& a, uint64_t& b) {
    if (h.compare(0, 6, "bytes=") != 0 || h.find(',') != std::string::npos) return false;
    std::string v = h.substr(6);
    size_t dash = v.find('-');
    if (dash == std::string::npos) return false;
    std::string x = v.substr(0, dash), y = v.substr(dash + 1);
    for (char c : x + y) if (c < '0' || c > '9') return false;
    if (x.empty() && y.empty()) return false;
    if (total == 0) return false;
    if (x.empty()) {  // последние n байт
        uint64_t n = strtoull(y.c_str(), nullptr, 10);
        if (n == 0) return false;
        a = n >= total ? 0 : total - n; b = total - 1;
    } else {
        a = strtoull(x.c_str(), nullptr, 10);
        b = y.empty() ? total - 1 : strtoull(y.c_str(), nullptr, 10);
        if (b >= total) b = total - 1;
        if (a > b) return false;
    }
    return true;
}

static void do_download(int fd, const std::string& n, bool inl, const std::string& range, bool head_only) {
    std::string e = fs_check_read(n);
    if (!e.empty()) { respond_err(fd, 403, e); return; }
    VStat st;
    int le = v_lstat(n, st);
    if (le) { std::string en = fs_errname(le); respond_err(fd, err_status(en), en); return; }
    VFile f;
    int oe = v_open_read(n, f);
    if (oe) { std::string en = fs_errname(oe); respond_err(fd, err_status(en), en); return; }
    if (!smb_is_path(n)) {
        struct stat fst;
        if (fstat(f.fd, &fst) != 0 || !S_ISREG(fst.st_mode)) { v_close(f); respond_err(fd, 400, "not_regular"); return; }
        st.size = (uint64_t)fst.st_size;
    } else if (st.dir) { v_close(f); respond_err(fd, 400, "not_regular"); return; }
    uint64_t a = 0, b = st.size ? st.size - 1 : 0;
    int code = 200;
    if (!range.empty()) {
        if (parse_range(range, st.size, a, b)) code = 206;
        else if (st.size > 0) {
            v_close(f);
            char cr[96];
            snprintf(cr, sizeof(cr), "Content-Range: bytes */%llu\r\n", (unsigned long long)st.size);
            send_head(fd, 416, "text/plain", 0, false, cr);
            return;
        }
    }
    uint64_t len = st.size == 0 ? 0 : b - a + 1;
    const char* it = inl ? inline_type(fs_basename(n)) : nullptr;
    std::string extra = it ? "" : "Content-Disposition: attachment; filename*=UTF-8''" + pct_encode(fs_basename(n)) + "\r\n";
    extra += "Accept-Ranges: bytes\r\n";
    if (code == 206) {
        char cr[96];
        snprintf(cr, sizeof(cr), "Content-Range: bytes %llu-%llu/%llu\r\n", (unsigned long long)a, (unsigned long long)b, (unsigned long long)st.size);
        extra += cr;
    }
    send_head(fd, code, it ? it : "application/octet-stream", (long long)len, false, extra);
    if (head_only || len == 0) { v_close(f); return; }
    v_seek(f, a);
    std::vector<char> buf(256 * 1024);
    uint64_t sent = 0;
    int tk = 0;
    while (sent < len) {
        long r = v_read(f, buf.data(), std::min<uint64_t>(buf.size(), len - sent));
        if (r <= 0) break;
        if (!send_all(fd, buf.data(), (size_t)r)) break;
        sent += (uint64_t)r;
        if (smb_is_path(n) && ++tk % 64 == 0) keep_awake_tick();
    }
    v_close(f);
}

// Положение панелей хранится на консоли, чтобы было одинаковым с любого устройства.
static std::string state_path() { return std::string(DATA_DIR) + "/state.json"; }
static std::string state_read() {
    FILE* f = fopen(state_path().c_str(), "rb");
    if (!f) return "{}";
    char b[4096];
    size_t n = fread(b, 1, sizeof(b) - 1, f);
    fclose(f);
    std::string s(b, n);
    JsonObj j = json_parse(s);
    if (!j.ok) return "{}";
    return "{\"l\":\"" + json_esc(j.get_str("l")) + "\",\"r\":\"" + json_esc(j.get_str("r")) + "\"}";
}
static bool state_write(const std::string& l, const std::string& r) {
    std::string tmp = state_path() + ".tmp";
    FILE* f = fopen(tmp.c_str(), "wb");
    if (!f) return false;
    std::string o = "{\"l\":\"" + json_esc(l) + "\",\"r\":\"" + json_esc(r) + "\"}";
    bool ok = fwrite(o.data(), 1, o.size(), f) == o.size();
    ok = fclose(f) == 0 && ok;
    if (!ok) { unlink(tmp.c_str()); return false; }
    return rename(tmp.c_str(), state_path().c_str()) == 0;
}

// Папка (или несколько имён из одного каталога) одним ZIP-архивом, собираемым на лету.
static void do_zip(int fd, const std::string& query) {
    std::string err;
    std::string dir = fs_normalize(qget(query, "dir"));
    if (dir.empty()) { respond_err(fd, 400, "bad_path"); return; }
    std::vector<std::string> names;
    size_t i = 0;
    while (i < query.size()) {
        size_t j = query.find('&', i);
        if (j == std::string::npos) j = query.size();
        if (j - i > 2 && query.compare(i, 2, "n=") == 0) names.push_back(url_decode(query.substr(i + 2, j - i - 2)));
        i = j + 1;
    }
    ZipPlan plan;
    if (!zip_plan(dir, names, plan, err)) { respond_err(fd, err_status(err), err); return; }
    std::string nm = names.size() == 1 ? names[0] : (dir == "/" ? std::string("archive") : fs_basename(dir));
    app_log("zip: %s (%d записей, %lld байт)", nm.c_str(), (int)plan.entries.size(), (long long)plan.total);
    send_head(fd, 200, "application/zip", (long long)plan.total, false,
              "Content-Disposition: attachment; filename*=UTF-8''" + pct_encode(nm + ".zip") + "\r\n");
    bool ok = zip_write(plan, [&](const char* p, size_t n) { return send_all(fd, p, n); });
    if (!ok) app_log("zip: клиент прервал скачивание %s", nm.c_str());
}

// Загрузка файла с компьютера: тело запроса пишется прямо в файл (рядом с ним, потом переименование).
static void do_upload(int fd, const std::string& query, size_t content_len, const std::string& already) {
    std::string err;
    std::string n = path_param(query, err);
    bool overwrite = qget(query, "overwrite") == "1";
    bool unsafe = qget(query, "unsafe") == "1";
    if (n.empty()) { respond_err(fd, 400, err); return; }
    std::string c = fs_check_modify(n, unsafe, true);
    if (c.empty() && fs_danger(n) == 2) c = "protected";
    if (!c.empty()) { respond_err(fd, err_status(c), c); return; }
    VStat st;
    bool exists = v_lstat(n, st) == 0;
    if (exists && !overwrite) { respond_err(fd, 409, "EEXIST"); return; }
    if (exists && st.dir) { respond_err(fd, 400, "EISDIR"); return; }
    std::string tmp = n + ".part";
    VFile f;
    int oe = v_open_write(tmp, f, 0666);
    if (oe) { std::string en = fs_errname(oe); respond_err(fd, err_status(en), en); return; }
    size_t got = 0;
    bool ok = true;
    int werr = 0;
    auto put = [&](const char* p, size_t len) {
        size_t off = 0;
        while (off < len) {
            long w = v_write(f, p + off, len - off);
            if (w <= 0) { ok = false; werr = w < 0 ? (int)-w : EIO; return; }
            off += (size_t)w;
        }
        got += len;
    };
    size_t take = std::min(already.size(), content_len);
    put(already.data(), take);
    std::vector<char> buf(256 * 1024);
    int last_tick = 0;
    while (ok && got < content_len) {
        ssize_t r = recv(fd, buf.data(), std::min(buf.size(), content_len - got), 0);
        if (r < 0 && errno == EINTR) continue;
        if (r <= 0) { ok = false; werr = EIO; break; }
        put(buf.data(), (size_t)r);
        if (++last_tick % 64 == 0) keep_awake_tick();
    }
    v_close(f);
    if (!ok || got != content_len) {
        v_unlink(tmp);
        respond_err(fd, 400, ok ? "short_body" : fs_errname(werr));
        return;
    }
    if (exists && !smb_is_path(n)) v_chmod(tmp, st.mode & 07777);  // перезапись (сохранение из редактора) не должна менять права
    if (exists && smb_is_path(n)) v_unlink(n);
    int re = v_rename(tmp, n);
    if (re) {
        std::string en = fs_errname(re);
        v_unlink(tmp);
        respond_err(fd, 400, en);
        return;
    }
    app_log("загружен файл %s (%zu байт)", n.c_str(), got);
    respond_json(fd, 200, "{\"ok\":true}");
}

static std::vector<std::string> norm_list(const std::vector<std::string>* in) {
    std::vector<std::string> out;
    if (!in) return out;
    for (auto& s : *in) {
        std::string n = fs_normalize(s);
        if (n.empty()) { out.clear(); out.push_back(""); return out; }
        out.push_back(n);
    }
    return out;
}

static void handle_post(int fd, const std::string& path, const std::string& body) {
    JsonObj j = json_parse(body);
    if (!j.ok) { respond_err(fd, 400, "bad_json"); return; }
    bool unsafe = j.get_flag("unsafe");
    std::string err;
    if (path == "/api/mkdir") {
        std::string n = fs_normalize(j.get_str("path"));
        if (n.empty()) { respond_err(fd, 400, "bad_path"); return; }
        if (fs_mkdir(n, unsafe, err)) { app_log("создана папка %s", n.c_str()); respond_json(fd, 200, "{\"ok\":true}"); }
        else respond_err(fd, err_status(err), err);
    } else if (path == "/api/rename") {
        std::string a = fs_normalize(j.get_str("path")), b = fs_normalize(j.get_str("to"));
        if (a.empty() || b.empty()) { respond_err(fd, 400, "bad_path"); return; }
        if (fs_rename(a, b, unsafe, err)) { app_log("переименовано %s -> %s", a.c_str(), b.c_str()); respond_json(fd, 200, "{\"ok\":true}"); }
        else respond_err(fd, err_status(err), err);
    } else if (path == "/api/job/start") {
        JobSpec s;
        s.op = j.get_str("op");
        s.srcs = norm_list(j.get_arr("paths"));
        if (!s.srcs.empty() && s.srcs[0].empty()) { respond_err(fd, 400, "bad_path"); return; }
        if (j.str.count("dest")) {
            s.dest = fs_normalize(j.get_str("dest"));
            if (s.dest.empty()) { respond_err(fd, 400, "bad_path"); return; }
        }
        s.overwrite = j.get_flag("overwrite");
        s.unsafe = unsafe;
        s.recursive = j.get_flag("recursive");
        std::string m = j.get_str("mode");
        if (s.op == "chmod") {
            char* end = nullptr;
            long v = strtol(m.c_str(), &end, 8);
            if (m.empty() || *end || v < 0 || v > 07777) { respond_err(fd, 400, "bad_mode"); return; }
            s.mode = (int)v;
        }
        if (job_start(s, err)) { app_log("запущено задание %s", s.op.c_str()); respond_json(fd, 200, "{\"ok\":true}"); }
        else respond_err(fd, err_status(err), err);
    } else if (path == "/api/pkg/install") {
        std::vector<std::string> ps = norm_list(j.get_arr("paths"));
        for (auto& x : ps) if (x.empty()) { respond_err(fd, 400, "bad_path"); return; }
        if (install_start(ps, err)) { app_log("установка pkg: %d шт.", (int)ps.size()); respond_json(fd, 200, "{\"ok\":true}"); }
        else respond_err(fd, err_status(err), err);
    } else if (path == "/api/state") {
        // l - левая панель, r - правая; отсутствующее поле оставляет прежнее значение
        JsonObj old = json_parse(state_read());
        std::string l = old.get_str("l"), r = old.get_str("r");
        if (j.str.count("l")) { l = fs_normalize(j.get_str("l")); }
        if (j.str.count("r")) { r = fs_normalize(j.get_str("r")); }
        if (state_write(l, r)) respond_json(fd, 200, "{\"ok\":true}"); else respond_err(fd, 400, "write_failed");
    } else if (path == "/api/smb/save") {
        SmbConn c;
        c.alias = j.get_str("alias"); c.host = j.get_str("host"); c.share = j.get_str("share");
        c.user = j.get_str("user"); c.pass = j.get_str("pass"); c.domain = j.get_str("domain");
        if (smb_save(c, err)) respond_json(fd, 200, "{\"ok\":true}");
        else respond_err(fd, err == "write_failed" ? 500 : 400, err);
    } else if (path == "/api/smb/del") {
        if (smb_remove(j.get_str("alias"))) respond_json(fd, 200, "{\"ok\":true}");
        else respond_err(fd, 404, "ENOENT");
    } else if (path == "/api/pkg/cancel") {
        install_cancel();
        respond_json(fd, 200, "{\"ok\":true}");
    } else if (path == "/api/job/cancel") {
        job_cancel();
        respond_json(fd, 200, "{\"ok\":true}");
    } else if (path == "/quit") {
        respond_json(fd, 200, "{\"ok\":true}");
        g_quit = true;
    } else respond_err(fd, 404, "not_found");
}

static void handle(int fd) {
    std::string req;
    char buf[4096];
    size_t head_end = std::string::npos;
    while (head_end == std::string::npos && req.size() < kMaxHeader) {
        ssize_t n = recv(fd, buf, sizeof(buf), 0);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) { close(fd); return; }
        req.append(buf, (size_t)n);
        head_end = req.find("\r\n\r\n");
    }
    if (head_end == std::string::npos) { respond_err(fd, 400, "bad_request"); close(fd); return; }

    std::string head = req.substr(0, head_end);
    std::string lower = head;
    for (char& c : lower) c = (char)tolower((unsigned char)c);
    if (!host_ok(lower)) { respond_err(fd, 403, "forbidden"); close(fd); return; }
    size_t sp1 = head.find(' '), sp2 = head.find(' ', sp1 + 1);
    if (sp1 == std::string::npos || sp2 == std::string::npos) { respond_err(fd, 400, "bad_request"); close(fd); return; }
    std::string method = head.substr(0, sp1), path = head.substr(sp1 + 1, sp2 - sp1 - 1);
    size_t qp = path.find('?');
    std::string query = qp == std::string::npos ? "" : path.substr(qp + 1);
    if (qp != std::string::npos) path.resize(qp);

    std::string range_hdr;
    { size_t rp = lower.find("\r\nrange:"); if (rp != std::string::npos) { size_t e2 = lower.find("\r\n", rp + 2); std::string v = lower.substr(rp + 8, e2 == std::string::npos ? std::string::npos : e2 - rp - 8); size_t b2 = v.find_first_not_of(" \t"); range_hdr = b2 == std::string::npos ? "" : v.substr(b2); while (!range_hdr.empty() && (range_hdr.back() == ' ' || range_hdr.back() == '\t')) range_hdr.pop_back(); } }
    bool is_head = method == "HEAD";
    if (is_head && path != "/api/download") { respond_err(fd, 405, "method"); close(fd); return; }
    if (method == "GET" || is_head) {
        std::string err;
        if (path == "/" || path == "/index.html")
            respond(fd, 200, "text/html; charset=utf-8", (const char*)ASSET_INDEX_HTML, ASSET_INDEX_HTML_LEN);
        else if (path == "/api/roots") respond_json(fd, 200, "{\"roots\":" + fs_roots_json() + ",\"version\":\"" APP_VERSION "\"}");
        else if (path == "/api/ls") {
            std::string n = path_param(query, err);
            if (n.empty()) { respond_err(fd, 400, err); }
            else {
                std::string js = fs_ls_json(n, err);
                if (js.empty()) respond_err(fd, err_status(err), err); else respond_json(fd, 200, js);
            }
        } else if (path == "/api/view") {
            std::string n = path_param(query, err);
            if (n.empty()) { respond_err(fd, 400, err); }
            else {
                std::string data;
                long long size = 0;
                long long off = atoll(qget(query, "off").c_str()), len = atoll(qget(query, "len").c_str());
                if (fs_read_range(n, off, len, data, size, err)) {
                    char extra[64];
                    snprintf(extra, sizeof(extra), "X-File-Size: %lld\r\n", size);
                    send_head(fd, 200, "application/octet-stream", (long long)data.size(), false, extra);
                    send_all(fd, data.data(), data.size());
                } else respond_err(fd, err_status(err), err);
            }
        } else if (path == "/api/download") {
            std::string n = path_param(query, err);
            if (n.empty()) respond_err(fd, 400, err); else do_download(fd, n, qget(query, "inline") == "1", range_hdr, is_head);
        } else if (path == "/api/find") {
            std::string n = path_param(query, err);
            if (n.empty()) respond_err(fd, 400, err);
            else {
                std::string js = fs_find_json(n, qget(query, "q"), 1000, 12, err);
                if (js.empty()) respond_err(fd, err_status(err), err); else respond_json(fd, 200, js);
            }
        } else if (path == "/api/job") respond_json(fd, 200, job_json());
        else if (path == "/api/pkg") respond_json(fd, 200, install_json());
        else if (path == "/api/zip") do_zip(fd, query);
        else if (path == "/api/smb") respond_json(fd, 200, "{\"conns\":" + smb_conns_json() + "}");
        else if (path == "/api/state") respond_json(fd, 200, state_read());
        else if (path == "/api/log") {
            size_t n = 8192;
            std::string t = log_tail(n);
            respond(fd, 200, "text/plain; charset=utf-8", t.data(), t.size());
        } else respond_err(fd, 404, "not_found");
        close(fd);
        return;
    }

    if (method != "POST") { respond_err(fd, 404, "not_found"); close(fd); return; }
    // Защита от запросов с чужих сайтов: такой заголовок браузер без разрешения не отправит.
    if (lower.find("x-requested-with: ps4cmd") == std::string::npos) { respond_err(fd, 403, "forbidden"); close(fd); return; }
    size_t cl = 0;
    bool have_cl = false;
    size_t cp = lower.find("content-length:");
    if (cp != std::string::npos) { cl = (size_t)strtoull(lower.c_str() + cp + 15, nullptr, 10); have_cl = true; }
    std::string body = req.substr(head_end + 4);

    if (path == "/api/upload") {
        if (!have_cl) respond_err(fd, 411, "length_required"); else do_upload(fd, query, cl, body);
        close(fd);
        return;
    }
    if (cl > kMaxBody) { respond_err(fd, 400, "too_large"); close(fd); return; }
    while (body.size() < cl) {
        ssize_t n = recv(fd, buf, std::min(sizeof(buf), cl - body.size()), 0);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) break;
        body.append(buf, (size_t)n);
    }
    handle_post(fd, path, body);
    close(fd);
}

static void client_thread(int fd) {
    handle(fd);
    --g_clients;
}

static int make_listener(int port, bool& in_use) {
    in_use = false;
    int s = socket(AF_INET, SOCK_STREAM, 0);
    if (s < 0) return -1;
    int yes = 1;
    setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));
    sockaddr_in a;
    memset(&a, 0, sizeof(a));
    a.sin_family = AF_INET;
    a.sin_port = htons((uint16_t)port);
    a.sin_addr.s_addr = htonl(INADDR_ANY);
    if (bind(s, (sockaddr*)&a, sizeof(a)) < 0) {
        in_use = (errno == EADDRINUSE);
        close(s);
        return -1;
    }
    if (listen(s, 8) < 0) { close(s); return -1; }
    return s;
}

static int g_port = 0;
int http_port() { return g_port; }

int http_run(int port) {
    g_port = port;
    mkdir(DATA_DIR, 0777);
    unlink(quit_flag_path().c_str());  // устаревший флаг от прошлого запуска
    bool in_use = false;
    int srv = make_listener(port, in_use);
    if (srv < 0 && in_use) {
        // Уже работает другая копия: просим её завершиться (файл-флаг) и ждём освобождения порта.
        app_log("порт %d занят: найдена старая копия, жду 3 с и прошу её завершиться", port);
        std::this_thread::sleep_for(std::chrono::seconds(3));
        FILE* f = fopen(quit_flag_path().c_str(), "w");
        if (f) { fputs("quit\n", f); fclose(f); }
        for (int i = 0; i < 180 && srv < 0; ++i) {  // до ~90 с
            std::this_thread::sleep_for(std::chrono::milliseconds(500));
            srv = make_listener(port, in_use);
        }
        unlink(quit_flag_path().c_str());
        if (srv >= 0) notify("ps4commander перезапущен (старая копия закрыта)");
    }
    if (srv < 0) {
        app_log("не удалось занять порт %d (errno=%d %s)", port, errno, strerror(errno));
        notify("ps4commander: не удалось занять порт %d", port);
        return -2;
    }
    app_log("веб-интерфейс слушает порт %d", port);
    {
        std::string ip = local_ip();
        app_log("адрес консоли: %s", ip.empty() ? "не определён" : ip.c_str());
        if (!ip.empty()) notify("ps4commander %s: откройте в браузере http://%s:%d", APP_VERSION, ip.c_str(), port);
        else notify("ps4commander %s: откройте в браузере http://<IP консоли>:%d (IP: Настройки → Сеть → Состояние подключения)", APP_VERSION, port);
    }

    for (;;) {
        pollfd p;
        p.fd = srv;
        p.events = POLLIN;
        p.revents = 0;
        int r = poll(&p, 1, 1000);
        if (g_quit) break;
        struct stat st;
        if (stat(quit_flag_path().c_str(), &st) == 0) {
            app_log("получена просьба завершиться от новой копии");
            unlink(quit_flag_path().c_str());
            break;
        }
        if (r <= 0) continue;
        int c = accept(srv, nullptr, nullptr);
        if (c < 0) {
            if (errno == EMFILE || errno == ENFILE || errno == ENOMEM)
                std::this_thread::sleep_for(std::chrono::milliseconds(200));
            continue;
        }
        timeval rt = {10, 0}, wt = {30, 0};
        setsockopt(c, SOL_SOCKET, SO_RCVTIMEO, &rt, sizeof(rt));
        setsockopt(c, SOL_SOCKET, SO_SNDTIMEO, &wt, sizeof(wt));
#ifdef SO_NOSIGPIPE
        { int one = 1; setsockopt(c, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof(one)); }
#endif
        if (g_clients.load() >= kMaxClients) { close(c); continue; }
        ++g_clients;
        try {
            std::thread(client_thread, c).detach();
        } catch (...) {
            --g_clients;
            close(c);
        }
    }
    close(srv);
    for (int i = 0; i < 30 && g_clients.load() > 0; ++i)
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    return 0;
}
