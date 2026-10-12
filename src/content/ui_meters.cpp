#include "content/ui_meters.hpp"

#include <algorithm>
#include <cmath>
#include <vector>

namespace ts2::content {

namespace {

// How dark a portrait's shadow gets where the portrait is opaque.
constexpr float kSilhouetteStrength = 210.0f / 255.0f;

bool complete(const tga::Pixels& p) {
    return p.width > 0 && p.height > 0 && p.rgba.size() == static_cast<std::size_t>(p.width) * p.height * 4;
}

} // namespace

int meterRow(int value, int min, int max, int rows) {
    if (rows <= 0 || max <= min) {
        return 0;
    }
    value = std::clamp(value, min, max);
    // The game works this out in floats and rounds down.
    const float scale = static_cast<float>(rows) / (static_cast<float>(max) - static_cast<float>(min));
    const int row = static_cast<int>(std::floor((static_cast<float>(value) - static_cast<float>(min)) * scale));
    return std::clamp(row, 0, rows - 1);
}

int moodRow(int mood, int rows) {
    if (rows <= 0) {
        return 0;
    }
    mood = std::clamp(mood, -100, 100);
    const int filled = static_cast<int>(std::floor(static_cast<float>(rows) * static_cast<float>(mood + 100) * 0.005f));
    return std::clamp(rows - filled, 0, rows - 1);
}

std::array<std::uint8_t, 3> pixel(const tga::Pixels& picture, std::uint32_t x, std::uint32_t y) {
    if (!complete(picture) || x >= picture.width || y >= picture.height) {
        return {0, 0, 0};
    }
    const std::size_t at = (static_cast<std::size_t>(y) * picture.width + x) * 4;
    return {picture.rgba[at], picture.rgba[at + 1], picture.rgba[at + 2]};
}

tga::Pixels paintBar(const tga::Pixels& fill, const tga::Pixels& gradient, int row) {
    if (!complete(fill) || !complete(gradient)) {
        return fill;
    }
    const auto from = static_cast<std::uint32_t>(std::clamp(row, 0, static_cast<int>(gradient.height) - 1));
    tga::Pixels out = fill;
    for (std::uint32_t y = 0; y < out.height; ++y) {
        for (std::uint32_t x = 0; x < out.width; ++x) {
            const auto colour = pixel(gradient, std::min(x, gradient.width - 1), from);
            std::copy(colour.begin(), colour.end(), out.rgba.begin() + static_cast<std::ptrdiff_t>((static_cast<std::size_t>(y) * out.width + x) * 4));
        }
    }
    return out;
}

tga::Pixels paintColumns(const tga::Pixels& fill, const tga::Pixels& gradient, int column) {
    if (!complete(fill) || !complete(gradient)) {
        return fill;
    }
    const auto from = static_cast<std::uint32_t>(std::clamp(column, 0, static_cast<int>(gradient.width) - 1));
    tga::Pixels out = fill;
    for (std::uint32_t y = 0; y < out.height; ++y) {
        const auto colour = pixel(gradient, from, std::min(y, gradient.height - 1));
        for (std::uint32_t x = 0; x < out.width; ++x) {
            std::copy(colour.begin(), colour.end(), out.rgba.begin() + static_cast<std::ptrdiff_t>((static_cast<std::size_t>(y) * out.width + x) * 4));
        }
    }
    return out;
}

bool isPlatinum(int aspirationScore) { return aspirationScore >= 75; }

int moodMeterValue(int mood, int aspirationScore) { return isPlatinum(aspirationScore) ? 100 : std::clamp(mood, -100, 100); }

tga::Pixels placed(const tga::Pixels& picture, std::uint32_t width, std::uint32_t height, int x, int y) {
    if (width > tga::kMaxSide || height > tga::kMaxSide) {
        return {};
    }
    tga::Pixels out{width, height, std::vector<std::uint8_t>(static_cast<std::size_t>(width) * height * 4, 0)};
    if (!complete(picture)) {
        return out;
    }
    for (std::uint32_t py = 0; py < picture.height; ++py) {
        const long long ty = static_cast<long long>(y) + py;
        if (ty < 0 || ty >= height) {
            continue;
        }
        for (std::uint32_t px = 0; px < picture.width; ++px) {
            const long long tx = static_cast<long long>(x) + px;
            if (tx < 0 || tx >= width) {
                continue;
            }
            const auto from = picture.rgba.begin() + static_cast<std::ptrdiff_t>((static_cast<std::size_t>(py) * picture.width + px) * 4);
            std::copy(from, from + 4, out.rgba.begin() + static_cast<std::ptrdiff_t>((static_cast<std::size_t>(ty) * width + static_cast<std::size_t>(tx)) * 4));
        }
    }
    return out;
}

tga::Pixels portraitCrop(const tga::Pixels& portrait) {
    // The middle 180 of a 256-pixel portrait (38 off each side); other sizes in proportion.
    const auto scale = [&](std::uint32_t v) { return static_cast<int>(static_cast<std::uint64_t>(v) * portrait.width / 256); };
    const int side = scale(180);
    if (!complete(portrait) || portrait.width != portrait.height || portrait.width > kMaxPortraitSide || side <= 0) {
        return portrait.width > kMaxPortraitSide ? tga::Pixels{} : portrait;
    }
    return placed(portrait, static_cast<std::uint32_t>(side), static_cast<std::uint32_t>(side), -scale(38), -scale(38));
}

tga::Pixels portraitSilhouette(const tga::Pixels& portrait) {
    if (!complete(portrait) || portrait.width > kMaxPortraitSide || portrait.height > kMaxPortraitSide) {
        return {};
    }
    const std::uint32_t w = portrait.width, h = portrait.height;
    const int radius = static_cast<int>(std::max<std::uint64_t>(1, static_cast<std::uint64_t>(9) * w / 256));
    std::vector<float> a(static_cast<std::size_t>(w) * h), line;
    for (std::size_t i = 0; i < a.size(); ++i) {
        a[i] = portrait.rgba[i * 4 + 3];
    }
    // One box blur along a row or column, the edge pixels repeated beyond it.
    auto blur = [&](std::size_t start, std::size_t step, std::uint32_t count) {
        line.assign(count, 0);
        auto at = [&](long long i) { return a[start + step * static_cast<std::size_t>(std::clamp<long long>(i, 0, count - 1))]; };
        float sum = 0;
        for (int k = -radius; k <= radius; ++k) {
            sum += at(k);
        }
        for (std::uint32_t i = 0; i < count; ++i) {
            line[i] = sum / static_cast<float>(2 * radius + 1);
            sum += at(static_cast<long long>(i) + radius + 1) - at(static_cast<long long>(i) - radius);
        }
        for (std::uint32_t i = 0; i < count; ++i) {
            a[start + step * i] = line[i];
        }
    };
    for (int pass = 0; pass < 3; ++pass) {
        for (std::uint32_t y = 0; y < h; ++y) {
            blur(static_cast<std::size_t>(y) * w, 1, w);
        }
        for (std::uint32_t x = 0; x < w; ++x) {
            blur(x, w, h);
        }
    }
    tga::Pixels out{w, h, std::vector<std::uint8_t>(a.size() * 4, 0)};
    for (std::size_t i = 0; i < a.size(); ++i) {
        out.rgba[i * 4 + 3] = static_cast<std::uint8_t>(std::clamp(std::lround(a[i] * kSilhouetteStrength), 0L, 255L));
    }
    return out;
}

tga::Pixels scaled(const tga::Pixels& picture, std::uint32_t width, std::uint32_t height) {
    if (!complete(picture) || width == 0 || height == 0 || width > tga::kMaxSide || height > tga::kMaxSide) {
        return {};
    }
    tga::Pixels out{width, height, std::vector<std::uint8_t>(static_cast<std::size_t>(width) * height * 4, 0)};
    const double kx = static_cast<double>(picture.width) / width, ky = static_cast<double>(picture.height) / height;
    for (std::uint32_t y = 0; y < height; ++y) {
        const double y0 = y * ky, y1 = (y + 1) * ky;
        for (std::uint32_t x = 0; x < width; ++x) {
            const double x0 = x * kx, x1 = (x + 1) * kx;
            double sum[4] = {0, 0, 0, 0}, area = 0;  // colour sums weighted by alpha
            for (auto sy = static_cast<std::uint32_t>(y0); sy < picture.height && sy < y1; ++sy) {
                const double h = std::min<double>(sy + 1, y1) - std::max<double>(sy, y0);
                for (auto sx = static_cast<std::uint32_t>(x0); sx < picture.width && sx < x1; ++sx) {
                    const double a = h * (std::min<double>(sx + 1, x1) - std::max<double>(sx, x0));
                    const std::uint8_t* p = &picture.rgba[(static_cast<std::size_t>(sy) * picture.width + sx) * 4];
                    for (int c = 0; c < 3; ++c) {
                        sum[c] += a * p[3] * p[c];
                    }
                    sum[3] += a * p[3];
                    area += a;
                }
            }
            std::uint8_t* o = &out.rgba[(static_cast<std::size_t>(y) * width + x) * 4];
            for (int c = 0; c < 3; ++c) {
                o[c] = sum[3] > 0 ? static_cast<std::uint8_t>(std::clamp(std::lround(sum[c] / sum[3]), 0L, 255L)) : 0;
            }
            o[3] = area > 0 ? static_cast<std::uint8_t>(std::clamp(std::lround(sum[3] / area), 0L, 255L)) : 0;
        }
    }
    return out;
}

tga::Pixels masked(const tga::Pixels& picture, const tga::Pixels& shape) {
    if (!complete(picture) || !complete(shape) || picture.width != shape.width || picture.height != shape.height) {
        return picture;
    }
    tga::Pixels out = picture;
    for (std::size_t i = 3; i < out.rgba.size(); i += 4) {
        out.rgba[i] = static_cast<std::uint8_t>((out.rgba[i] * shape.rgba[i] + 127) / 255);
    }
    return out;
}

tga::Pixels unpremultiplied(tga::Pixels picture) {
    if (!complete(picture)) {
        return picture;
    }
    for (std::size_t i = 0; i < picture.rgba.size(); i += 4) {
        const int a = picture.rgba[i + 3];
        for (std::size_t c = 0; c < 3 && a > 0; ++c) {
            picture.rgba[i + c] = static_cast<std::uint8_t>(std::min(255, (picture.rgba[i + c] * 255 + a / 2) / a));
        }
    }
    return picture;
}

tga::Pixels fillShape(const tga::Pixels& shape, std::array<std::uint8_t, 3> colour) {
    if (!complete(shape)) {
        return shape;
    }
    tga::Pixels out = shape;
    for (std::size_t at = 0; at < out.rgba.size(); at += 4) {
        std::copy(colour.begin(), colour.end(), out.rgba.begin() + static_cast<std::ptrdiff_t>(at));
    }
    return out;
}

} // namespace ts2::content
