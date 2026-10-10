#include "formats/image.hpp"

#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_PNG
#define STBI_ONLY_JPEG
#define STBI_ONLY_BMP
#define STBI_NO_STDIO
#include "stb_image.h"

namespace ts2::image {

std::optional<tga::Pixels> decode(std::span<const std::uint8_t> data) {
    const bool other = data.size() >= 4 && ((data[0] == 0x89 && data[1] == 'P') || (data[0] == 0xFF && data[1] == 0xD8) ||
                                            (data[0] == 'B' && data[1] == 'M'));
    if (!other) {
        return tga::decode(data);
    }
    int w = 0, h = 0, n = 0;
    stbi_uc* pixels = stbi_load_from_memory(data.data(), static_cast<int>(data.size()), &w, &h, &n, 4);
    if (pixels == nullptr) {
        return std::nullopt;
    }
    tga::Pixels out;
    out.width = static_cast<std::uint32_t>(w);
    out.height = static_cast<std::uint32_t>(h);
    out.rgba.assign(pixels, pixels + static_cast<std::size_t>(w) * h * 4);
    stbi_image_free(pixels);
    return out;
}

} // namespace ts2::image
