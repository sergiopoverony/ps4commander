#include "ui.h"
#include "service.h"
#include "scene.h"
#include "image.h"
#include "i18n.h"
#include "log.h"

#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#include <string.h>
#include <unistd.h>
#include <time.h>
#include <algorithm>
#include <string>
#include <vector>

#include <orbis/Pad.h>
#include <orbis/UserService.h>
#include <orbis/ImeDialog.h>
#include <orbis/CommonDialog.h>
#include <orbis/Sysmodule.h>

extern "C" uint64_t sceKernelGetProcessTime(void);
extern "C" int sceSystemServiceHideSplashScreen(void);

#ifndef APP_VERSION
#define APP_VERSION "0.0.0.0"
#endif

#define FRAME_WIDTH   1920
#define FRAME_HEIGHT  1080
#define FRAME_DEPTH      4

static const int FONT_PX  = 22;
static const int ROW_H    = 30;
static const int MARGIN_X = 36;
static const int PANEL_TOP = 110;
static const int PANEL_H   = 820;      // до строки нижнего меню
static const int BAR_TOP   = PANEL_TOP + PANEL_H + 14;
static const int BAR_H     = 44;

static const Color C_BG     = { 18,  20,  26 };
static const Color C_PANEL  = { 26,  29,  37 };
static const Color C_ROW2   = { 31,  34,  43 };
static const Color C_EDGE   = { 70,  78, 100 };
static const Color C_EDGE_A = { 120, 160, 235 };   // рамка активной панели
static const Color C_TEXT   = { 230, 230, 230 };
static const Color C_DIM    = { 150, 155, 165 };
static const Color C_DIMMER = { 95,  99, 108 };
static const Color C_DIR    = { 140, 190, 255 };
static const Color C_SEL    = { 255, 210, 110 };
static const Color C_ACCENT = { 110, 170, 255 };
static const Color C_RED    = { 235, 100, 100 };
static const Color C_CUR_BG = { 48,  58,  86 };
static const Color C_BARBTN = { 36,  40,  52 };
static const Color C_BARBTN_F = { 54,  70, 110 };

static Scene2D* g_scene = NULL;
static FT_Face  g_font;
static int      g_cellW = 12;
static bool     g_ready = false;
static int      g_redraw = 2;
static int      g_frameID = 0;

static Image g_qrSbp, g_qrPaypal;
static bool  g_qrSbpOk = false, g_qrPaypalOk = false, g_qrTried = false;

// ---------------------------------------------------------------- UTF-8 / UTF-16

static int decode_utf8(const unsigned char*& p)
{
    unsigned c = *p++;
    if (c < 0x80) return (int)c;
    int extra; unsigned cp;
    if ((c & 0xE0) == 0xC0)      { cp = c & 0x1F; extra = 1; }
    else if ((c & 0xF0) == 0xE0) { cp = c & 0x0F; extra = 2; }
    else if ((c & 0xF8) == 0xF0) { cp = c & 0x07; extra = 3; }
    else return '?';
    while (extra-- > 0) {
        if ((*p & 0xC0) != 0x80) return '?';
        cp = (cp << 6) | (*p & 0x3F);
        p++;
    }
    return (int)cp;
}

static void encode_utf8(int cp, std::string& out)
{
    if (cp < 0x80) { out += (char)cp; }
    else if (cp < 0x800) { out += (char)(0xC0 | (cp >> 6)); out += (char)(0x80 | (cp & 0x3F)); }
    else if (cp < 0x10000) {
        out += (char)(0xE0 | (cp >> 12)); out += (char)(0x80 | ((cp >> 6) & 0x3F)); out += (char)(0x80 | (cp & 0x3F));
    } else {
        out += (char)(0xF0 | (cp >> 18)); out += (char)(0x80 | ((cp >> 12) & 0x3F));
        out += (char)(0x80 | ((cp >> 6) & 0x3F)); out += (char)(0x80 | (cp & 0x3F));
    }
}

static int count_cp(const std::string& s)
{
    const unsigned char* p = (const unsigned char*)s.c_str();
    int n = 0;
    while (*p) { decode_utf8(p); n++; }
    return n;
}

static std::string fit(const std::string& s, int maxChars)
{
    if (maxChars <= 0) return "";
    if (count_cp(s) <= maxChars) return s;
    const unsigned char* start = (const unsigned char*)s.c_str();
    const unsigned char* p = start;
    for (int i = 0; i < maxChars - 1 && *p; i++) decode_utf8(p);
    return std::string((const char*)start, (size_t)(p - start)) + "\xE2\x80\xA6";
}

static size_t utf8_to_utf16(const std::string& s, uint16_t* out, size_t outCap)
{
    size_t n = 0;
    const unsigned char* p = (const unsigned char*)s.data();
    const unsigned char* end = p + s.size();
    while (p < end && n + 1 < outCap) {
        int cp = decode_utf8(p);
        if (cp < 0) break;
        if (cp <= 0xFFFF) out[n++] = (uint16_t)cp;
        else if (n + 2 < outCap) { cp -= 0x10000; out[n++] = (uint16_t)(0xD800 + (cp >> 10)); out[n++] = (uint16_t)(0xDC00 + (cp & 0x3FF)); }
        else break;
    }
    out[n] = 0;
    return n;
}

static std::string utf16_to_utf8(const uint16_t* in, size_t cap)
{
    std::string out;
    for (size_t i = 0; i < cap && in[i]; i++) {
        unsigned c = in[i];
        if (c >= 0xD800 && c <= 0xDBFF && i + 1 < cap && in[i + 1] >= 0xDC00 && in[i + 1] <= 0xDFFF) {
            int cp = 0x10000 + ((c - 0xD800) << 10) + (in[++i] - 0xDC00);
            encode_utf8(cp, out);
        } else {
            encode_utf8((int)c, out);
        }
    }
    return out;
}

// ---------------------------------------------------------------- текст

static void draw_text(const std::string& s, int x, int baseline, Color fg, Color bg)
{
    FT_GlyphSlot slot = g_font->glyph;
    const unsigned char* p = (const unsigned char*)s.c_str();
    while (*p) {
        int cp = decode_utf8(p);
        FT_UInt gi = FT_Get_Char_Index(g_font, (FT_ULong)cp);
        if (FT_Load_Glyph(g_font, gi, FT_LOAD_DEFAULT)) continue;
        if (FT_Render_Glyph(slot, ft_render_mode_normal)) continue;
        for (unsigned yy = 0; yy < slot->bitmap.rows; yy++) {
            for (unsigned xx = 0; xx < slot->bitmap.width; xx++) {
                unsigned a = slot->bitmap.buffer[yy * slot->bitmap.pitch + xx];
                if (a == 0) continue;
                int px = x + (int)xx + slot->bitmap_left;
                int py = baseline + (int)yy - slot->bitmap_top;
                if (px < 0 || py < 0 || px >= FRAME_WIDTH || py >= FRAME_HEIGHT) continue;
                Color c;
                c.r = (uint8_t)((bg.r * (255 - a) + fg.r * a) / 255);
                c.g = (uint8_t)((bg.g * (255 - a) + fg.g * a) / 255);
                c.b = (uint8_t)((bg.b * (255 - a) + fg.b * a) / 255);
                g_scene->PutPixel(px, py, c);
            }
        }
        x += (int)(slot->advance.x >> 6);
    }
}

static void put_center(int cx, const std::string& s, int baseline, Color fg, Color bg)
{
    draw_text(s, cx - (count_cp(s) * g_cellW) / 2, baseline, fg, bg);
}

