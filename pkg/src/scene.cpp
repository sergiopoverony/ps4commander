#include "scene.h"
#include "log.h"

#include <string.h>
#include <errno.h>

Scene2D::Scene2D(int w, int h, int pixelDepth)
    : width(w), height(h), depth(pixelDepth), video(0),
      directMemOff(0), directMemAllocationSize(0), videoMemSP(0), videoMem(NULL),
      frameBuffers(NULL), frameBufferSize(w * h * pixelDepth), frameBufferCount(0),
      activeFrameBufferIdx(0)
{
}

bool Scene2D::Init(size_t memSize, int numFrameBuffers)
{
    int rc;

    video = sceVideoOutOpen(ORBIS_VIDEO_USER_MAIN, ORBIS_VIDEO_OUT_BUS_MAIN, 0, 0);
    videoMem = NULL;

    if (video < 0) {
        logf_("ui: sceVideoOutOpen failed: %d", video);
        return false;
    }

    // Загружаем и инициализируем FreeType
    rc = sceSysmoduleLoadModule((OrbisSysModule)0x009A);
    if (rc < 0) {
        logf_("ui: cannot load freetype module: 0x%x", rc);
        return false;
    }

    rc = FT_Init_FreeType(&ftLib);
    if (rc != 0) {
        logf_("ui: FT_Init_FreeType failed: %d", rc);
        return false;
    }

    if (!initFlipQueue()) {
        logf_("ui: flip queue init failed");
        return false;
    }

    if (!allocateVideoMem(memSize, 0x200000)) {
        logf_("ui: cannot allocate video memory");
        return false;
    }

    if (!allocateFrameBuffers(numFrameBuffers)) {
        logf_("ui: cannot allocate frame buffers");
        return false;
    }

    sceVideoOutSetFlipRate(video, 0);
    activeFrameBufferIdx = 0;
    return true;
}

bool Scene2D::initFlipQueue()
{
    int rc = sceKernelCreateEqueue(&flipQueue, "ps4hddtools flip queue");
    if (rc < 0) return false;

    sceVideoOutAddFlipEvent(flipQueue, video, 0);
    return true;
}

bool Scene2D::allocateFrameBuffers(int num)
{
    frameBufferCount = num;
    frameBuffers = new char*[num];

    for (int i = 0; i < num; i++)
        frameBuffers[i] = allocateDisplayMem(frameBufferSize);

    // Формат пикселей SRGB
    sceVideoOutSetBufferAttribute(&attr, 0x80000000, 1, 0, width, height, width);

    return (sceVideoOutRegisterBuffers(video, 0, (void**)frameBuffers, num, &attr) == 0);
}

char* Scene2D::allocateDisplayMem(size_t size)
{
    char* allocatedPtr = (char*)videoMemSP;
    videoMemSP += size;
    return allocatedPtr;
}

bool Scene2D::allocateVideoMem(size_t size, int alignment)
{
    int rc;

    directMemAllocationSize = (size + alignment - 1) / alignment * alignment;

    rc = sceKernelAllocateDirectMemory(0, sceKernelGetDirectMemorySize(), directMemAllocationSize,
                                       alignment, 3, &directMemOff);
    if (rc < 0) {
        directMemAllocationSize = 0;
        return false;
    }

    rc = sceKernelMapDirectMemory(&videoMem, directMemAllocationSize, 0x33, 0, directMemOff, alignment);
    if (rc < 0) {
        sceKernelReleaseDirectMemory(directMemOff, directMemAllocationSize);
        directMemOff = 0;
        directMemAllocationSize = 0;
        return false;
    }

    videoMemSP = (uintptr_t)videoMem;
    return true;
}

void Scene2D::SubmitFlip(int frameID)
{
    sceVideoOutSubmitFlip(video, activeFrameBufferIdx, ORBIS_VIDEO_OUT_FLIP_VSYNC, frameID);
}

void Scene2D::FrameWait(int frameID)
{
    OrbisKernelEvent evt;
    int count;

    if (video == 0) return;

    for (;;) {
        OrbisVideoOutFlipStatus flipStatus;
        sceVideoOutGetFlipStatus(video, &flipStatus);

        if (flipStatus.flipArg == frameID) break;

        if (sceKernelWaitEqueue(flipQueue, &evt, 1, &count, 0) != 0) break;
    }
}

void Scene2D::FrameBufferSwap()
{
    if (frameBufferCount > 0)
        activeFrameBufferIdx = (activeFrameBufferIdx + 1) % frameBufferCount;
}

void Scene2D::FillRect(int x, int y, int w, int h, Color color)
{
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x + w > width)  w = width - x;
    if (y + h > height) h = height - y;
    if (w <= 0 || h <= 0) return;

    uint32_t v = 0x80000000u + ((uint32_t)color.r << 16) + ((uint32_t)color.g << 8) + color.b;
    uint32_t* base = (uint32_t*)frameBuffers[activeFrameBufferIdx];

    for (int yy = y; yy < y + h; yy++) {
        uint32_t* p = base + (size_t)yy * width + x;
        for (int xx = 0; xx < w; xx++) p[xx] = v;
    }
}

bool Scene2D::InitFont(FT_Face* face, const char* fontPath, int fontSize)
{
    int rc = FT_New_Face(ftLib, fontPath, 0, face);
    if (rc != 0) return false;

    rc = FT_Set_Pixel_Sizes(*face, 0, fontSize);
    if (rc != 0) return false;

    return true;
}
