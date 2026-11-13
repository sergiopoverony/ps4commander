#include "service.h"
#include "httpc.h"
#include "json.h"
#include "net.h"
#include "log.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/stat.h>

const char* SERVICE_HOST = "127.0.0.1";
const int   SERVICE_PORT = 8484;
static const int LOADER_PORT = 9090;
static const char* PAYLOAD_PATH = "/app0/assets/misc/ps4commander.elf";

// Два разных пути для одного и того же файла (см. torr-handoff.md, раздел 4):
//  - AUTORUN_ELF_PATH - под этим путём приложение реально может писать файл (fopen/mkdir).
//  - AUTORUN_INI_ENTRY - именно этот путь (с "/user") нужно прописывать в payloads.ini,
//    так GoldHEN реально находит payload на автозапуске.
static const char* AUTORUN_ELF_PATH  = "/data/payloads/ps4commander.elf";
static const char* AUTORUN_INI_ENTRY = "/user/data/payloads/ps4commander.elf";
static const char* AUTORUN_INI_PATH  = "/data/GoldHEN/payloads.ini";

static std::string urlenc(const std::string& s)
{
    static const char* hex = "0123456789ABCDEF";
    std::string o;
    for (unsigned char c : s) {
        if (isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~' || c == '/') o += (char)c;
        else { o += '%'; o += hex[c >> 4]; o += hex[c & 15]; }
    }
    return o;
}

// Маленький helper: экранирование строки для вставки в JSON-тело запроса (копия json_esc службы,
// здесь не линкуется библиотека службы, поэтому своя минимальная версия).
static std::string json_esc_pub(const std::string& s)
{
    std::string o;
    for (unsigned char c : s) {
        if (c == '"' || c == '\\') { o += '\\'; o += (char)c; }
        else if (c == '\n') o += "\\n";
        else if (c < 0x20) { char b[8]; snprintf(b, sizeof(b), "\\u%04x", c); o += b; }
        else o += (char)c;
    }
    return o;
}

static volatile bool g_unsafe = false;
void service_set_unsafe(bool on) { g_unsafe = on; }
static const char* unsafe_field() { return g_unsafe ? ",\"unsafe\":true" : ""; }

static std::string json_path(const std::string& p) { return "{\"path\":\"" + json_esc_pub(p) + "\"" + unsafe_field() + "}"; }

bool service_ls(const std::string& path, LsView& out)
{
    out = LsView();
    HttpResult r = http_get(SERVICE_HOST, SERVICE_PORT, "/api/ls?path=" + urlenc(path), 4000);
    if (!r.connected) { out.err = "noconn"; return false; }
    if (r.status != 200) { JsonValue j; json_parse(r.body, j); out.err = j["error"].asStr("http" ); return false; }
    JsonValue j;
    if (!json_parse(r.body, j) || j.type != JsonValue::OBJ) { out.err = "bad_json"; return false; }
    out.ok = true;
    out.path = j["path"].asStr();
    out.parent = j["parent"].asStr();
    out.freeB = (int64_t)j["free"].asNum(-1);
    out.totalB = (int64_t)j["total"].asNum(-1);
    out.danger = j["danger"].asBool();
    out.trunc = j["trunc"].asBool();
    const JsonValue& arr = j["entries"];
    for (size_t i = 0; i < arr.arr.size(); i++) {
        const JsonValue& e = arr.arr[i];
        Entry en;
        en.name = e["n"].asStr();
        std::string t = e["t"].asStr("f");
        en.type = t.empty() ? 'f' : t[0];
        en.size = (int64_t)e["s"].asNum();
        en.mtime = (int64_t)e["m"].asNum();
        en.perm = e["p"].asStr("644");
        en.link = e["l"].asStr();
        en.td = e["td"].asBool();
        en.user = e["u"].asStr();
        en.group = e["g"].asStr();
        out.entries.push_back(en);
    }
    return true;
}

bool service_roots(std::vector<RootView>& out, std::string& version)
{
    out.clear();
    HttpResult r = http_get(SERVICE_HOST, SERVICE_PORT, "/api/roots", 3000);
    if (!r.connected || r.status != 200) return false;
    JsonValue j;
    if (!json_parse(r.body, j) || j.type != JsonValue::OBJ) return false;
    version = j["version"].asStr();
    const JsonValue& arr = j["roots"];
    for (size_t i = 0; i < arr.arr.size(); i++) {
        const JsonValue& e = arr.arr[i];
        RootView v;
        v.path = e["p"].asStr();
        v.mnt = e["mnt"].asBool();
        v.smb = e["smb"].asBool();
        out.push_back(v);
    }
    return true;
}

static std::string paths_json(const std::vector<std::string>& ps)
{
    std::string o = "[";
    for (size_t i = 0; i < ps.size(); i++) o += (i ? ",\"" : "\"") + json_esc_pub(ps[i]) + "\"";
    return o + "]";
}

static bool post_check(const std::string& path, const std::string& body, std::string& err)
{
    HttpResult r = http_post(SERVICE_HOST, SERVICE_PORT, path, body, 5000);
    if (!r.connected) { err = "noconn"; return false; }
    if (r.status == 200) return true;
    JsonValue j; json_parse(r.body, j);
    err = j["error"].asStr("http");
    return false;
}

bool service_job_start(const std::string& op, const std::vector<std::string>& paths,
                        const std::string& dest, bool overwrite, bool recursive,
                        const std::string& mode, std::string& err)
{
    std::string b = "{\"op\":\"" + op + "\",\"paths\":" + paths_json(paths);
    if (!dest.empty()) b += ",\"dest\":\"" + json_esc_pub(dest) + "\"";
    if (overwrite) b += ",\"overwrite\":true";
    if (recursive) b += ",\"recursive\":true";
    if (!mode.empty()) b += ",\"mode\":\"" + mode + "\"";
    b += unsafe_field();
    b += "}";
    return post_check("/api/job/start", b, err);
}

bool service_job_state(JobView& out)
{
    out = JobView();
    HttpResult r = http_get(SERVICE_HOST, SERVICE_PORT, "/api/job", 3000);
    if (!r.connected || r.status != 200) return false;
    JsonValue j;
    if (!json_parse(r.body, j) || j.type != JsonValue::OBJ) return false;
    out.ok = true;
    out.id = j["id"].asInt();
    out.op = j["op"].asStr();
    out.state = j["state"].asStr();
    out.cur = j["cur"].asStr();
    out.done = j["done"].asInt();
    out.total = j["total"].asInt();
    out.filesDone = j["files_done"].asInt();
    out.filesTotal = j["files_total"].asInt();
    out.secs = j["secs"].asInt();
    out.errTotal = j["err_total"].asInt();
    return true;
}

bool service_job_cancel() { std::string e; return post_check("/api/job/cancel", "{}", e); }

bool service_mkdir(const std::string& path, std::string& err) { return post_check("/api/mkdir", json_path(path), err); }

bool service_rename(const std::string& path, const std::string& to, std::string& err)
{
    return post_check("/api/rename", "{\"path\":\"" + json_esc_pub(path) + "\",\"to\":\"" + json_esc_pub(to) + "\"" + unsafe_field() + "}", err);
}

bool service_chmod(const std::string& path, const std::string& mode, bool recursive, std::string& err)
{
    std::vector<std::string> ps; ps.push_back(path);
    return service_job_start("chmod", ps, "", false, recursive, mode, err);
}

bool service_delete(const std::vector<std::string>& paths, bool recursive, std::string& err)
{
    return service_job_start("delete", paths, "", false, recursive, "", err);
}

bool service_pkg_install(const std::vector<std::string>& paths, std::string& err)
{
    return post_check("/api/pkg/install", "{\"paths\":" + paths_json(paths) + "}", err);
}

bool service_pkg_state(PkgQueueView& out)
{
    out = PkgQueueView();
    HttpResult r = http_get(SERVICE_HOST, SERVICE_PORT, "/api/pkg", 3000);
    if (!r.connected || r.status != 200) return false;
    JsonValue j;
    if (!json_parse(r.body, j) || j.type != JsonValue::OBJ) return false;
    out.active = j["active"].asBool();
    const JsonValue& arr = j["items"];
    for (size_t i = 0; i < arr.arr.size(); i++) {
        const JsonValue& e = arr.arr[i];
        PkgItemView v;
        v.name = e["n"].asStr();
        v.path = e["p"].asStr();
        v.status = e["st"].asStr();
        v.msg = e["msg"].asStr();
        v.total = e["total"].asInt();
        v.done = e["done"].asInt();
        out.items.push_back(v);
    }
    return true;
}

bool service_pkg_cancel() { std::string e; return post_check("/api/pkg/cancel", "{}", e); }

bool service_state_get(std::string& l, std::string& r_, std::string& theme)
{
    HttpResult r = http_get(SERVICE_HOST, SERVICE_PORT, "/api/state", 2000);
    if (!r.connected || r.status != 200) return false;
    JsonValue j;
    if (!json_parse(r.body, j) || j.type != JsonValue::OBJ) return false;
    l = j["l"].asStr();
    r_ = j["r"].asStr();
    theme = j["theme"].asStr("classic");
    return true;
}

bool service_state_set_theme(const std::string& theme)
{
    std::string e;
    return post_check("/api/state", "{\"theme\":\"" + theme + "\"}", e);
}

bool service_state_set_paths(const std::string& l, const std::string& r)
{
    std::string e;
    return post_check("/api/state", "{\"l\":\"" + json_esc_pub(l) + "\",\"r\":\"" + json_esc_pub(r) + "\"}", e);
}

bool service_view(const std::string& path, int64_t maxLen, std::string& data, std::string& err)
{
    char q[64]; snprintf(q, sizeof(q), "&off=0&len=%lld", (long long)maxLen);
    HttpResult r = http_get(SERVICE_HOST, SERVICE_PORT, "/api/view?path=" + urlenc(path) + q, 5000);
    if (!r.connected) { err = "noconn"; return false; }
    if (r.status != 200) { err = "http"; return false; }
    data = r.body;
    return true;
}

bool service_send_payload(std::string& err, bool* connectFailed)
{
    if (connectFailed) *connectFailed = false;

    FILE* f = fopen(PAYLOAD_PATH, "rb");
    if (!f) { err = "no payload file in PKG"; logf_("service: cannot open %s", PAYLOAD_PATH); return false; }
    std::string data;
    char buf[16384];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), f)) > 0) data.append(buf, n);
    fclose(f);
    if (data.size() < 1024) { err = "payload file is empty"; return false; }

    int s = net_connect(SERVICE_HOST, LOADER_PORT, 3000);
    if (s < 0) {
        err = "BinLoader (port 9090) is not available. Is GoldHEN running?";
        logf_("service: BinLoader connect failed");
        if (connectFailed) *connectFailed = true;
        return false;
    }

    size_t sent = 0;
    while (sent < data.size()) {
        int w = send(s, data.data() + sent, data.size() - sent, 0);
        if (w <= 0) { err = "sending to BinLoader failed"; logf_("service: send failed after %d of %d bytes", (int)sent, (int)data.size()); close(s); return false; }
        sent += (size_t)w;
    }
    close(s);
    logf_("service: payload sent (%d bytes)", (int)sent);
    return true;
}

