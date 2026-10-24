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
    // Rows below the gradient keep the fill's own pixels.
    for (std::uint32_t y = 0; y < std::min(out.height, gradient.height); ++y) {
        const std::size_t source = (static_cast<std::size_t>(y) * gradient.width + from) * 4;
        for (std::uint32_t x = 0; x < out.width; ++x) {
            std::copy_n(gradient.rgba.begin() + static_cast<std::ptrdiff_t>(source), 4,
                        out.rgba.begin() + static_cast<std::ptrdiff_t>((static_cast<std::size_t>(y) * out.width + x) * 4));
        }
    }
    // Then the fill's alpha masks every channel, as the game's meter does (premultiplied).
    for (std::size_t i = 0; i < out.rgba.size(); i += 4) {
        const unsigned a = fill.rgba[i + 3];
        for (std::size_t c = 0; c < 4; ++c) {
            out.rgba[i + c] = static_cast<std::uint8_t>(out.rgba[i + c] * a / 255);
        }
    }
    return out;
}

bool isPlatinum(int aspirationScore) { return aspirationScore >= 75; }

void NeedTrend::add(int value) {
    if (values_.size() >= kKept) {
        sum_ -= values_.front();
        values_.pop_front();
        averages_.pop_front();
    }
    values_.push_back(value);
    sum_ += value;
    const double average = static_cast<double>(sum_) / static_cast<double>(values_.size());
    averages_.push_back(static_cast<int>(average < 0 ? average - 0.5 : average + 0.5));
}

int NeedTrend::arrows() const {
    if (averages_.empty()) {
        return 0;
    }
    const int change = averages_.back() - averages_.front();
    const int count = std::abs(change) <= 2 ? 1 : std::abs(change) <= 4 ? 2 : 3;
    return change == 0 ? 0 : change < 0 ? -count : count;
}

void NeedTrend::clear() {
    values_.clear();
    averages_.clear();
    sum_ = 0;
}

tga::Pixels greyed(tga::Pixels picture) {
    for (std::size_t i = 0; i + 3 < picture.rgba.size(); i += 4) {
        const float grey = picture.rgba[i] * 0.3f + picture.rgba[i + 1] * 0.6f + picture.rgba[i + 2] * 0.1f;
        const auto v = static_cast<std::uint8_t>(std::min(255.0f, grey + 0.5f));
        picture.rgba[i] = picture.rgba[i + 1] = picture.rgba[i + 2] = v;
    }
    return picture;
}

int moodMeterValue(int mood, int aspirationScore) { return isPlatinum(aspirationScore) ? 100 : std::clamp(mood, -100, 100); }

AgeProgress ageProgress(std::int16_t ageDuration, std::int16_t ageDaysLeft) {
    // The game reads both as unsigned 16-bit values.
    const int length = static_cast<std::uint16_t>(ageDuration) + 1;
    const int left = std::min(static_cast<std::uint16_t>(ageDaysLeft) + 1, length);
    return {length - left, length};
}

int ageStage(std::int16_t lifeStage) {
    if (lifeStage <= 1) return -1;
    if (lifeStage == 2) return 0;
    if (lifeStage <= 15) return 1;
    if (lifeStage <= 18) return 2;
    if (lifeStage <= 50) return 3;
    return 4;
}

AgeTip ageTip(std::int16_t lifeStage, bool plantSim, std::int16_t ageDuration, std::int16_t ageDaysLeft, std::int16_t daysBefore) {
    const AgeProgress progress = ageProgress(ageDuration, ageDaysLeft);
    // The game keeps both counts in 16 bits, so a whole stage of 65536 days wraps to 0.
    const auto lived = static_cast<std::int16_t>(progress.lived);
    const auto left = static_cast<std::int16_t>(progress.length - progress.lived);
    switch (ageStage(lifeStage)) {
    case 4: return {0x34, daysBefore + lived};
    case 3: return {static_cast<std::uint16_t>(left == 1 ? 0x30 : 0x2C), left};
    case 2: return {static_cast<std::uint16_t>(left == 1 ? 0x31 : 0x2D), left};
    case 1: return {static_cast<std::uint16_t>(left == 1 ? 0x32 : 0x2E), left};
    case 0:
        if (plantSim) {
            return {static_cast<std::uint16_t>(left == 1 ? 0x3F : 0x3E), left};
        }
        return {static_cast<std::uint16_t>(left == 1 ? 0x33 : 0x2F), left};
    default: return {};
    }
}

std::uint32_t barFill(int lived, int length, std::uint32_t width) {
    if (length <= 0 || lived <= 0) {
        return 0;
    }
    if (lived >= length) {
        return width;
    }
    return static_cast<std::uint32_t>(static_cast<std::uint64_t>(width) * static_cast<std::uint64_t>(lived) / static_cast<std::uint64_t>(length));
}

int barPixels(int value, int min, int max, int steps, int size) {
    if (max <= min || size <= 0) {
        return 0;
    }
    value = std::clamp(value, min, max);
    const int step = std::max(1, max / std::max(1, steps));
    value -= value % step;
    // Worked out in doubles, as the game's x87 code (53-bit precision) does.
    const double pixels = static_cast<double>(size) / static_cast<double>(max - min) * static_cast<double>(value - min);
    const int filled = static_cast<int>(std::floor(pixels));
    return std::clamp(filled, 0, size - 1);
}

int nearestZodiacSign(int outgoing, int nice, int playful, int neat, int active) {
    // Outgoing, nice, playful, neat, active of each sign.
    static constexpr int kSigns[12][5] = {{8, 3, 3, 5, 6}, {5, 4, 8, 5, 3}, {7, 3, 3, 4, 8}, {3, 6, 4, 6, 6},
                                          {10, 3, 4, 4, 4}, {2, 5, 3, 9, 6}, {8, 7, 6, 2, 2}, {5, 3, 3, 6, 8},
                                          {3, 4, 7, 2, 9}, {4, 5, 8, 7, 1}, {4, 6, 7, 4, 4}, {3, 7, 3, 5, 7}};
    const double traits[5] = {outgoing * 0.01, nice * 0.01, playful * 0.01, neat * 0.01, active * 0.01};
    int best = 1;
    double bestDistance = 0;
    for (int sign = 0; sign < 12; ++sign) {
        double distance = 0;
        for (int t = 0; t < 5; ++t) {
            const double d = traits[t] - kSigns[sign][t];
            distance += d * d;
        }
        if (sign == 0 || distance < bestDistance) {
            best = sign + 1;
            bestDistance = distance;
        }
    }
    return best;
}

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
    // The middle 180 of a 256-pixel portrait (38 off each side), other sizes in proportion.
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