static void draw_wrapped(const std::string& s, int x, int yTop, int maxc, int maxLines, int lineH, Color fg, Color bg)
{
    const unsigned char* base = (const unsigned char*)s.c_str();
    size_t total = s.size(), pos = 0;
    int line = 0;
    while (pos < total && line < maxLines) {
        const unsigned char* p = base + pos;
        int n = 0;
        while (*p && n < maxc) { decode_utf8(p); n++; }
        size_t byteEnd = (size_t)(p - base);
        if (byteEnd < total) {
            size_t sp = s.rfind(' ', byteEnd);
            if (sp != std::string::npos && sp > pos) byteEnd = sp;
        } else byteEnd = total;
        draw_text(s.substr(pos, byteEnd - pos), x, yTop + line * lineH, fg, bg);
        pos = byteEnd; while (pos < total && s[pos] == ' ') pos++;
        line++;
    }
}

// ---------------------------------------------------------------- иконки кнопок геймпада

enum Icon { ICON_CROSS, ICON_CIRCLE, ICON_SQUARE, ICON_TRIANGLE, ICON_OPTIONS, ICON_ARROWS, ICON_UPDOWN };
static const int ICON_S = 20;

static bool in_tri(float px, float py, float ax, float ay, float bx, float by, float cx, float cy)
{
    float d1 = (px - bx) * (ay - by) - (ax - bx) * (py - by);
    float d2 = (px - cx) * (by - cy) - (bx - cx) * (py - cy);
    float d3 = (px - ax) * (cy - ay) - (cx - ax) * (py - ay);
    bool neg = (d1 < 0) || (d2 < 0) || (d3 < 0);
    bool pos = (d1 > 0) || (d2 > 0) || (d3 > 0);
    return !(neg && pos);
}

static bool icon_hit(Icon k, float u, float v)
{
    switch (k) {
    case ICON_CIRCLE: { float r = sqrtf(u * u + v * v); return r < 0.88f && r > 0.60f; }
    case ICON_CROSS: {
        float a = fabsf(u - v) * 0.7071f, b = fabsf(u + v) * 0.7071f;
        return fabsf(u) < 0.82f && fabsf(v) < 0.82f && (a < 0.17f || b < 0.17f);
    }
    case ICON_SQUARE: { float m = fmaxf(fabsf(u), fabsf(v)); return m < 0.80f && m > 0.56f; }
    case ICON_TRIANGLE:
        return in_tri(u, v, 0.0f, -0.90f, -0.90f, 0.70f, 0.90f, 0.70f)
            && !in_tri(u, v, 0.0f, -0.42f, -0.495f, 0.46f, 0.495f, 0.46f);
    case ICON_OPTIONS:
        return fabsf(u) < 0.80f && (fabsf(v + 0.55f) < 0.11f || fabsf(v) < 0.11f || fabsf(v - 0.55f) < 0.11f);
    case ICON_UPDOWN:
        return in_tri(u, v, 0.0f, -0.95f, -0.60f, -0.15f, 0.60f, -0.15f)
            || in_tri(u, v, -0.60f, 0.15f, 0.60f, 0.15f, 0.0f, 0.95f);
    case ICON_ARROWS:
        return in_tri(u, v, 0.0f, -0.95f, -0.32f, -0.30f, 0.32f, -0.30f)
            || in_tri(u, v, 0.0f, 0.95f, -0.32f, 0.30f, 0.32f, 0.30f)
            || in_tri(u, v, -0.95f, 0.0f, -0.30f, -0.32f, -0.30f, 0.32f)
            || in_tri(u, v, 0.95f, 0.0f, 0.30f, -0.32f, 0.30f, 0.32f);
    }
    return false;
}

static Color icon_color(Icon k)
{
    switch (k) {
    case ICON_CROSS:    { Color c = { 120, 170, 255 }; return c; }
    case ICON_CIRCLE:   { Color c = { 255, 100, 100 }; return c; }
    case ICON_SQUARE:   { Color c = { 240, 130, 200 }; return c; }
    case ICON_TRIANGLE: { Color c = {  90, 210, 170 }; return c; }
    default:            { Color c = { 200, 204, 212 }; return c; }
    }
}

static void draw_icon(Icon k, int x, int top, Color bg)
{
    Color fg = icon_color(k);
    for (int py = 0; py < ICON_S; py++) {
        for (int px = 0; px < ICON_S; px++) {
            int hit = 0;
            for (int sy = 0; sy < 4; sy++)
                for (int sx = 0; sx < 4; sx++) {
                    float u = ((px + (sx + 0.5f) / 4.0f) / ICON_S) * 2.0f - 1.0f;
                    float v = ((py + (sy + 0.5f) / 4.0f) / ICON_S) * 2.0f - 1.0f;
                    if (icon_hit(k, u, v)) hit++;
                }
            if (hit == 0) continue;
            int xx = x + px, yy = top + py;
            if (xx < 0 || yy < 0 || xx >= FRAME_WIDTH || yy >= FRAME_HEIGHT) continue;
            Color c;
            c.r = (uint8_t)((bg.r * (16 - hit) + fg.r * hit) / 16);
            c.g = (uint8_t)((bg.g * (16 - hit) + fg.g * hit) / 16);
            c.b = (uint8_t)((bg.b * (16 - hit) + fg.b * hit) / 16);
            g_scene->PutPixel(xx, yy, c);
        }
    }
}

// Бейдж-иконка "L1"/"R1"/"L2"/"R2" - скруглённый прямоугольник с подписью (своих треугольников
// под эти кнопки не делаем, буквы понятнее).
static void fill_rounded(int x, int y, int w, int h, int r, Color c);
static void draw_badge(const char* label, int x, int top, Color bg)
{
    Color c = { 200, 204, 212 };
    fill_rounded(x, top, ICON_S + 6, ICON_S, 6, c);
    draw_text(label, x + 3, top + ICON_S - 5, bg, c);
}

struct RichTok { bool isIcon; bool isBadge; Icon icon; std::string text; };

static bool icon_by_name(const std::string& n, Icon& out)
{
    if (n == "cross") { out = ICON_CROSS; return true; }
    if (n == "circle") { out = ICON_CIRCLE; return true; }
    if (n == "square") { out = ICON_SQUARE; return true; }
    if (n == "triangle") { out = ICON_TRIANGLE; return true; }
    if (n == "options") { out = ICON_OPTIONS; return true; }
    if (n == "updown") { out = ICON_UPDOWN; return true; }
    if (n == "arrows") { out = ICON_ARROWS; return true; }
    return false;
}

static std::vector<RichTok> rich_parse(const std::string& s)
{
    std::vector<RichTok> out;
    std::string cur;
    size_t i = 0;
    while (i < s.size()) {
        if (s[i] == '{') {
            size_t e = s.find('}', i);
            if (e != std::string::npos) {
                std::string name = s.substr(i + 1, e - i - 1);
                Icon k;
                bool isBadge = (name == "l1" || name == "r1" || name == "l2" || name == "r2");
                if (isBadge || icon_by_name(name, k)) {
                    if (!cur.empty()) { RichTok t; t.isIcon = false; t.isBadge = false; t.icon = ICON_CROSS; t.text = cur; out.push_back(t); cur.clear(); }
                    RichTok t; t.isIcon = !isBadge; t.isBadge = isBadge; t.icon = isBadge ? ICON_CROSS : k; t.text = isBadge ? name : ""; out.push_back(t);
                    i = e + 1;
                    continue;
                }
            }
        }
        cur += s[i++];
    }
    if (!cur.empty()) { RichTok t; t.isIcon = false; t.isBadge = false; t.icon = ICON_CROSS; t.text = cur; out.push_back(t); }
    return out;
}

