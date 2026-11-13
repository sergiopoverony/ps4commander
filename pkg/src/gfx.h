#pragma once
// Рисование: экран 1920x1080, текст (FreeType с кэшем глифов), значки кнопок геймпада, цветовые темы
// (те же три темы, что в веб-версии: classic / dark / light).
#include <string>
#include <vector>
#include <stdint.h>
#include "scene.h"

#define FRAME_WIDTH   1920
#define FRAME_HEIGHT  1080

struct Theme {
    Color bg, panel, surface2, border, text, dir, sel, curBg, curFg, curInactive, dim, dimmer;
    Color barBg, barFg, barNum, barNumFg, warn, ok, dlgBg, dlgFg, dlgBorder, accent, accentFg;
    bool modern;      // dark/light: скруглённые панели; classic: прямые углы, как в Norton Commander
    int radius;       // радиус скругления панелей/диалогов
};
extern Theme T;

void gfx_set_theme(const std::string& id);   // "classic" | "dark" | "light"; иное - classic
const std::string& gfx_theme();

bool gfx_init();                // экран + шрифт
void gfx_present();             // показать нарисованный кадр
int  cell_w();                  // ширина знакоместа (моноширинный шрифт)

// --- UTF-8 / UTF-16
int  count_cp(const std::string& s);
std::string fit(const std::string& s, int maxChars);
size_t utf8_to_utf16(const std::string& s, uint16_t* out, size_t outCap);
std::string utf16_to_utf8(const uint16_t* in, size_t cap);
std::vector<std::string> wrap_lines(const std::string& s, int maxc);   // перенос по словам, "\n" - обязательный

// --- примитивы
void fill(int x, int y, int w, int h, Color c);
void fill_rounded(int x, int y, int w, int h, int r, Color c);
void draw_frame(int x, int y, int w, int h, int r, Color border, Color fillc);

// --- текст
void draw_text(const std::string& s, int x, int baseline, Color fg, Color bg);
void put_center(int cx, const std::string& s, int baseline, Color fg, Color bg);

// Строка с вкраплениями значков: {cross} {circle} {square} {triangle} {options} {arrows} {updown}.
// L1/R1/L2/R2 значками не рисуются - пишутся обычным текстом (см. вызывающий код).
void put_center_rich(int cx, const std::string& s, int baseline, Color fg, Color bg);
void draw_rich(const std::string& s, int x, int baseline, Color fg, Color bg);
int  rich_width(const std::string& s);
