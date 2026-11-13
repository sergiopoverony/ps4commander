#define STB_IMAGE_IMPLEMENTATION
#define STBI_NO_LINEAR
#define STBI_NO_HDR
#include <stb/stb_image.h>

#include "image.h"
#include "log.h"

Image::~Image()
{
    if (pixels) stbi_image_free(pixels);
}

bool Image::load(const char* path)
{
    int channels = 0;
    pixels = stbi_load(path, &width, &height, &channels, 4);   // всегда просим RGBA
    if (!pixels) {
        logf_("image: failed to load %s (%s)", path, stbi_failure_reason());
        width = 0;
        height = 0;
        return false;
    }
    return true;
}

bool Image::loadMem(const unsigned char* data, size_t size)
{
    int channels = 0;
    if (pixels) { stbi_image_free(pixels); pixels = nullptr; }
    pixels = stbi_load_from_memory(data, (int)size, &width, &height, &channels, 4);
    if (!pixels) {
        logf_("image: failed to decode memory image (%s)", stbi_failure_reason());
        width = 0;
        height = 0;
        return false;
    }
    return true;
}
