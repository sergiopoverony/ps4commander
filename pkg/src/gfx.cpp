#include "gfx.h"
#include "log.h"

#include <math.h>
#include <string.h>
#include <unordered_map>

#define FRAME_DEPTH 4

static Scene2D* g_scene = NULL;
static FT_Face  g_font;
static int      g_cellW = 12;
static int      g_frameID = 0;
static std::string g_themeId = "classic";

Theme T;

static Color rgb(int r, int g, int b) { Color c = { (uint8_t)r, (uint8_t)g, (uint8_t)b }; return c; }

// Палитры - значения CSS-переменных из web/index.html (:root, [data-theme="dark"], [data-theme="light"]).
void gfx_set_theme(const std::string& id)
{
    if (id == "dark") {
        g_themeId = "dark";
        T.bg = rgb(14, 16, 21);      T.panel = rgb(22, 26, 33);   T.surface2 = rgb(30, 35, 45);  T.border = rgb(43, 49, 60);
        T.text = rgb(223, 227, 234); T.dir = rgb(242, 244, 248);  T.sel = rgb(255, 211, 122);
        T.curBg = rgb(42, 60, 95);   T.curFg = rgb(255, 255, 255); T.curInactive = rgb(30, 35, 45);
        T.dim = rgb(138, 147, 164);  T.dimmer = rgb(96, 103, 117);
        T.barBg = rgb(30, 35, 45);   T.barFg = rgb(223, 227, 234); T.barNum = rgb(138, 180, 255); T.barNumFg = rgb(11, 27, 58);
        T.warn = rgb(255, 138, 128); T.ok = rgb(123, 220, 154);
        T.dlgBg = rgb(30, 35, 45);   T.dlgFg = rgb(232, 235, 241); T.dlgBorder = rgb(47, 54, 67);
        T.accent = rgb(138, 180, 255); T.accentFg = rgb(11, 27, 58);
        T.modern = true; T.radius = 16;
    } else if (id == "light") {
        g_themeId = "light";
        T.bg = rgb(233, 236, 242);   T.panel = rgb(255, 255, 255); T.surface2 = rgb(243, 245, 250); T.border = rgb(214, 218, 227);
        T.text = rgb(31, 37, 48);    T.dir = rgb(15, 26, 51);      T.sel = rgb(179, 92, 0);
        T.curBg = rgb(217, 230, 255); T.curFg = rgb(10, 42, 102);  T.curInactive = rgb(243, 245, 250);
        T.dim = rgb(106, 115, 134);  T.dimmer = rgb(160, 167, 181);
        T.barBg = rgb(243, 245, 250); T.barFg = rgb(31, 37, 48);   T.barNum = rgb(26, 98, 214); T.barNumFg = rgb(255, 255, 255);
        T.warn = rgb(198, 40, 40);   T.ok = rgb(27, 127, 59);
        T.dlgBg = rgb(255, 255, 255); T.dlgFg = rgb(27, 33, 44);   T.dlgBorder = rgb(214, 218, 227);
        T.accent = rgb(26, 98, 214); T.accentFg = rgb(255, 255, 255);
        T.modern = true; T.radius = 16;
    } else {
        g_themeId = "classic";
        T.bg = rgb(0, 0, 122);       T.panel = rgb(0, 0, 168);     T.surface2 = rgb(0, 0, 140);  T.border = rgb(85, 255, 255);
        T.text = rgb(168, 168, 255); T.dir = rgb(255, 255, 255);   T.sel = rgb(255, 255, 85);
        T.curBg = rgb(0, 168, 168);  T.curFg = rgb(0, 0, 0);       T.curInactive = rgb(0, 0, 140);
        T.dim = rgb(122, 122, 200);  T.dimmer = rgb(90, 90, 170);
        T.barBg = rgb(0, 168, 168);  T.barFg = rgb(0, 0, 0);       T.barNum = rgb(168, 168, 168); T.barNumFg = rgb(0, 0, 0);
        T.warn = rgb(255, 85, 85);   T.ok = rgb(85, 255, 85);
        T.dlgBg = rgb(168, 168, 168); T.dlgFg = rgb(0, 0, 0);      T.dlgBorder = rgb(255, 255, 255);
        T.accent = rgb(85, 255, 255); T.accentFg = rgb(0, 0, 0);
        T.modern = false; T.radius = 0;
    }
}

