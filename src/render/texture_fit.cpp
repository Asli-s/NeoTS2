#include "render/texture_fit.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <utility>

namespace ts2::render {

namespace {

bool isPowerOfTwo(unsigned v) {
    return v != 0 && (v & (v - 1)) == 0;
}

unsigned nextPowerOfTwo(unsigned v) {
    unsigned p = 1;
    while (p < v) {
        p <<= 1;
    }
    return p;
}

struct Tap {
    std::uint32_t index;
    float weight;
};

// For each of `dst` positions, the source positions that make it and their weights (summing to 1).
std::vector<std::vector<Tap>> axisTaps(unsigned src, unsigned dst) {
    std::vector<std::vector<Tap>> taps(dst);
    const double scale = static_cast<double>(src) / dst;
    for (unsigned d = 0; d < dst; ++d) {
        if (dst < src) {  // shrinking: the area it covers
            const double a = d * scale, b = (d + 1) * scale;
            for (auto s = static_cast<unsigned>(std::floor(a)); s < src && s < std::ceil(b); ++s) {
                const double w = std::min(b, s + 1.0) - std::max(a, static_cast<double>(s));
                if (w > 0) {
                    taps[d].push_back({s, static_cast<float>(w / scale)});
                }
            }
        } else {  // growing: between the two nearest
            const double c = (d + 0.5) * scale - 0.5;
            const auto s0 = static_cast<long>(std::floor(c));
            const auto t = static_cast<float>(c - static_cast<double>(s0));
            const auto clampTo = [&](long s) { return static_cast<std::uint32_t>(std::clamp(s, 0L, static_cast<long>(src) - 1)); };
            taps[d].push_back({clampTo(s0), 1 - t});
            taps[d].push_back({clampTo(s0 + 1), t});
        }
    }
    return taps;
}

} // namespace

bool fitsTexture(unsigned width, unsigned height, const TextureLimits& limits) {
    return isPowerOfTwo(width) && isPowerOfTwo(height) && width >= limits.minSide && height >= limits.minSide && width <= limits.maxSide &&
           height <= limits.maxSide;
}

unsigned fitSide(unsigned side, const TextureLimits& limits) {
    if (side == 0) {
        return limits.minSide;
    }
    const auto nearest = static_cast<unsigned>(std::lround(std::exp2(std::round(std::log2(static_cast<double>(side))))));
    return std::clamp(nearest, limits.minSide, limits.maxSide);
}

ClampedFit fitClamped(unsigned width, unsigned height, const TextureLimits& limits) {
    ClampedFit fit;
    const unsigned w = std::max(nextPowerOfTwo(width), limits.minSide), h = std::max(nextPowerOfTwo(height), limits.minSide);
    if (width != 0 && height != 0 && w <= limits.maxSide && h <= limits.maxSide) {
        fit.width = w;
        fit.height = h;
        fit.u = static_cast<float>(width) / static_cast<float>(w);
        fit.v = static_cast<float>(height) / static_cast<float>(h);
        return fit;
    }
    fit.width = fitSide(width, limits);
    fit.height = fitSide(height, limits);
    fit.stretch = true;
    return fit;
}

scene::Image stretched(const scene::Image& image, unsigned width, unsigned height) {
    scene::Image out;
    out.width = width;
    out.height = height;
    out.rgba.resize(std::size_t{width} * height * 4);
    if (image.width == 0 || image.height == 0 || width == 0 || height == 0) {
        return out;
    }
    const auto xs = axisTaps(image.width, width), ys = axisTaps(image.height, height);
    for (unsigned y = 0; y < height; ++y) {
        for (unsigned x = 0; x < width; ++x) {
            float sum[4] = {};
            for (const Tap& ty : ys[y]) {
                for (const Tap& tx : xs[x]) {
                    const std::uint8_t* texel = &image.rgba[(std::size_t{ty.index} * image.width + tx.index) * 4];
                    const float weight = ty.weight * tx.weight;
                    for (std::size_t k = 0; k < 4; ++k) {
                        sum[k] += weight * texel[k];
                    }
                }
            }
            std::uint8_t* at = &out.rgba[(std::size_t{y} * width + x) * 4];
            for (std::size_t k = 0; k < 4; ++k) {
                at[k] = static_cast<std::uint8_t>(std::lround(std::clamp(sum[k], 0.0f, 255.0f)));
            }
        }
    }
    return out;
}

scene::Image padded(const scene::Image& image, unsigned width, unsigned height) {
    scene::Image out;
    out.width = width;
    out.height = height;
    out.rgba.resize(std::size_t{width} * height * 4);
    if (image.width == 0 || image.height == 0) {
        return out;
    }
    for (unsigned y = 0; y < height; ++y) {
        const unsigned sy = std::min(y, image.height - 1);
        for (unsigned x = 0; x < width; ++x) {
            const unsigned sx = std::min(x, image.width - 1);
            std::copy_n(&image.rgba[(std::size_t{sy} * image.width + sx) * 4], 4, &out.rgba[(std::size_t{y} * width + x) * 4]);
        }
    }
    return out;
}

scene::Image quantized(const scene::Image& image, unsigned colours, const QuantizeOptions& options) {
    scene::Image out = image;
    out.smallerLevels.clear();
    const std::size_t count = std::size_t{image.width} * image.height;
    if (colours < 2 || count == 0 || out.rgba.size() != count * 4) {
        return out;
    }
    struct Box {
        std::size_t first, last;  // a range of `order`
    };
    std::vector<std::uint32_t> order(count);
    for (std::size_t i = 0; i < count; ++i) {
        order[i] = static_cast<std::uint32_t>(i);
    }
    const auto channel = [&](std::uint32_t pixel, std::size_t k) { return out.rgba[std::size_t{pixel} * 4 + k]; };
    // The channel a box varies the most in, and by how much.
    const auto widest = [&](const Box& box) {
        std::size_t best = 0;
        int bestRange = -1;
        for (std::size_t k = 0; k < 4; ++k) {
            int low = 255, high = 0;
            for (std::size_t i = box.first; i < box.last; ++i) {
                low = std::min<int>(low, channel(order[i], k));
                high = std::max<int>(high, channel(order[i], k));
            }
            if (high - low > bestRange) {
                bestRange = high - low;
                best = k;
            }
        }
        return std::pair{best, bestRange};
    };
    std::vector<Box> boxes{{0, count}};
    while (boxes.size() < colours) {
        std::size_t pick = boxes.size();
        int pickRange = 0;
        for (std::size_t b = 0; b < boxes.size(); ++b) {
            if (boxes[b].last - boxes[b].first < 2) {
                continue;
            }
            const int range = widest(boxes[b]).second;
            if (range > pickRange) {
                pickRange = range;
                pick = b;
            }
        }
        if (pick == boxes.size()) {
            break;  // every box is one colour
        }
        const Box box = boxes[pick];
        const std::size_t k = widest(box).first;
        std::sort(order.begin() + static_cast<std::ptrdiff_t>(box.first), order.begin() + static_cast<std::ptrdiff_t>(box.last),
                  [&](std::uint32_t a, std::uint32_t b) { return channel(a, k) < channel(b, k); });
        const std::size_t middle = box.first + (box.last - box.first) / 2;
        boxes[pick] = {box.first, middle};
        boxes.push_back({middle, box.last});
    }
    // Each box's average is a palette colour.
    std::vector<std::array<float, 4>> palette(boxes.size());
    std::vector<std::uint8_t> result(out.rgba.size());
    for (std::size_t b = 0; b < boxes.size(); ++b) {
        std::size_t sum[4] = {};
        for (std::size_t i = boxes[b].first; i < boxes[b].last; ++i) {
            for (std::size_t k = 0; k < 4; ++k) {
                sum[k] += channel(order[i], k);
            }
        }
        const std::size_t n = boxes[b].last - boxes[b].first;
        for (std::size_t k = 0; k < 4; ++k) {
            palette[b][k] = static_cast<float>(sum[k]) / static_cast<float>(n);
        }
        if (options.iterations == 0 && !options.dither) {
            for (std::size_t i = boxes[b].first; i < boxes[b].last; ++i) {
                for (std::size_t k = 0; k < 4; ++k) {
                    result[std::size_t{order[i]} * 4 + k] = static_cast<std::uint8_t>((sum[k] + n / 2) / n);
                }
            }
        }
    }
    if (options.iterations == 0 && !options.dither) {
        out.rgba = std::move(result);
        return out;
    }
    const auto nearest = [&](const float* px) {
        std::size_t best = 0;
        float bestDistance = 1e30f;
        for (std::size_t c = 0; c < palette.size(); ++c) {
            float distance = 0;
            for (std::size_t k = 0; k < 4; ++k) {
                const float d = px[k] - palette[c][k];
                distance += d * d;
            }
            if (distance < bestDistance) {
                bestDistance = distance;
                best = c;
            }
        }
        return best;
    };
    // k-means on a sample of the pixels: each takes the nearest colour, and each colour moves to the average of its pixels.
    const std::size_t sampleSize = std::min<std::size_t>(count, 4096);
    for (unsigned pass = 0; pass < options.iterations; ++pass) {
        std::vector<std::array<double, 5>> sums(palette.size());
        for (std::size_t s = 0; s < sampleSize; ++s) {
            const std::size_t pixel = s * count / sampleSize;
            float px[4];
            for (std::size_t k = 0; k < 4; ++k) {
                px[k] = out.rgba[pixel * 4 + k];
            }
            auto& sum = sums[nearest(px)];
            for (std::size_t k = 0; k < 4; ++k) {
                sum[k] += px[k];
            }
            sum[4] += 1;
        }
        for (std::size_t c = 0; c < palette.size(); ++c) {
            if (sums[c][4] > 0) {
                for (std::size_t k = 0; k < 4; ++k) {
                    palette[c][k] = static_cast<float>(sums[c][k] / sums[c][4]);
                }
            }
        }
    }
    // Every pixel takes the nearest colour; dithering adds what the pixels before it lost to the colour of the next.
    const std::size_t width = image.width, height = image.height;
    std::vector<float> thisRow((width + 2) * 3, 0.0f), nextRow((width + 2) * 3, 0.0f);
    for (std::size_t y = 0; y < height; ++y) {
        for (std::size_t x = 0; x < width; ++x) {
            const std::size_t pixel = y * width + x;
            float px[4];
            for (std::size_t k = 0; k < 4; ++k) {
                px[k] = out.rgba[pixel * 4 + k];
            }
            if (options.dither) {
                for (std::size_t k = 0; k < 3; ++k) {
                    px[k] = std::clamp(px[k] + thisRow[(x + 1) * 3 + k], 0.0f, 255.0f);
                }
            }
            const std::size_t chosen = nearest(px);
            for (std::size_t k = 0; k < 4; ++k) {
                result[pixel * 4 + k] = static_cast<std::uint8_t>(std::lround(std::clamp(palette[chosen][k], 0.0f, 255.0f)));
            }
            if (options.dither) {
                for (std::size_t k = 0; k < 3; ++k) {
                    const float error = px[k] - palette[chosen][k];
                    thisRow[(x + 2) * 3 + k] += error * 7 / 16;
                    nextRow[x * 3 + k] += error * 3 / 16;
                    nextRow[(x + 1) * 3 + k] += error * 5 / 16;
                    nextRow[(x + 2) * 3 + k] += error * 1 / 16;
                }
            }
        }
        thisRow.swap(nextRow);
        std::fill(nextRow.begin(), nextRow.end(), 0.0f);
    }
    out.rgba = std::move(result);
    return out;
}

double imageError(const scene::Image& a, const scene::Image& b) {
    if (a.rgba.size() != b.rgba.size() || a.rgba.empty()) {
        return 0;
    }
    double sum = 0;
    for (std::size_t i = 0; i < a.rgba.size(); ++i) {
        const double d = static_cast<double>(a.rgba[i]) - b.rgba[i];
        sum += d * d;
    }
    return std::sqrt(sum / static_cast<double>(a.rgba.size()));
}

scene::Image unpremultiplied(const scene::Image& image) {
    scene::Image out = image;
    out.smallerLevels.clear();
    for (std::size_t i = 0; i + 3 < out.rgba.size(); i += 4) {
        const unsigned alpha = out.rgba[i + 3];
        if (alpha == 0 || alpha == 255) {
            continue;
        }
        for (std::size_t k = 0; k < 3; ++k) {
            out.rgba[i + k] = static_cast<std::uint8_t>(std::min(255u, (out.rgba[i + k] * 255u + alpha / 2) / alpha));
        }
    }
    return out;
}

} // namespace ts2::render