static int rich_width(const std::vector<RichTok>& toks)
{
    int w = 0;
    for (auto& t : toks) w += (t.isIcon || t.isBadge) ? (ICON_S + 6) : count_cp(t.text) * g_cellW;
    return w;
}

static void draw_rich(const std::vector<RichTok>& toks, int x, int baseline, Color fg, Color bg)
{
    for (auto& t : toks) {
        if (t.isBadge) { draw_badge(t.text == "l1" ? "L1" : t.text == "r1" ? "R1" : t.text == "l2" ? "L2" : "R2", x, baseline - ICON_S + 3, bg); x += ICON_S + 6; }
        else if (t.isIcon) { draw_icon(t.icon, x, baseline - ICON_S + 3, bg); x += ICON_S + 6; }
        else { draw_text(t.text, x, baseline, fg, bg); x += count_cp(t.text) * g_cellW; }
    }
}

static void put_center_rich(int cx, const std::string& s, int baseline, Color fg, Color bg)
{
    std::vector<RichTok> toks = rich_parse(s);
    draw_rich(toks, cx - rich_width(toks) / 2, baseline, fg, bg);
}

// ---------------------------------------------------------------- примитивы

static void fill(int x, int y, int w, int h, Color c) { g_scene->FillRect(x, y, w, h, c); }

static void fill_rounded(int x, int y, int w, int h, int r, Color c)
{
    if (r <= 0) { fill(x, y, w, h, c); return; }
    fill(x + r, y, w - 2 * r, h, c);
    fill(x, y + r, w, h - 2 * r, c);
    for (int yy = 0; yy < r; yy++) {
        double dx2 = (double)r * r - (double)(r - yy) * (r - yy);
        int dx = (int)sqrt(dx2 < 0 ? 0 : dx2);
        int rowW = r + dx;
        fill(x + r - dx, y + yy, rowW - (r - dx), 1, c);
        fill(x + w - r - dx, y + yy, rowW - (r - dx), 1, c);
        fill(x + r - dx, y + h - 1 - yy, rowW - (r - dx), 1, c);
        fill(x + w - r - dx, y + h - 1 - yy, rowW - (r - dx), 1, c);
    }
}

static void draw_frame(int x, int y, int w, int h, int r, Color border, Color fillc)
{
    fill_rounded(x - 2, y - 2, w + 4, h + 4, r + 2, border);
    fill_rounded(x, y, w, h, r, fillc);
}

// ---------------------------------------------------------------- модель панели

struct Entry2 { std::string name, perm, link; char type = 'f'; int64_t size = 0, mtime = 0; };

struct Panel {
    std::string path;
    std::vector<Entry2> items;    // первый элемент - ".." (если путь не корень), затем сами записи
    int cur = 0, scroll = 0;
    std::vector<std::string> sel;    // отмеченные (Square) имена
    bool hasUp = false;
    std::string err;
};

static Panel g_panels[2];
static int g_active = 0;

static std::string parent_of(const std::string& p)
{
    size_t i = p.find_last_of('/');
    return (i == std::string::npos || i == 0) ? "/" : p.substr(0, i);
}
static std::string base_of(const std::string& p)
{
    size_t i = p.find_last_of('/');
    return i == std::string::npos ? p : p.substr(i + 1);
}
static std::string join_path(const std::string& dir, const std::string& name)
{
    if (dir == "/") return "/" + name;
    return dir + "/" + name;
}

static void panel_load(Panel& p, const std::string& path, const std::string& keepName)
{
    LsView v;
    bool ok = service_ls(path, v);
    p.sel.clear();
    if (!ok) { p.err = v.err.empty() ? "error" : v.err; return; }
    p.err.clear();
    p.path = v.path;
    p.items.clear();
    p.hasUp = p.path != "/";
    if (p.hasUp) { Entry2 up; up.name = ".."; up.type = 'd'; p.items.push_back(up); }
    for (auto& e : v.entries) {
        Entry2 x;
        x.name = e.name; x.type = e.type; x.size = e.size; x.mtime = e.mtime; x.perm = e.perm; x.link = e.link;
        p.items.push_back(x);
    }
    std::sort(p.items.begin() + (p.hasUp ? 1 : 0), p.items.end(), [](const Entry2& a, const Entry2& b) {
        if ((a.type == 'd') != (b.type == 'd')) return a.type == 'd';
        return a.name < b.name;
    });
    p.cur = 0;
    if (!keepName.empty()) for (size_t i = 0; i < p.items.size(); i++) if (p.items[i].name == keepName) { p.cur = (int)i; break; }
    p.scroll = 0;
}

static Entry2* panel_cur(Panel& p) { return (p.cur >= 0 && p.cur < (int)p.items.size()) ? &p.items[p.cur] : NULL; }

static void panel_refresh(Panel& p) { std::string keep = panel_cur(p) ? panel_cur(p)->name : ""; panel_load(p, p.path, keep); }

static void panel_up(Panel& p) { if (p.hasUp) { std::string from = base_of(p.path); panel_load(p, parent_of(p.path), from); } }

static void panel_enter(Panel& p)
{
    Entry2* e = panel_cur(p);
    if (!e) return;
    if (e->name == "..") { panel_up(p); return; }
    if (e->type == 'd' || (e->type == 'l')) panel_load(p, join_path(p.path, e->name), "");
}

// ---------------------------------------------------------------- состояние приложения

enum Svc { S_CHECK, S_STARTING, S_READY, S_FAILED };
static Svc g_svc = S_CHECK;
static uint64_t g_nextCheck = 0;
static int g_startTries = 0;
static std::string g_svcErr;

enum Focus { FOCUS_PANEL, FOCUS_BAR, FOCUS_APPMENU, FOCUS_CTX, FOCUS_CONFIRM, FOCUS_MSG, FOCUS_PROGRESS, FOCUS_DONATE };
static Focus g_focus = FOCUS_PANEL;
static int g_barCursor = 0;
static int g_appMenuCursor = 0;
static int g_ctxCursor = 0;
static std::string g_msg;
static bool g_msgIsError = false;

enum ConfirmKind { CONF_NONE, CONF_DELETE, CONF_OVERWRITE, CONF_PKG_INSTALL, CONF_QUIT_SVC };
static ConfirmKind g_confirm = CONF_NONE;
static std::string g_confirmOp;              // "copy"/"move" при CONF_OVERWRITE
static std::vector<std::string> g_confirmPaths;
static std::string g_confirmDest;

enum ProgKind { PROG_NONE, PROG_JOB, PROG_PKG };
static ProgKind g_prog = PROG_NONE;

static const char* KEY_NAMES[10] = {
    "help", "refresh", "view", "edit", "copy", "move", "mkdir", "del", "chmod", "menu"
};
static const char* KEY_DEFAULT[10] = {
    "Help", "Refresh", "View", "Edit", "Copy", "Move", "MkDir", "Delete", "Chmod", "Menu"
};

static std::string tr_err(const std::string& code) { return tr(("err_" + code).c_str(), code.c_str()); }

// Подстановка "{n}" в переведённую строку - тот же простой шаблонный формат,
// что использует веб-интерфейс (t('key', {n}) в web/index.html).
static std::string tr_fmt(const char* key, const char* def, const std::string& n) {
    std::string s = tr(key, def);
    size_t pos = s.find("{n}");
    if (pos != std::string::npos) s.replace(pos, 3, n);
    return s;
}