bool service_install_autorun(std::string& err)
{
    FILE* f = fopen(PAYLOAD_PATH, "rb");
    if (!f) { err = "no payload file in PKG"; logf_("autorun: cannot open %s", PAYLOAD_PATH); return false; }
    std::string data;
    char buf[16384];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), f)) > 0) data.append(buf, n);
    fclose(f);
    if (data.size() < 1024) { err = "payload file is empty"; return false; }

    mkdir("/data/payloads", 0777);
    FILE* out = fopen(AUTORUN_ELF_PATH, "wb");
    if (!out) { err = std::string("cannot write ") + AUTORUN_ELF_PATH; logf_("autorun: %s", err.c_str()); return false; }
    size_t wn = fwrite(data.data(), 1, data.size(), out);
    fclose(out);
    if (wn != data.size()) { err = "autorun payload write incomplete"; logf_("autorun: %s", err.c_str()); return false; }

    std::string ini;
    FILE* fi = fopen(AUTORUN_INI_PATH, "rb");
    if (fi) {
        char ibuf[4096];
        size_t rn;
        while ((rn = fread(ibuf, 1, sizeof(ibuf), fi)) > 0) ini.append(ibuf, rn);
        fclose(fi);
    }

    bool changed = false;
    if (ini.find(AUTORUN_INI_ENTRY) == std::string::npos) {
        std::string entry = std::string(AUTORUN_INI_ENTRY) + " = 1\n";
        size_t sec = ini.find("[AutoRun]");
        if (sec == std::string::npos) {
            if (!ini.empty() && ini.back() != '\n') ini += "\n";
            if (!ini.empty()) ini += "\n";
            ini += "[AutoRun]\n";
            ini += entry;
        } else {
            size_t lineEnd = ini.find('\n', sec);
            size_t insertAt = (lineEnd == std::string::npos) ? ini.size() : lineEnd + 1;
            ini.insert(insertAt, entry);
        }
        changed = true;
    }

    if (!changed) { logf_("autorun: payload copied, payloads.ini already correct"); return true; }

    FILE* fw = fopen(AUTORUN_INI_PATH, "wb");
    if (!fw) { err = std::string("cannot write ") + AUTORUN_INI_PATH; logf_("autorun: %s", err.c_str()); return false; }
    fwrite(ini.data(), 1, ini.size(), fw);
    fclose(fw);
    logf_("autorun: payload installed and registered in payloads.ini (%s)", AUTORUN_INI_ENTRY);
    return true;
}

