#pragma once
// Экран 1920x1080: своя копия Scene2D из примеров OpenOrbis.
// Отличия: нет зависимости от log.h примеров, всегда есть шрифт (FreeType),
// быстрая заливка прямоугольников, правильная начальная инициализация.

#include <stdint.h>
#include <stddef.h>
#include <orbis/libkernel.h>
#include <orbis/VideoOut.h>
#include <orbis/Sysmodule.h>
#include <proto-include.h>

struct Color {
    uint8_t r;
    uint8_t g;
    uint8_t b;
};

class Scene2D {
public:
    Scene2D(int w, int h, int pixelDepth);

    bool Init(size_t memSize, int numFrameBuffers);
    bool InitFont(FT_Face* face, const char* fontPath, int fontSize);

    void SubmitFlip(int frameID);
    void FrameWait(int frameID);
    void FrameBufferSwap();

    // Заливка прямоугольника цветом (с обрезкой по границам экрана).
    void FillRect(int x, int y, int w, int h, Color color);

    // Один пиксель; координаты должны быть внутри экрана.
    inline void PutPixel(int x, int y, Color c)
    {
        uint32_t* fb = (uint32_t*)frameBuffers[activeFrameBufferIdx];
        fb[y * width + x] = 0x80000000u + ((uint32_t)c.r << 16) + ((uint32_t)c.g << 8) + c.b;
    }

private:
    int width;
    int height;
    int depth;
    int video;

    off_t directMemOff;
    size_t directMemAllocationSize;

    uintptr_t videoMemSP;
    void* videoMem;

    char** frameBuffers;
    OrbisKernelEqueue flipQueue;
    OrbisVideoOutBufferAttribute attr;

    int frameBufferSize;
    int frameBufferCount;
    int activeFrameBufferIdx;

    FT_Library ftLib;

    bool initFlipQueue();
    bool allocateFrameBuffers(int num);
    char* allocateDisplayMem(size_t size);
    bool allocateVideoMem(size_t size, int alignment);
};