// Тот же формат, что fmtDate() в web/index.html: "YYYY-MM-DD HH:MM".
static std::string fmt_date(int64_t sec)
{
    if (sec <= 0) return "";
    time_t t = (time_t)sec;
    struct tm tmv;
    if (!gmtime_r(&t, &tmv)) return "";
    char b[32];
    snprintf(b, sizeof(b), "%04d-%02d-%02d %02d:%02d", tmv.tm_year + 1900, tmv.tm_mon + 1, tmv.tm_mday, tmv.tm_hour, tmv.tm_min);
    return b;
}

static std::string fmt_size(int64_t s)
{
    char b[32];
    if (s < 0) return "";
    const char* u[] = { "B", "KB", "MB", "GB", "TB" };
    double v = (double)s; int ui = 0;
    while (v >= 1024.0 && ui < 4) { v /= 1024.0; ui++; }
    snprintf(b, sizeof(b), ui == 0 ? "%.0f %s" : "%.1f %s", v, u[ui]);
    return b;
}

static bool is_pkg(const Entry2& e) { std::string n = e.name; size_t d = n.rfind('.'); return d != std::string::npos && n.substr(d) == ".pkg"; }

static bool g_imeReady = false;
static int g_userId = 0;

static bool ime_text(const std::string& title, const std::string& initial, std::string& out);

// ---------------------------------------------------------------- действия нижней панели (F1..F10)

static void act_view();
static void act_edit();
static void act_copy_move(bool move);
static void act_mkdir();
static void act_delete();
static void act_chmod();

static void run_bar_action(int idx)
{
    switch (idx) {
    case 0: g_msg = tr("hint_cmd", "{arrows} move    {cross} enter/select    {circle} up    {square} mark    {triangle} menu    {l1}/{r1} switch panel    {l2}/{r2} bottom bar"); g_focus = FOCUS_MSG; g_msgIsError = false; break;
    case 1: panel_refresh(g_panels[0]); panel_refresh(g_panels[1]); break;
    case 2: act_view(); break;
    case 3: act_edit(); break;
    case 4: act_copy_move(false); break;
    case 5: act_copy_move(true); break;
    case 6: act_mkdir(); break;
    case 7: act_delete(); break;
    case 8: act_chmod(); break;
    case 9: g_focus = FOCUS_APPMENU; g_appMenuCursor = 0; break;
    }
}

static void act_view()
{
    Panel& p = g_panels[g_active];
    Entry2* e = panel_cur(p);
    if (!e || e->name == ".." || e->type != 'f') return;
    std::string data, err;
    if (!service_view(join_path(p.path, e->name), 60000, data, err)) { g_msg = tr_err(err); g_msgIsError = true; g_focus = FOCUS_MSG; return; }
    // Полноценный просмотрщик с прокруткой не делаем на этом этапе - показываем начало файла
    // в окне сообщения (достаточно, чтобы опознать текстовый файл; полный просмотр - в веб-интерфейсе).
    g_msg = data.substr(0, 2000);
    g_msgIsError = false;
    g_focus = FOCUS_MSG;
}

static void act_edit()
{
    // Редактирование текста с клавиатуры PS4 попросит слишком длинный ввод для системного IME -
    // в этой версии не делаем (полноценный редактор остаётся в веб-интерфейсе).
    g_msg = tr("editWeb", "Full editing is available on the web interface (port 8484).");
    g_msgIsError = false;
    g_focus = FOCUS_MSG;
}

static void act_copy_move(bool move)
{
    Panel& src = g_panels[g_active];
    Panel& dst = g_panels[1 - g_active];
    std::vector<std::string> paths = src.sel;
    if (paths.empty()) { Entry2* e = panel_cur(src); if (e && e->name != "..") paths.push_back(join_path(src.path, e->name)); }
    if (paths.empty()) return;
    g_confirmOp = move ? "move" : "copy";
    g_confirmPaths = paths;
    g_confirmDest = dst.path;
    g_confirm = CONF_OVERWRITE;
    g_focus = FOCUS_CONFIRM;
}

static void act_mkdir()
{
    std::string name;
    if (!ime_text(tr("newFolder", "New folder name:"), "", name) || name.empty()) return;
    std::string err;
    if (service_mkdir(join_path(g_panels[g_active].path, name), err)) panel_refresh(g_panels[g_active]);
    else { g_msg = tr_err(err); g_msgIsError = true; g_focus = FOCUS_MSG; }
}

static void act_delete()
{
    Panel& p = g_panels[g_active];
    std::vector<std::string> names = p.sel;
    if (names.empty()) { Entry2* e = panel_cur(p); if (e && e->name != "..") names.push_back(e->name); }
    if (names.empty()) return;
    std::vector<std::string> paths;
    for (auto& n : names) paths.push_back(join_path(p.path, n));
    g_confirmPaths = paths;
    g_confirm = CONF_DELETE;
    g_focus = FOCUS_CONFIRM;
}

static void act_chmod()
{
    Entry2* e = panel_cur(g_panels[g_active]);
    if (!e || e->name == "..") return;
    std::string mode;
    if (!ime_text(tr("modeOct", "Permissions (octal, e.g. 755):"), e->perm, mode) || mode.empty()) return;
    std::string err;
    if (service_chmod(join_path(g_panels[g_active].path, e->name), mode, false, err)) panel_refresh(g_panels[g_active]);
    else { g_msg = tr_err(err); g_msgIsError = true; g_focus = FOCUS_MSG; }
}

// ---------------------------------------------------------------- контекстное меню (Треугольник)

// Порядок фиксирован: Rename/Mark/Properties/Cancel всегда на местах 0..3,
// "Install .pkg" добавляется пятым пунктом (индекс 4) только для .pkg-файлов
// (см. ctx_count) - чтобы индексы не менялись местами в зависимости от pkg.
// Ключи те же, что у веб-версии: rename/select/infoTitle/cancel/install.
static const char* CTX_ITEMS[5] = { "rename", "select", "infoTitle", "cancel", "install" };

static int ctx_count(bool pkg) { return pkg ? 5 : 4; }

static void ctx_run(int idx, bool pkg)
{
    Panel& p = g_panels[g_active];
    Entry2* e = panel_cur(p);
    if (!e || e->name == "..") return;
    if (idx == 0) {
        std::string name;
        if (ime_text(tr("rename", "Rename"), e->name, name) && !name.empty() && name != e->name) {
            std::string err;
            if (service_rename(join_path(p.path, e->name), join_path(p.path, name), err)) panel_refresh(p);
            else { g_msg = tr_err(err); g_msgIsError = true; g_focus = FOCUS_MSG; return; }
        }
    } else if (idx == 1) {
        auto it = std::find(p.sel.begin(), p.sel.end(), e->name);
        if (it == p.sel.end()) p.sel.push_back(e->name); else p.sel.erase(it);
    } else if (idx == 2) {
        // Те же поля и ключи, что в диалоге "Properties" веб-версии (showInfo в web/index.html).
        const char* typeKey = e->type == 'd' ? "typeDir" : e->type == 'l' ? "typeLink" : e->type == 'f' ? "typeFile" : "typeOther";
        std::string msg = tr("fPath", "Path") + ": " + join_path(p.path, e->name) +
                           "\n" + tr("fType", "Type") + ": " + tr(typeKey, "File") +
                           "\n" + tr("fMode", "Permissions") + ": " + e->perm;
        if (e->type == 'f') msg += "\n" + tr("fSize", "Size") + ": " + fmt_size(e->size) + " (" + std::to_string((long long)e->size) + " B)";
        std::string dt = fmt_date(e->mtime);
        if (!dt.empty()) msg += "\n" + tr("fTime", "Modified") + ": " + dt;
        if (!e->link.empty()) msg += "\n" + tr("fTarget", "Link target") + ": " + e->link;
        g_msg = msg; g_msgIsError = false;
    } else if (idx == 4 && pkg) {
        g_confirmPaths.clear(); g_confirmPaths.push_back(join_path(p.path, e->name));
        g_confirm = CONF_PKG_INSTALL;
        g_focus = FOCUS_CONFIRM;
        return;
    }
    g_focus = (g_focus == FOCUS_CONFIRM) ? g_focus : FOCUS_PANEL;
    if (idx == 2) g_focus = FOCUS_MSG;
}

