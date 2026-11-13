#pragma once

// Простая обёртка над stb_image: декодирует PNG в RGBA8 (4 байта на пиксель).
// Нужна для картинок интерфейса (например, title.png), не для фотографий —
// всё держим целиком в памяти, без потоковой обработки.
struct Image {
    int width = 0;
    int height = 0;
    unsigned char* pixels = nullptr;   // RGBA8, width*height*4 байт, или nullptr при неудаче

    ~Image();

    // Загружает PNG (или другой формат, который умеет stb_image) из файла.
    // true при успехе; при неудаче width/height остаются 0, причина пишется в лог.
    bool load(const char* path);
};