const std::string& gfx_theme() { return g_themeId; }

int cell_w() { return g_cellW; }

bool gfx_init()
{
    gfx_set_theme("classic");
    g_scene = new Scene2D(FRAME_WIDTH, FRAME_HEIGHT, FRAME_DEPTH);
    if (!g_scene->Init(0xC000000, 2)) { logf_("gfx: scene init FAILED"); return false; }
    if (!g_scene->InitFont(&g_font, "/app0/assets/fonts/DejaVuSansMono.ttf", 22)) { logf_("gfx: font init FAILED"); return false; }
    FT_UInt gi = FT_Get_Char_Index(g_font, 'M');
    if (!FT_Load_Glyph(g_font, gi, FT_LOAD_DEFAULT)) {
        int w = (int)(g_font->glyph->advance.x >> 6);
        if (w > 0) g_cellW = w;
    }
    return true;
}

void gfx_present()
{
    g_scene->SubmitFlip(g_frameID);
    g_scene->FrameWait(g_frameID);
    g_scene->FrameBufferSwap();
    g_frameID++;
}

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

int count_cp(const std::string& s)
{
    const unsigned char* p = (const unsigned char*)s.c_str();
    int n = 0;
    while (*p) { decode_utf8(p); n++; }
    return n;
}

std::string fit(const std::string& s, int maxChars)
{
    if (maxChars <= 0) return "";
    if (count_cp(s) <= maxChars) return s;
    const unsigned char* start = (const unsigned char*)s.c_str();
    const unsigned char* p = start;
    for (int i = 0; i < maxChars - 1 && *p; i++) decode_utf8(p);
    return std::string((const char*)start, (size_t)(p - start)) + "\xE2\x80\xA6";
}

size_t utf8_to_utf16(const std::string& s, uint16_t* out, size_t outCap)
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

std::string utf16_to_utf8(const uint16_t* in, size_t cap)
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

// Перенос по словам: не длиннее maxc знаков в строке; "\n" - принудительный перенос.
std::vector<std::string> wrap_lines(const std::string& s, int maxc)
{
    std::vector<std::string> out;
    if (maxc < 4) maxc = 4;
    size_t start = 0;
    while (start <= s.size()) {
        size_t nl = s.find('\n', start);
        std::string para = s.substr(start, nl == std::string::npos ? std::string::npos : nl - start);
        const unsigned char* base = (const unsigned char*)para.c_str();
        size_t total = para.size(), pos = 0;
        if (total == 0) out.push_back("");
        while (pos < total) {
            const unsigned char* p = base + pos;
            int n = 0;
            while (*p && n < maxc) { decode_utf8(p); n++; }
            size_t byteEnd = (size_t)(p - base);
            if (byteEnd < total) {
                size_t sp = para.rfind(' ', byteEnd);
                if (sp != std::string::npos && sp > pos) byteEnd = sp;
            } else byteEnd = total;
            out.push_back(para.substr(pos, byteEnd - pos));
            pos = byteEnd;
            while (pos < total && para[pos] == ' ') pos++;
        }
        if (nl == std::string::npos) break;
        start = nl + 1;
    }
    return out;
}

// ---------------------------------------------------------------- примитивы

void fill(int x, int y, int w, int h, Color c) { g_scene->FillRect(x, y, w, h, c); }

