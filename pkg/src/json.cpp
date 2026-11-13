#include "json.h"
#include <stdlib.h>

static const JsonValue g_empty;

const JsonValue& JsonValue::operator[](const char* key) const
{
    if (type != OBJ) return g_empty;
    for (size_t i = 0; i < obj.size(); i++)
        if (obj[i].first == key) return obj[i].second;
    return g_empty;
}

namespace {

struct Parser {
    const char* p;
    const char* end;
    bool ok = true;

    void skip_ws() { while (p < end && (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r')) p++; }

    bool expect(char c)
    {
        skip_ws();
        if (p >= end || *p != c) { ok = false; return false; }
        p++;
        return true;
    }

    // Записывает один символ в UTF-8 в конец строки out.
    void append_utf8(std::string& out, unsigned cp)
    {
        if (cp < 0x80) {
            out += (char)cp;
        } else if (cp < 0x800) {
            out += (char)(0xC0 | (cp >> 6));
            out += (char)(0x80 | (cp & 0x3F));
        } else {
            out += (char)(0xE0 | (cp >> 12));
            out += (char)(0x80 | ((cp >> 6) & 0x3F));
            out += (char)(0x80 | (cp & 0x3F));
        }
    }

    bool parse_string(std::string& out)
    {
        if (!expect('"')) return false;
        out.clear();
        while (p < end && *p != '"') {
            unsigned char c = (unsigned char)*p;
            if (c == '\\') {
                p++;
                if (p >= end) { ok = false; return false; }
                char e = *p++;
                switch (e) {
                    case '"':  out += '"';  break;
                    case '\\': out += '\\'; break;
                    case '/':  out += '/';  break;
                    case 'b':  out += '\b'; break;
                    case 'f':  out += '\f'; break;
                    case 'n':  out += '\n'; break;
                    case 'r':  out += '\r'; break;
                    case 't':  out += '\t'; break;
                    case 'u': {
                        if (end - p < 4) { ok = false; return false; }
                        unsigned cp = (unsigned)strtoul(std::string(p, 4).c_str(), NULL, 16);
                        p += 4;
                        append_utf8(out, cp);
                        break;
                    }
                    default: ok = false; return false;
                }
            } else {
                out += (char)c;
                p++;
            }
        }
        if (p >= end) { ok = false; return false; }
        p++;   // закрывающая кавычка
        return true;
    }

    bool parse_value(JsonValue& v)
    {
        skip_ws();
        if (p >= end) { ok = false; return false; }

        if (*p == '"') {
            v.type = JsonValue::STR;
            return parse_string(v.str);
        }
        if (*p == '{') return parse_object(v);
        if (*p == '[') return parse_array(v);
        if (p + 4 <= end && std::string(p, 4) == "true") { v.type = JsonValue::BOOL; v.boolean = true; p += 4; return true; }
        if (p + 5 <= end && std::string(p, 5) == "false") { v.type = JsonValue::BOOL; v.boolean = false; p += 5; return true; }
        if (p + 4 <= end && std::string(p, 4) == "null") { v.type = JsonValue::NONE; p += 4; return true; }

        // число
        const char* start = p;
        if (p < end && (*p == '-' || *p == '+')) p++;
        bool any = false;
        while (p < end && ((*p >= '0' && *p <= '9') || *p == '.' || *p == 'e' || *p == 'E' || *p == '-' || *p == '+')) { p++; any = true; }
        if (!any) { ok = false; return false; }
        v.type = JsonValue::NUM;
        v.num = strtod(std::string(start, p).c_str(), NULL);
        return true;
    }

    bool parse_object(JsonValue& v)
    {
        if (!expect('{')) return false;
        v.type = JsonValue::OBJ;
        skip_ws();
        if (p < end && *p == '}') { p++; return true; }
        for (;;) {
            std::string key;
            skip_ws();
            if (!parse_string(key)) return false;
            if (!expect(':')) return false;
            JsonValue val;
            if (!parse_value(val)) return false;
            v.obj.push_back(std::make_pair(key, val));
            skip_ws();
            if (p < end && *p == ',') { p++; continue; }
            break;
        }
        return expect('}');
    }

    bool parse_array(JsonValue& v)
    {
        if (!expect('[')) return false;
        v.type = JsonValue::ARR;
        skip_ws();
        if (p < end && *p == ']') { p++; return true; }
        for (;;) {
            JsonValue val;
            if (!parse_value(val)) return false;
            v.arr.push_back(val);
            skip_ws();
            if (p < end && *p == ',') { p++; continue; }
            break;
        }
        return expect(']');
    }
};

}   // namespace

bool json_parse(const std::string& text, JsonValue& out)
{
    Parser ps;
    ps.p = text.data();
    ps.end = text.data() + text.size();
    if (!ps.parse_value(out)) return false;
    return ps.ok;
}