// ---------------------------------------------------------------- меню приложения (Options / F10)

static std::string theme_name(const std::string& th)
{
    if (th == "dark") return tr("themeDark", "Dark");
    if (th == "light") return tr("themeLight", "Light");
    return tr("themeClassic", "Classic");
}

static std::string g_theme = "classic";

static const int APP_MENU_N = 4;

static void appmenu_run(int idx)
{
    if (idx == 0) {
        g_theme = g_theme == "classic" ? "dark" : g_theme == "dark" ? "light" : "classic";
        service_state_set_theme(g_theme);
    } else if (idx == 1) {
        i18n_set(i18n_current() == "ru" ? "en" : "ru");
    } else if (idx == 2) {
        if (!g_qrTried) { g_qrTried = true; g_qrSbpOk = g_qrSbp.load("/app0/assets/images/qr_sbp.png"); g_qrPaypalOk = g_qrPaypal.load("/app0/assets/images/qr_paypal.png"); }
        g_focus = FOCUS_DONATE;
    } else if (idx == 3) {
        exit(0);
    }
}

// ---------------------------------------------------------------- отрисовка

static void draw_panel_box(int idx, int x, int w)
{
    Panel& p = g_panels[idx];
    bool act = idx == g_active;
    draw_frame(x, PANEL_TOP, w, PANEL_H, 10, act ? C_EDGE_A : C_EDGE, C_PANEL);

    std::string title = p.err.empty() ? p.path : (p.path + "  [" + p.err + "]");
    draw_text(fit(title, (w - 20) / g_cellW), x + 10, PANEL_TOP + 26, act ? C_ACCENT : C_DIM, C_PANEL);
    fill(x + 6, PANEL_TOP + 36, w - 12, 1, C_EDGE);

    int rows = (PANEL_H - 50) / ROW_H;
    if (p.cur < p.scroll) p.scroll = p.cur;
    if (p.cur >= p.scroll + rows) p.scroll = p.cur - rows + 1;
    int nameW = (w - 20 - 220) / g_cellW;
    if (nameW < 6) nameW = 6;

    for (int row = 0; row < rows; row++) {
        int i = p.scroll + row;
        if (i >= (int)p.items.size()) break;
        Entry2& e = p.items[i];
        int ry = PANEL_TOP + 46 + row * ROW_H;
        bool isCur = i == p.cur;
        bool isSel = std::find(p.sel.begin(), p.sel.end(), e.name) != p.sel.end();
        Color rowBg = isCur && act ? C_CUR_BG : (row % 2 ? C_ROW2 : C_PANEL);
        fill(x + 4, ry, w - 8, ROW_H, rowBg);
        Color fg = e.name == ".." ? C_DIM : (isSel ? C_SEL : (e.type == 'd' ? C_DIR : C_TEXT));
        std::string nm = e.name == ".." ? ".." : (e.type == 'd' ? (e.name + "/") : e.name);
        draw_text((isSel ? "*" : " ") + fit(nm, nameW), x + 10, ry + ROW_H - 9, fg, rowBg);
        if (e.name != ".." && e.type != 'd') draw_text(fmt_size(e.size), x + w - 210, ry + ROW_H - 9, C_DIM, rowBg);
        else if (e.type == 'l') draw_text("->", x + w - 210, ry + ROW_H - 9, C_DIM, rowBg);
    }
}

static void draw_bar()
{
    int n = 10;
    int gap = 6;
    int bw = (FRAME_WIDTH - 2 * MARGIN_X - gap * (n - 1)) / n;
    for (int i = 0; i < n; i++) {
        int bx = MARGIN_X + i * (bw + gap);
        bool hl = g_focus == FOCUS_BAR && i == g_barCursor;
        fill_rounded(bx, BAR_TOP, bw, BAR_H, 8, hl ? C_BARBTN_F : C_BARBTN);
        char num[4]; snprintf(num, sizeof(num), "%d", i + 1);
        std::string label = tr(KEY_NAMES[i], KEY_DEFAULT[i]);
        std::string s = std::string(num) + " " + fit(label, (bw - 20) / g_cellW);
        draw_text(s, bx + 10, BAR_TOP + BAR_H / 2 + 7, hl ? C_TEXT : C_DIM, hl ? C_BARBTN_F : C_BARBTN);
    }
}

static void draw_hint_and_credit()
{
    std::string hint;
    if (g_focus == FOCUS_PANEL) hint = "{arrows} " + tr("hint_nav", "move") + "   {cross} " + tr("hint_enter", "open") +
        "   {circle} " + tr("hint_up", "up") + "   {square} " + tr("hint_mark", "mark") + "   {triangle} " + tr("hint_ctx", "menu") +
        "   {l1}/{r1} " + tr("hint_swap", "switch panel") + "   {l2}/{r2} " + tr("hint_bar", "bottom bar");
    else if (g_focus == FOCUS_BAR) hint = "{l2}/{r2} " + tr("hint_pick", "choose") + "   {cross} " + tr("hint_do", "do") + "   {circle} " + tr("hint_back", "back");
    else hint = "{circle} " + tr("hint_back", "back");
    put_center_rich(FRAME_WIDTH / 2, hint, BAR_TOP + BAR_H + 30, C_DIM, C_BG);

    std::string credit = "PS4 Commander v" APP_VERSION "  Created by SergioPoverony and Mr.Claude";
    draw_text(credit, (FRAME_WIDTH - count_cp(credit) * g_cellW) / 2, BAR_TOP + BAR_H + 58, C_DIMMER, C_BG);
}

static void draw_overlay_panel(int w, int h, const char* title_key, const char* title_def)
{
    int x = (FRAME_WIDTH - w) / 2, y = (FRAME_HEIGHT - h) / 2;
    draw_frame(x, y, w, h, 16, C_EDGE_A, C_PANEL);
    if (title_key) put_center(FRAME_WIDTH / 2, tr(title_key, title_def), y + 44, C_TEXT, C_PANEL);
}

static void draw_appmenu()
{
    int w = 560, h = 60 + APP_MENU_N * 56 + 30;
    draw_overlay_panel(w, h, "menu", "Menu");
    int x = (FRAME_WIDTH - w) / 2, y = (FRAME_HEIGHT - h) / 2;
    // theme/lang/support - те же ключи, что в меню веб-версии (showMenu в web/index.html).
    // "appExit" - новый ключ: в вебе аналога нет (там просто закрывают страницу), а здесь
    // нужно явно закрыть приложение, не останавливая службу.
    const char* labels[APP_MENU_N] = { "theme", "lang", "support", "appExit" };
    const char* defs[APP_MENU_N] = { "Theme", "Language", "Support the project", "Exit" };
    std::string vals[APP_MENU_N] = { theme_name(g_theme), i18n_current() == "ru" ? "Русский" : "English", "", "" };
    for (int i = 0; i < APP_MENU_N; i++) {
        int ry = y + 70 + i * 56;
        bool hl = i == g_appMenuCursor;
        if (hl) fill_rounded(x + 20, ry - 30, w - 40, 44, 10, C_CUR_BG);
        std::string s = tr(labels[i], defs[i]);
        if (!vals[i].empty()) s += ":  " + vals[i];
        draw_text(s, x + 36, ry, hl ? C_TEXT : C_DIM, hl ? C_CUR_BG : C_PANEL);
    }
    put_center_rich(FRAME_WIDTH / 2, "{arrows} " + tr("hint_nav", "move") + "   {cross} " + tr("hint_do", "do") + "   {circle} " + tr("hint_back", "back"), y + h - 20, C_DIM, C_PANEL);
}