void fill_rounded(int x, int y, int w, int h, int r, Color c)
{
    if (r <= 0) { fill(x, y, w, h, c); return; }
    if (r * 2 > h) r = h / 2;
    if (r * 2 > w) r = w / 2;
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

void draw_frame(int x, int y, int w, int h, int r, Color border, Color fillc)
{
    fill_rounded(x - 2, y - 2, w + 4, h + 4, r > 0 ? r + 2 : 0, border);
    fill_rounded(x, y, w, h, r, fillc);
}

// ---------------------------------------------------------------- текст (кэш глифов)

struct Glyph {
    int w = 0, h = 0, left = 0, top = 0, adv = 0;
    std::vector<unsigned char> a;
};
static std::unordered_map<int, Glyph> g_glyphs;

static const Glyph& glyph_for(int cp)
{
    auto it = g_glyphs.find(cp);
    if (it != g_glyphs.end()) return it->second;
    Glyph g;
    FT_GlyphSlot slot = g_font->glyph;
    FT_UInt gi = FT_Get_Char_Index(g_font, (FT_ULong)cp);
    if (!FT_Load_Glyph(g_font, gi, FT_LOAD_DEFAULT) && !FT_Render_Glyph(slot, ft_render_mode_normal)) {
        g.w = (int)slot->bitmap.width; g.h = (int)slot->bitmap.rows;
        g.left = slot->bitmap_left; g.top = slot->bitmap_top;
        g.adv = (int)(slot->advance.x >> 6);
        g.a.resize((size_t)g.w * g.h);
        for (int yy = 0; yy < g.h; yy++)
            for (int xx = 0; xx < g.w; xx++) g.a[(size_t)yy * g.w + xx] = slot->bitmap.buffer[yy * slot->bitmap.pitch + xx];
    } else {
        g.adv = g_cellW;
    }
    return g_glyphs.emplace(cp, std::move(g)).first->second;
}

void draw_text(const std::string& s, int x, int baseline, Color fg, Color bg)
{
    const unsigned char* p = (const unsigned char*)s.c_str();
    while (*p) {
        int cp = decode_utf8(p);
        const Glyph& g = glyph_for(cp);
        for (int yy = 0; yy < g.h; yy++) {
            int py = baseline + yy - g.top;
            if (py < 0 || py >= FRAME_HEIGHT) continue;
            for (int xx = 0; xx < g.w; xx++) {
                unsigned a = g.a[(size_t)yy * g.w + xx];
                if (a == 0) continue;
                int px = x + xx + g.left;
                if (px < 0 || px >= FRAME_WIDTH) continue;
                Color c;
                c.r = (uint8_t)((bg.r * (255 - a) + fg.r * a) / 255);
                c.g = (uint8_t)((bg.g * (255 - a) + fg.g * a) / 255);
                c.b = (uint8_t)((bg.b * (255 - a) + fg.b * a) / 255);
                g_scene->PutPixel(px, py, c);
            }
        }
        x += g.adv;
    }
}

void put_center(int cx, const std::string& s, int baseline, Color fg, Color bg)
{
    draw_text(s, cx - (count_cp(s) * g_cellW) / 2, baseline, fg, bg);
}

// ---------------------------------------------------------------- значки кнопок геймпада

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

// Цвета фирменных кнопок PlayStation; на тёмных/классической темах они яркие, на светлой - чуть темнее.
static Color icon_color(Icon k)
{
    bool light = g_themeId == "light";
    switch (k) {
    case ICON_CROSS:    return light ? rgb(40, 100, 220) : rgb(120, 170, 255);
    case ICON_CIRCLE:   return light ? rgb(210, 50, 50) : rgb(255, 100, 100);
    case ICON_SQUARE:   return light ? rgb(200, 60, 150) : rgb(240, 130, 200);
    case ICON_TRIANGLE: return light ? rgb(20, 150, 110) : rgb(90, 210, 170);
    default:            return light ? rgb(70, 76, 90) : rgb(200, 204, 212);
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

struct RichTok { bool isIcon; Icon icon; std::string text; };

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
            Icon k;
            if (e != std::string::npos && icon_by_name(s.substr(i + 1, e - i - 1), k)) {
                if (!cur.empty()) { RichTok t; t.isIcon = false; t.icon = ICON_CROSS; t.text = cur; out.push_back(t); cur.clear(); }
                RichTok t; t.isIcon = true; t.icon = k; out.push_back(t);
                i = e + 1;
                continue;
            }
        }
        cur += s[i++];
    }
    if (!cur.empty()) { RichTok t; t.isIcon = false; t.icon = ICON_CROSS; t.text = cur; out.push_back(t); }
    return out;
}

int rich_width(const std::string& s)
{
    int w = 0;
    for (auto& t : rich_parse(s)) w += t.isIcon ? (ICON_S + 6) : count_cp(t.text) * g_cellW;
    return w;
}

void draw_rich(const std::string& s, int x, int baseline, Color fg, Color bg)
{
    for (auto& t : rich_parse(s)) {
        if (t.isIcon) { draw_icon(t.icon, x, baseline - ICON_S + 3, bg); x += ICON_S + 6; }
        else { draw_text(t.text, x, baseline, fg, bg); x += count_cp(t.text) * g_cellW; }
    }
}

void put_center_rich(int cx, const std::string& s, int baseline, Color fg, Color bg)
{
    draw_rich(s, cx - rich_width(s) / 2, baseline, fg, bg);
}
