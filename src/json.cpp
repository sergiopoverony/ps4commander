#include "json.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace {
struct P {
    const std::string& s;
    size_t i = 0;
    explicit P(const std::string& str) : s(str) {}
    void ws() { while (i < s.size() && (s[i] == ' ' || s[i] == '\t' || s[i] == '\n' || s[i] == '\r')) ++i; }
    bool eat(char c) { ws(); if (i < s.size() && s[i] == c) { ++i; return true; } return false; }

    static void put_utf8(std::string& o, unsigned cp) {
        if (cp < 0x80) o += (char)cp;
        else if (cp < 0x800) { o += (char)(0xC0 | (cp >> 6)); o += (char)(0x80 | (cp & 0x3F)); }
        else if (cp < 0x10000) { o += (char)(0xE0 | (cp >> 12)); o += (char)(0x80 | ((cp >> 6) & 0x3F)); o += (char)(0x80 | (cp & 0x3F)); }
        else { o += (char)(0xF0 | (cp >> 18)); o += (char)(0x80 | ((cp >> 12) & 0x3F)); o += (char)(0x80 | ((cp >> 6) & 0x3F)); o += (char)(0x80 | (cp & 0x3F)); }
    }
    static int hex4(const std::string& s, size_t i) {
        if (i + 4 > s.size()) return -1;
        int v = 0;
        for (int k = 0; k < 4; ++k) {
            char c = s[i + k];
            v <<= 4;
            if (c >= '0' && c <= '9') v |= c - '0';
            else if (c >= 'a' && c <= 'f') v |= c - 'a' + 10;
            else if (c >= 'A' && c <= 'F') v |= c - 'A' + 10;
            else return -1;
        }
        return v;
    }
    bool str(std::string& out) {
        ws();
        if (i >= s.size() || s[i] != '"') return false;
        ++i;
        out.clear();
        while (i < s.size()) {
            unsigned char c = (unsigned char)s[i++];
            if (c == '"') return true;
            if (c != '\\') { out += (char)c; continue; }
            if (i >= s.size()) return false;
            char e = s[i++];
            switch (e) {
                case '"': out += '"'; break;
                case '\\': out += '\\'; break;
                case '/': out += '/'; break;
                case 'b': out += '\b'; break;
                case 'f': out += '\f'; break;
                case 'n': out += '\n'; break;
                case 'r': out += '\r'; break;
                case 't': out += '\t'; break;
                case 'u': {
                    int h = hex4(s, i);
                    if (h < 0) return false;
                    i += 4;
                    unsigned cp = (unsigned)h;
                    if (cp >= 0xD800 && cp < 0xDC00 && i + 6 <= s.size() && s[i] == '\\' && s[i + 1] == 'u') {
                        int lo = hex4(s, i + 2);
                        if (lo >= 0xDC00 && lo < 0xE000) { cp = 0x10000 + ((cp - 0xD800) << 10) + ((unsigned)lo - 0xDC00); i += 6; }
                    }
                    if (cp == 0) return false;  // NUL в путях недопустим
                    put_utf8(out, cp);
                    break;
                }
                default: return false;
            }
        }
        return false;
    }
};
}  // namespace

JsonObj json_parse(const std::string& body) {
    JsonObj o;
    P p(body);
    if (!p.eat('{')) return o;
    if (p.eat('}')) { o.ok = true; return o; }
    for (;;) {
        std::string key;
        if (!p.str(key) || !p.eat(':')) return o;
        p.ws();
        if (p.i >= body.size()) return o;
        char c = body[p.i];
        if (c == '"') {
            std::string v;
            if (!p.str(v)) return o;
            o.str[key] = v;
        } else if (c == '[') {
            ++p.i;
            std::vector<std::string> v;
            if (!p.eat(']')) {
                for (;;) {
                    std::string e;
                    if (!p.str(e)) return o;
                    v.push_back(e);
                    if (p.eat(',')) continue;
                    if (p.eat(']')) break;
                    return o;
                }
            }
            o.arr[key] = v;
        } else if (!strncmp(body.c_str() + p.i, "true", 4)) { o.flag[key] = true; p.i += 4; }
        else if (!strncmp(body.c_str() + p.i, "false", 5)) { o.flag[key] = false; p.i += 5; }
        else if (!strncmp(body.c_str() + p.i, "null", 4)) { p.i += 4; }
        else {
            char* end = nullptr;
            double d = strtod(body.c_str() + p.i, &end);
            if (end == body.c_str() + p.i) return o;
            o.num[key] = d;
            p.i = (size_t)(end - body.c_str());
        }
        if (p.eat(',')) continue;
        if (p.eat('}')) break;
        return o;
    }
    o.ok = true;
    return o;
}

std::string json_esc(const std::string& s) {
    std::string o;
    o.reserve(s.size() + 8);
    size_t i = 0, n = s.size();
    while (i < n) {
        unsigned char c = (unsigned char)s[i];
        if (c == '"') { o += "\\\""; ++i; }
        else if (c == '\\') { o += "\\\\"; ++i; }
        else if (c == '\n') { o += "\\n"; ++i; }
        else if (c == '\r') { o += "\\r"; ++i; }
        else if (c == '\t') { o += "\\t"; ++i; }
        else if (c < 0x20 || c == 0x7F) { char b[8]; snprintf(b, sizeof(b), "\\u%04x", c); o += b; ++i; }
        else if (c < 0x80) { o += (char)c; ++i; }
        else {
            int len = (c >= 0xF0 && c < 0xF8) ? 4 : (c >= 0xE0) ? 3 : (c >= 0xC2) ? 2 : 0;
            bool good = len > 0 && c < 0xF8 && i + (size_t)len <= n;
            for (int k = 1; good && k < len; ++k)
                if (((unsigned char)s[i + (size_t)k] & 0xC0) != 0x80) good = false;
            if (good) { o.append(s, i, (size_t)len); i += (size_t)len; }
            else { o += "\xEF\xBF\xBD"; ++i; }
        }
    }
    return o;
}
