#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_PNG          // нам нужны только PNG (иконка и баннер), меньше кода в итоге
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