bool service_quit()
{
    HttpResult r = http_post(SERVICE_HOST, SERVICE_PORT, "/quit", "{}", 2000);
    logf_("service: quit -> connected=%d status=%d", r.connected ? 1 : 0, r.status);
    return r.connected && r.status == 200;
}

bool service_find(const std::string& path, const std::string& q, FindResult& out, std::string& err)
{
    out = FindResult();
    HttpResult r = http_get(SERVICE_HOST, SERVICE_PORT, "/api/find?path=" + urlenc(path) + "&q=" + urlenc(q), 60000);
    if (!r.connected) { err = "noconn"; return false; }
    JsonValue j;
    if (!json_parse(r.body, j) || j.type != JsonValue::OBJ) { err = "bad_json"; return false; }
    if (r.status != 200) { err = j["error"].asStr("http"); return false; }
    out.scanned = j["scanned"].asInt();
    out.trunc = j["trunc"].asBool();
    const JsonValue& arr = j["results"];
    for (size_t i = 0; i < arr.arr.size(); i++) {
        FindHit h;
        h.path = arr.arr[i]["p"].asStr();
        std::string t = arr.arr[i]["t"].asStr("f");
        h.type = t.empty() ? 'f' : t[0];
        h.size = (int64_t)arr.arr[i]["s"].asNum();
        out.hits.push_back(h);
    }
    return true;
}

