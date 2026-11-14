#pragma once
// Программный декодер H.264 (OpenH264) для MP4: запасной путь, когда видеодекодер консоли не выдаёт кадры
// (например, потоки с 4 опорными кадрами и B-пирамидой). Звук при этом играет системный плеер, а картинку
// берём отсюда по тому же файлу (через службу, Range-запросы).
#include <stdint.h>
#include <string>

struct SwVideo;

struct SwFrame {
    int w = 0, h = 0;                       // видимый размер
    const uint8_t* y = nullptr;             // I420
    const uint8_t* u = nullptr;
    const uint8_t* v = nullptr;
    int strideY = 0, strideC = 0;
    long long ptsMs = 0;
};

// Разбирает MP4 (обычный, не фрагментированный) и создаёт декодер; nullptr - не получилось (причина в err).
SwVideo* sw_open(const std::string& path, unsigned long long fileSize, std::string& err);
void sw_close(SwVideo* s);
// Следующий кадр в порядке показа; false - файл закончился или ошибка. Указатели действительны до следующего вызова.
bool sw_next(SwVideo* s, SwFrame& out);
// Переход к ближайшему ключевому кадру не позже ms.
void sw_seek(SwVideo* s, long long ms);
int sw_width(SwVideo* s);
int sw_height(SwVideo* s);