static void draw_ctxmenu()
{
    Entry2* e = panel_cur(g_panels[g_active]);
    bool pkg = e && is_pkg(*e);
    int n = ctx_count(pkg);
    int w = 480, h = 60 + n * 52 + 30;
    draw_overlay_panel(w, h, NULL, NULL);
    int x = (FRAME_WIDTH - w) / 2, y = (FRAME_HEIGHT - h) / 2;
    put_center(FRAME_WIDTH / 2, e ? fit(e->name, (w - 40) / g_cellW) : "", y + 40, C_ACCENT, C_PANEL);
    const char* defs[5] = { "Rename", "Select", "Properties", "Cancel", "Install PKG" };
    for (int i = 0; i < n; i++) {
        int ry = y + 72 + i * 52;
        bool hl = i == g_ctxCursor;
        if (hl) fill_rounded(x + 20, ry - 28, w - 40, 42, 10, C_CUR_BG);
        draw_text(tr(CTX_ITEMS[i], defs[i]), x + 36, ry, hl ? C_TEXT : C_DIM, hl ? C_CUR_BG : C_PANEL);
    }
}

static void draw_confirm()
{
    int w = 760, h = 280;
    draw_overlay_panel(w, h, NULL, NULL);
    int x = (FRAME_WIDTH - w) / 2, y = (FRAME_HEIGHT - h) / 2;
    int maxc = (w - 80) / (g_cellW > 0 ? g_cellW : 1);
    // Тексты и ключи переводов здесь - те же, что у веб-версии (del/items/delWarn/
    // move/copy/overwrite/install/installQ), чтобы приложение выглядело максимально
    // похоже на веб, а не по образцу других проектов.
    std::string title, body, yesLbl;
    if (g_confirm == CONF_DELETE) {
        yesLbl = tr("del", "Delete");
        title = yesLbl + " " + std::to_string(g_confirmPaths.size()) + " " + tr("items", "items") + "?";
        body = tr("delWarn", "This cannot be undone.");
    } else if (g_confirm == CONF_OVERWRITE) {
        yesLbl = g_confirmOp == "move" ? tr("move", "Move") : tr("copy", "Copy");
        title = yesLbl + " " + std::to_string(g_confirmPaths.size()) + " " + tr("items", "items") + "?";
        body = tr("overwrite", "Overwrite existing files");
    } else if (g_confirm == CONF_PKG_INSTALL) {
        yesLbl = tr("install", "Install PKG");
        std::string nm = g_confirmPaths.empty() ? "" : base_of(g_confirmPaths[0]);
        title = tr_fmt("installQ", "Install {n} via the system installer?", "\"" + nm + "\"");
    }
    put_center(FRAME_WIDTH / 2, title, y + 70, C_TEXT, C_PANEL);
    draw_wrapped(body, x + 40, y + 110, maxc, 3, 32, C_DIM, C_PANEL);
    put_center_rich(FRAME_WIDTH / 2, "{cross} " + yesLbl + "     {circle} " + tr("cancel", "Cancel"), y + h - 40, C_TEXT, C_PANEL);
}

static void draw_msg()
{
    int w = 900, h = 420;
    draw_overlay_panel(w, h, NULL, NULL);
    int x = (FRAME_WIDTH - w) / 2, y = (FRAME_HEIGHT - h) / 2;
    int maxc = (w - 80) / (g_cellW > 0 ? g_cellW : 1);
    draw_wrapped(g_msg, x + 40, y + 50, maxc, 11, 28, g_msgIsError ? C_RED : C_TEXT, C_PANEL);
    put_center_rich(FRAME_WIDTH / 2, "{circle} " + tr("close", "Close"), y + h - 30, C_DIM, C_PANEL);
}

static void draw_progress()
{
    int w = 760, h = 220;
    draw_overlay_panel(w, h, NULL, NULL);
    int x = (FRAME_WIDTH - w) / 2, y = (FRAME_HEIGHT - h) / 2;
    std::string line1, line2;
    double frac = 0;
    if (g_prog == PROG_JOB) {
        JobView jv; service_job_state(jv);
        line1 = jv.op + ": " + jv.cur;
        line2 = fmt_size(jv.done) + " / " + fmt_size(jv.total);
        if (jv.total > 0) frac = (double)jv.done / (double)jv.total;
        if (jv.state != "running" && jv.state != "scan") { g_prog = PROG_NONE; g_focus = FOCUS_PANEL; panel_refresh(g_panels[0]); panel_refresh(g_panels[1]); return; }
    } else if (g_prog == PROG_PKG) {
        PkgQueueView qv; service_pkg_state(qv);
        if (!qv.items.empty()) { auto& it = qv.items.back(); line1 = it.name + "  " + it.status; line2 = it.msg; if (it.total > 0) frac = (double)it.done / (double)it.total; }
        if (!qv.active) { g_prog = PROG_NONE; g_focus = FOCUS_PANEL; return; }
    }
    put_center(FRAME_WIDTH / 2, fit(line1, (w - 80) / g_cellW), y + 60, C_TEXT, C_PANEL);
    draw_text(fit(line2, (w - 80) / g_cellW), x + 40, y + 100, C_DIM, C_PANEL);
    int bx = x + 40, by = y + 130, bw = w - 80, bh = 20;
    fill_rounded(bx, by, bw, bh, 8, C_ROW2);
    if (frac > 0) fill_rounded(bx, by, (int)(bw * std::min(1.0, frac)), bh, 8, C_ACCENT);
    put_center_rich(FRAME_WIDTH / 2, "{cross} " + tr("hint_cancel", "cancel"), y + h - 30, C_DIM, C_PANEL);
}

static void draw_donate()
{
    int w = 700, h = 560;
    draw_overlay_panel(w, h, "support", "Support the project");
    int x = (FRAME_WIDTH - w) / 2, y = (FRAME_HEIGHT - h) / 2;
    bool ru = i18n_current() == "ru";
    Image& img = ru ? g_qrSbp : g_qrPaypal;
    bool ok = ru ? g_qrSbpOk : g_qrPaypalOk;
    if (ok && img.pixels) {
        int iw = 420, ih = 420 * img.height / img.width;
        int ix = x + (w - iw) / 2, iy = y + 70;
        for (int yy = 0; yy < ih; yy++) {
            int sy = yy * img.height / ih;
            for (int xx = 0; xx < iw; xx++) {
                int sx = xx * img.width / iw;
                unsigned char* px = img.pixels + (size_t)(sy * img.width + sx) * 4;
                Color c = { px[0], px[1], px[2] };
                g_scene->PutPixel(ix + xx, iy + yy, c);
            }
        }
        put_center(FRAME_WIDTH / 2, tr("supportCap", "PayPal"), y + h - 70, C_DIM, C_PANEL);
    } else {
        put_center(FRAME_WIDTH / 2, tr("noQr", "QR code not found in the app"), y + h / 2, C_DIM, C_PANEL);
    }
    put_center_rich(FRAME_WIDTH / 2, "{circle} " + tr("close", "Close"), y + h - 30, C_DIM, C_PANEL);
}