bool service_log(int maxBytes, std::string& out)
{
    HttpResult r = http_get(SERVICE_HOST, SERVICE_PORT, "/api/log?n=" + std::to_string(maxBytes), 4000);
    if (!r.connected || r.status != 200) return false;
    out = r.body;
    return true;
}

static std::string smb_body(const SmbConnView& c, const std::string& pass, bool withShare)
{
    std::string b = "{\"alias\":\"" + json_esc_pub(c.alias) + "\",\"host\":\"" + json_esc_pub(c.host) + "\",\"user\":\"" + json_esc_pub(c.user) +
                    "\",\"pass\":\"" + json_esc_pub(pass) + "\",\"domain\":\"" + json_esc_pub(c.domain) + "\"";
    if (withShare) b += ",\"share\":\"" + json_esc_pub(c.share) + "\"";
    return b + "}";
}

bool service_smb_list(std::vector<SmbConnView>& out, std::string& err)
{
    out.clear();
    HttpResult r = http_get(SERVICE_HOST, SERVICE_PORT, "/api/smb", 4000);
    if (!r.connected) { err = "noconn"; return false; }
    JsonValue j;
    if (r.status != 200 || !json_parse(r.body, j) || j.type != JsonValue::OBJ) { err = "http"; return false; }
    const JsonValue& arr = j["conns"];
    for (size_t i = 0; i < arr.arr.size(); i++) {
        const JsonValue& e = arr.arr[i];
        SmbConnView c;
        c.alias = e["alias"].asStr(); c.host = e["host"].asStr(); c.share = e["share"].asStr();
        c.user = e["user"].asStr(); c.domain = e["domain"].asStr(); c.hasPass = e["has_pass"].asBool();
        out.push_back(c);
    }
    return true;
}