static void draw_connecting()
{
    std::string s = g_svc == S_CHECK ? tr("svc_checking", "Looking for the ps4commander service...")
                   : g_svc == S_STARTING ? tr("svc_starting", "Starting the service via BinLoader...")
                   : tr("svc_failed", "Service not responding. " ) + g_svcErr;
    put_center(FRAME_WIDTH / 2, s, FRAME_HEIGHT / 2, g_svc == S_FAILED ? C_RED : C_DIM, C_BG);
    if (g_svc == S_FAILED) put_center_rich(FRAME_WIDTH / 2, "{cross} " + tr("hint_retry", "try again"), FRAME_HEIGHT / 2 + 50, C_DIM, C_BG);
}

static void draw_all()
{
    fill(0, 0, FRAME_WIDTH, FRAME_HEIGHT, C_BG);
    draw_text("PS4 Commander", MARGIN_X, 56, C_ACCENT, C_BG);

    if (g_svc != S_READY) { draw_connecting(); return; }

    int halfW = (FRAME_WIDTH - 2 * MARGIN_X - 16) / 2;
    draw_panel_box(0, MARGIN_X, halfW);
    draw_panel_box(1, MARGIN_X + halfW + 16, halfW);
    draw_bar();
    draw_hint_and_credit();

    if (g_focus == FOCUS_APPMENU) draw_appmenu();
    else if (g_focus == FOCUS_CTX) draw_ctxmenu();
    else if (g_focus == FOCUS_CONFIRM) draw_confirm();
    else if (g_focus == FOCUS_MSG) draw_msg();
    else if (g_focus == FOCUS_PROGRESS) draw_progress();
    else if (g_focus == FOCUS_DONATE) draw_donate();
}

// ---------------------------------------------------------------- геймпад и системная клавиатура

static int g_pad = -1;

static bool pad_init()
{
    if (scePadInit() != 0) return false;
    OrbisUserServiceInitializeParams param;
    param.priority = ORBIS_KERNEL_PRIO_FIFO_LOWEST;
    sceUserServiceInitialize(&param);
    int userID = 0;
    sceUserServiceGetInitialUser(&userID);
    g_userId = userID;
    g_pad = scePadOpen(userID, 0, 0, NULL);
    return g_pad >= 0;
}

static unsigned pad_read()
{
    if (g_pad < 0) return 0;
    OrbisPadData data;
    memset(&data, 0, sizeof(data));
    scePadReadState(g_pad, &data);
    return (unsigned)data.buttons;
}

static bool ime_text(const std::string& title, const std::string& initial, std::string& out)
{
    if (!g_imeReady) { logf_("ui: ime dialog not ready, skip keyboard"); return false; }

    uint16_t buf[256];
    uint16_t wtitle[64];
    memset(buf, 0, sizeof(buf));
    memset(wtitle, 0, sizeof(wtitle));
    utf8_to_utf16(initial, buf, 256);
    utf8_to_utf16(title, wtitle, 64);

    OrbisImeDialogSetting setting;
    memset(&setting, 0, sizeof(setting));
    setting.userId = (uint32_t)g_userId;
    setting.type = ORBIS_TYPE_DEFAULT;
    setting.supportedLanguages = 0;
    setting.enterLabel = ORBIS_BUTTON_LABEL_DEFAULT;
    setting.inputMethod = ORBIS__DEFAULT;
    setting.filter = NULL;
    setting.option = 0;
    setting.maxTextLength = 255;
    setting.inputTextBuffer = (wchar_t*)buf;
    setting.posx = (float)(FRAME_WIDTH / 2);
    setting.posy = (float)(FRAME_HEIGHT / 2);
    setting.horizontalAlignment = ORBIS_H_CENTER;
    setting.verticalAlignment = ORBIS_V_CENTER;
    setting.placeholder = NULL;
    setting.title = (const wchar_t*)wtitle;

    if (sceImeDialogInit(&setting, NULL) != 0) { logf_("ui: sceImeDialogInit failed"); return false; }

    OrbisDialogStatus st;
    while ((st = sceImeDialogGetStatus()) == ORBIS_DIALOG_STATUS_RUNNING) {
        if (g_scene) { g_scene->SubmitFlip(g_frameID); g_scene->FrameWait(g_frameID); g_scene->FrameBufferSwap(); g_frameID++; }
        usleep(16000);
    }

    bool ok = false;
    if (st == ORBIS_DIALOG_STATUS_STOPPED) {
        OrbisDialogResult res;
        memset(&res, 0, sizeof(res));
        if (sceImeDialogGetResult(&res) == 0 && res.endstatus == ORBIS_DIALOG_OK) { out = utf16_to_utf8(buf, 256); ok = true; }
    }
    sceImeDialogTerm();
    g_redraw = 2;
    return ok;
}

// ---------------------------------------------------------------- инициализация и цикл

bool ui_init()
{
    g_scene = new Scene2D(FRAME_WIDTH, FRAME_HEIGHT, FRAME_DEPTH);
    if (!g_scene->Init(0xC000000, 2)) { logf_("ui: scene init FAILED"); return false; }
    if (!g_scene->InitFont(&g_font, "/app0/assets/fonts/DejaVuSansMono.ttf", FONT_PX)) { logf_("ui: font init FAILED"); return false; }

    FT_UInt gi = FT_Get_Char_Index(g_font, 'M');
    if (!FT_Load_Glyph(g_font, gi, FT_LOAD_DEFAULT)) {
        int w = (int)(g_font->glyph->advance.x >> 6);
        if (w > 0) g_cellW = w;
    }

    bool padOk = pad_init();

    bool imeMod = sceSysmoduleLoadModule(ORBIS_SYSMODULE_IME_DIALOG) == 0;
    bool cdMod = sceSysmoduleLoadModuleInternal(ORBIS_SYSMODULE_INTERNAL_COMMON_DIALOG) == 0;
    if (cdMod) cdMod = sceCommonDialogInitialize() == 0;
    g_imeReady = imeMod && cdMod;
    logf_("ui: ime modules ime=%d commonDialog=%d", imeMod ? 1 : 0, cdMod ? 1 : 0);

    logf_("ui: ready, cell width %d, pad %s", g_cellW, padOk ? "ok" : "FAILED");
    g_ready = true;
    return true;
}

static void svc_step(uint64_t now)
{
    if (now < g_nextCheck) return;
    if (g_svc == S_CHECK || g_svc == S_FAILED) {
        std::vector<RootView> roots;
        std::string version;
        if (service_roots(roots, version)) {
            if (version != APP_VERSION) {
                logf_("svc: version mismatch (service=%s app=%s), sending payload", version.c_str(), APP_VERSION);
                g_svc = S_STARTING; g_startTries = 0; g_nextCheck = now;
                return;
            }
            g_svc = S_READY;
            std::string l, r, th;
            if (service_state_get(l, r, th)) g_theme = th.empty() ? "classic" : th;
            if (l.empty()) l = "/data";
            if (r.empty()) r = "/data";
            panel_load(g_panels[0], l, "");
            panel_load(g_panels[1], r, "");
            g_redraw = 2;
        } else {
            g_svc = S_STARTING; g_startTries = 0; g_nextCheck = now;
        }
    } else if (g_svc == S_STARTING) {
        if (g_startTries == 0) {
            bool connFailed = false;
            std::string err;
            if (!service_send_payload(err, &connFailed)) {
                g_svc = S_FAILED;
                g_svcErr = connFailed ? tr("svc_noloader", "BinLoader is off (GoldHEN - Servers - Enable Payloader Server).") : err;
                g_nextCheck = now + 2000000ULL;
                return;
            }
            service_install_autorun(err);
        }
        g_startTries++;
        std::vector<RootView> roots; std::string version;
        if (service_roots(roots, version) && version == APP_VERSION) {
            g_svc = S_READY;
            std::string l, r, th;
            if (service_state_get(l, r, th)) g_theme = th.empty() ? "classic" : th;
            panel_load(g_panels[0], l.empty() ? "/data" : l, "");
            panel_load(g_panels[1], r.empty() ? "/data" : r, "");
            g_redraw = 2;
        } else if (g_startTries > 25) {
            g_svc = S_FAILED; g_svcErr = tr("svc_timeout", "no answer from the service"); g_nextCheck = now + 2000000ULL;
        } else {
            g_nextCheck = now + 400000ULL;
        }
    }
}