bool service_smb_scan_start(std::string& err)
{
    HttpResult r = http_post(SERVICE_HOST, SERVICE_PORT, "/api/smb/scan", "{}", 5000);
    if (!r.connected) { err = "noconn"; return false; }
    if (r.status == 200) return true;
    JsonValue j; json_parse(r.body, j);
    err = j["error"].asStr("http");
    return false;
}

bool service_smb_scan_state(SmbScanView& out)
{
    out = SmbScanView();
    HttpResult r = http_get(SERVICE_HOST, SERVICE_PORT, "/api/smb/scan", 3000);
    if (!r.connected || r.status != 200) return false;
    JsonValue j;
    if (!json_parse(r.body, j) || j.type != JsonValue::OBJ) return false;
    out.running = j["running"].asBool();
    out.done = j["done"].asInt();
    out.total = j["total"].asInt();
    const JsonValue& arr = j["found"];
    for (size_t i = 0; i < arr.arr.size(); i++) out.found.push_back(arr.arr[i].asStr());
    return true;
}

bool service_smb_shares(const SmbConnView& c, const std::string& pass, std::vector<std::string>& shares, std::string& err)
{
    shares.clear();
    HttpResult r = http_post(SERVICE_HOST, SERVICE_PORT, "/api/smb/shares", smb_body(c, pass, false), 20000);
    if (!r.connected) { err = "noconn"; return false; }
    JsonValue j; json_parse(r.body, j);
    if (r.status != 200) { err = j["error"].asStr("http"); return false; }
    const JsonValue& arr = j["shares"];
    for (size_t i = 0; i < arr.arr.size(); i++) shares.push_back(arr.arr[i].asStr());
    return true;
}

bool service_smb_save(const SmbConnView& c, const std::string& pass, std::string& err)
{
    HttpResult r = http_post(SERVICE_HOST, SERVICE_PORT, "/api/smb/save", smb_body(c, pass, true), 20000);
    if (!r.connected) { err = "noconn"; return false; }
    if (r.status == 200) return true;
    JsonValue j; json_parse(r.body, j);
    err = j["error"].asStr("http");
    return false;
}

bool service_smb_del(const std::string& alias, std::string& err)
{
    return post_check("/api/smb/del", "{\"alias\":\"" + json_esc_pub(alias) + "\"}", err);
}