void ui_pump()
{
    if (!g_ready) return;
    uint64_t now = sceKernelGetProcessTime();
    svc_step(now);

    unsigned b = pad_read();
    static unsigned prev = 0;
    unsigned pressed = b & ~prev;
    prev = b;
    bool cross = (pressed & ORBIS_PAD_BUTTON_CROSS) != 0;
    bool circle = (pressed & ORBIS_PAD_BUTTON_CIRCLE) != 0;
    bool square = (pressed & ORBIS_PAD_BUTTON_SQUARE) != 0;
    bool triangle = (pressed & ORBIS_PAD_BUTTON_TRIANGLE) != 0;
    bool options = (pressed & ORBIS_PAD_BUTTON_OPTIONS) != 0;
    bool up = (pressed & ORBIS_PAD_BUTTON_UP) != 0;
    bool down = (pressed & ORBIS_PAD_BUTTON_DOWN) != 0;
    bool l1 = (pressed & ORBIS_PAD_BUTTON_L1) != 0;
    bool r1 = (pressed & ORBIS_PAD_BUTTON_R1) != 0;
    bool l2 = (pressed & ORBIS_PAD_BUTTON_L2) != 0;
    bool r2 = (pressed & ORBIS_PAD_BUTTON_R2) != 0;

    if (g_svc != S_READY) {
        if (g_svc == S_FAILED && cross) { g_svc = S_CHECK; g_nextCheck = now; }
        goto redraw;
    }

    if (g_focus == FOCUS_PROGRESS) {
        if (cross) { if (g_prog == PROG_JOB) service_job_cancel(); else if (g_prog == PROG_PKG) service_pkg_cancel(); }
        goto redraw;
    }
    if (g_focus == FOCUS_DONATE) { if (circle || cross) g_focus = FOCUS_PANEL; goto redraw; }
    if (g_focus == FOCUS_MSG) { if (circle || cross) g_focus = FOCUS_PANEL; goto redraw; }
    if (g_focus == FOCUS_CONFIRM) {
        if (cross) {
            std::string err;
            if (g_confirm == CONF_DELETE) {
                if (service_delete(g_confirmPaths, true, err)) { g_prog = PROG_JOB; g_focus = FOCUS_PROGRESS; }
                else { g_msg = tr_err(err); g_msgIsError = true; g_focus = FOCUS_MSG; }
            } else if (g_confirm == CONF_OVERWRITE) {
                if (service_job_start(g_confirmOp, g_confirmPaths, g_confirmDest, true, true, "", err)) { g_prog = PROG_JOB; g_focus = FOCUS_PROGRESS; }
                else { g_msg = tr_err(err); g_msgIsError = true; g_focus = FOCUS_MSG; }
            } else if (g_confirm == CONF_PKG_INSTALL) {
                if (service_pkg_install(g_confirmPaths, err)) { g_prog = PROG_PKG; g_focus = FOCUS_PROGRESS; }
                else { g_msg = tr_err(err); g_msgIsError = true; g_focus = FOCUS_MSG; }
            }
            g_confirm = CONF_NONE;
        } else if (circle) { g_confirm = CONF_NONE; g_focus = FOCUS_PANEL; }
        goto redraw;
    }
    if (g_focus == FOCUS_CTX) {
        Entry2* e = panel_cur(g_panels[g_active]);
        bool pkg = e && is_pkg(*e);
        int n = ctx_count(pkg);
        if (up || down) { g_ctxCursor = up ? (g_ctxCursor - 1 + n) % n : (g_ctxCursor + 1) % n; }
        else if (circle) { g_focus = FOCUS_PANEL; }
        else if (cross) {
            if (g_ctxCursor == 3) g_focus = FOCUS_PANEL;                        // "Отмена" - всегда 4-й пункт (индекс 3)
            else { ctx_run(g_ctxCursor, pkg); if (g_focus != FOCUS_CONFIRM) g_focus = (g_focus == FOCUS_MSG) ? FOCUS_MSG : FOCUS_PANEL; }
        }
        goto redraw;
    }
    if (g_focus == FOCUS_APPMENU) {
        if (up || down) { g_appMenuCursor = up ? (g_appMenuCursor - 1 + APP_MENU_N) % APP_MENU_N : (g_appMenuCursor + 1) % APP_MENU_N; }
        else if (circle) { g_focus = FOCUS_PANEL; }
        else if (cross) { appmenu_run(g_appMenuCursor); if (g_appMenuCursor != 0 && g_appMenuCursor != 1) g_focus = FOCUS_PANEL; }
        goto redraw;
    }

    if (options) { g_focus = FOCUS_APPMENU; g_appMenuCursor = 0; goto redraw; }

    if (l1 || r1) {
        g_active = 1 - g_active;
        g_focus = FOCUS_PANEL;
        goto redraw;
    }

    if (g_focus == FOCUS_BAR) {
        if (l2) g_barCursor = (g_barCursor - 1 + 10) % 10;
        else if (r2) g_barCursor = (g_barCursor + 1) % 10;
        else if (circle || up) g_focus = FOCUS_PANEL;
        else if (cross) { run_bar_action(g_barCursor); if (g_focus == FOCUS_BAR) g_focus = FOCUS_PANEL; }
        goto redraw;
    }

    // FOCUS_PANEL
    if (l2 || r2) { g_focus = FOCUS_BAR; goto redraw; }
    {
        Panel& p = g_panels[g_active];
        if (up && !p.items.empty()) { p.cur = (p.cur - 1 + (int)p.items.size()) % (int)p.items.size(); }
        else if (down && !p.items.empty()) { p.cur = (p.cur + 1) % (int)p.items.size(); }
        else if (cross) { panel_enter(p); }
        else if (circle) { panel_up(p); }
        else if (square) {
            Entry2* e = panel_cur(p);
            if (e && e->name != "..") {
                auto it = std::find(p.sel.begin(), p.sel.end(), e->name);
                if (it == p.sel.end()) p.sel.push_back(e->name); else p.sel.erase(it);
                if (!p.items.empty()) p.cur = (p.cur + 1) % (int)p.items.size();   // как в Total Commander - курсор сдвигается вниз
            }
        }
        else if (triangle) {
            Entry2* e = panel_cur(p);
            if (e && e->name != "..") { g_ctxCursor = 0; g_focus = FOCUS_CTX; }
        }
    }

redraw:
    static uint64_t lastTick = 0;
    if (now - lastTick >= 1000000ULL) { lastTick = now; g_redraw = 2; }
    if (g_focus == FOCUS_PROGRESS) g_redraw = 2;   // прогресс обновляем каждый кадр, не только по тику

    if (g_redraw > 0) {
        draw_all();
        g_scene->SubmitFlip(g_frameID);
        g_scene->FrameWait(g_frameID);
        g_scene->FrameBufferSwap();
        g_frameID++;
        g_redraw--;
        static bool hidden = false;
        if (!hidden) { hidden = true; int r = sceSystemServiceHideSplashScreen(); logf_("ui: HideSplashScreen = 0x%08X", (unsigned)r); }
    }
}
