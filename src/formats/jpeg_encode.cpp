#include "formats/jpeg.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <vector>

namespace ts2::jpeg {

namespace {

constexpr std::array<int, 64> kZigzag{0,  1,  8,  16, 9,  2,  3,  10, 17, 24, 32, 25, 18, 11, 4,  5,  12, 19, 26, 33, 40, 48,
                                      41, 34, 27, 20, 13, 6,  7,  14, 21, 28, 35, 42, 49, 56, 57, 50, 43, 36, 29, 22, 15, 23,
                                      30, 37, 44, 51, 58, 59, 52, 45, 38, 31, 39, 46, 53, 60, 61, 54, 47, 55, 62, 63};

// The JPEG standard's example quantization tables (Annex K), in natural order.
constexpr std::array<int, 64> kLuminance{16, 11, 10, 16, 24,  40,  51,  61,  12, 12, 14, 19, 26,  58,  60,  55,
                                         14, 13, 16, 24, 40,  57,  69,  56,  14, 17, 22, 29, 51,  87,  80,  62,
                                         18, 22, 37, 56, 68,  109, 103, 77,  24, 35, 55, 64, 81,  104, 113, 92,
                                         49, 64, 78, 87, 103, 121, 120, 101, 72, 92, 95, 98, 112, 100, 103, 99};
constexpr std::array<int, 64> kChrominance{17, 18, 24, 47, 99, 99, 99, 99, 18, 21, 26, 66, 99, 99, 99, 99, 24, 26, 56, 99, 99, 99,
                                           99, 99, 47, 66, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99,
                                           99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99};

// The standard's example Huffman tables (Annex K): code counts per length, then the values.
struct HuffmanSpec {
    std::array<std::uint8_t, 16> counts;
    std::vector<std::uint8_t> values;
};
const HuffmanSpec kDcLuminance{{0, 1, 5, 1, 1, 1, 1, 1, 1, 0, 0, 0, 0, 0, 0, 0}, {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11}};
const HuffmanSpec kDcChrominance{{0, 3, 1, 1, 1, 1, 1, 1, 1, 1, 1, 0, 0, 0, 0, 0}, {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11}};
const HuffmanSpec kAcLuminance{
    {0, 2, 1, 3, 3, 2, 4, 3, 5, 5, 4, 4, 0, 0, 1, 0x7D},
    {0x01, 0x02, 0x03, 0x00, 0x04, 0x11, 0x05, 0x12, 0x21, 0x31, 0x41, 0x06, 0x13, 0x51, 0x61, 0x07, 0x22, 0x71, 0x14, 0x32, 0x81,
     0x91, 0xA1, 0x08, 0x23, 0x42, 0xB1, 0xC1, 0x15, 0x52, 0xD1, 0xF0, 0x24, 0x33, 0x62, 0x72, 0x82, 0x09, 0x0A, 0x16, 0x17, 0x18,
     0x19, 0x1A, 0x25, 0x26, 0x27, 0x28, 0x29, 0x2A, 0x34, 0x35, 0x36, 0x37, 0x38, 0x39, 0x3A, 0x43, 0x44, 0x45, 0x46, 0x47, 0x48,
     0x49, 0x4A, 0x53, 0x54, 0x55, 0x56, 0x57, 0x58, 0x59, 0x5A, 0x63, 0x64, 0x65, 0x66, 0x67, 0x68, 0x69, 0x6A, 0x73, 0x74, 0x75,
     0x76, 0x77, 0x78, 0x79, 0x7A, 0x83, 0x84, 0x85, 0x86, 0x87, 0x88, 0x89, 0x8A, 0x92, 0x93, 0x94, 0x95, 0x96, 0x97, 0x98, 0x99,
     0x9A, 0xA2, 0xA3, 0xA4, 0xA5, 0xA6, 0xA7, 0xA8, 0xA9, 0xAA, 0xB2, 0xB3, 0xB4, 0xB5, 0xB6, 0xB7, 0xB8, 0xB9, 0xBA, 0xC2, 0xC3,
     0xC4, 0xC5, 0xC6, 0xC7, 0xC8, 0xC9, 0xCA, 0xD2, 0xD3, 0xD4, 0xD5, 0xD6, 0xD7, 0xD8, 0xD9, 0xDA, 0xE1, 0xE2, 0xE3, 0xE4, 0xE5,
     0xE6, 0xE7, 0xE8, 0xE9, 0xEA, 0xF1, 0xF2, 0xF3, 0xF4, 0xF5, 0xF6, 0xF7, 0xF8, 0xF9, 0xFA}};
const HuffmanSpec kAcChrominance{
    {0, 2, 1, 2, 4, 4, 3, 4, 7, 5, 4, 4, 0, 1, 2, 0x77},
    {0x00, 0x01, 0x02, 0x03, 0x11, 0x04, 0x05, 0x21, 0x31, 0x06, 0x12, 0x41, 0x51, 0x07, 0x61, 0x71, 0x13, 0x22, 0x32, 0x81, 0x08,
     0x14, 0x42, 0x91, 0xA1, 0xB1, 0xC1, 0x09, 0x23, 0x33, 0x52, 0xF0, 0x15, 0x62, 0x72, 0xD1, 0x0A, 0x16, 0x24, 0x34, 0xE1, 0x25,
     0xF1, 0x17, 0x18, 0x19, 0x1A, 0x26, 0x27, 0x28, 0x29, 0x2A, 0x35, 0x36, 0x37, 0x38, 0x39, 0x3A, 0x43, 0x44, 0x45, 0x46, 0x47,
     0x48, 0x49, 0x4A, 0x53, 0x54, 0x55, 0x56, 0x57, 0x58, 0x59, 0x5A, 0x63, 0x64, 0x65, 0x66, 0x67, 0x68, 0x69, 0x6A, 0x73, 0x74,
     0x75, 0x76, 0x77, 0x78, 0x79, 0x7A, 0x82, 0x83, 0x84, 0x85, 0x86, 0x87, 0x88, 0x89, 0x8A, 0x92, 0x93, 0x94, 0x95, 0x96, 0x97,
     0x98, 0x99, 0x9A, 0xA2, 0xA3, 0xA4, 0xA5, 0xA6, 0xA7, 0xA8, 0xA9, 0xAA, 0xB2, 0xB3, 0xB4, 0xB5, 0xB6, 0xB7, 0xB8, 0xB9, 0xBA,
     0xC2, 0xC3, 0xC4, 0xC5, 0xC6, 0xC7, 0xC8, 0xC9, 0xCA, 0xD2, 0xD3, 0xD4, 0xD5, 0xD6, 0xD7, 0xD8, 0xD9, 0xDA, 0xE2, 0xE3, 0xE4,
     0xE5, 0xE6, 0xE7, 0xE8, 0xE9, 0xEA, 0xF2, 0xF3, 0xF4, 0xF5, 0xF6, 0xF7, 0xF8, 0xF9, 0xFA}};

struct Code {
    std::uint16_t bits = 0;
    std::uint8_t length = 0;
};

std::array<Code, 256> codesOf(const HuffmanSpec& spec) {
    std::array<Code, 256> codes{};
    std::uint16_t code = 0;
    std::size_t k = 0;
    for (int length = 1; length <= 16; ++length) {
        for (int i = 0; i < spec.counts[static_cast<std::size_t>(length - 1)]; ++i) {
            codes[spec.values[k++]] = {code, static_cast<std::uint8_t>(length)};
            ++code;
        }
        code = static_cast<std::uint16_t>(code << 1);
    }
    return codes;
}

// The tables the IJG library makes for a quality from 1 to 100.
std::array<int, 64> scaled(const std::array<int, 64>& base, int quality) {
    const int scale = quality < 50 ? 5000 / quality : 200 - quality * 2;
    std::array<int, 64> out{};
    for (std::size_t i = 0; i < 64; ++i) {
        out[i] = std::clamp((base[i] * scale + 50) / 100, 1, 255);
    }
    return out;
}

class BitWriter {
public:
    explicit BitWriter(std::vector<std::uint8_t>& out) : out_(out) {}
    void put(std::uint32_t value, int length) {
        for (int i = length - 1; i >= 0; --i) {
            buffer_ = static_cast<std::uint8_t>(buffer_ << 1 | ((value >> i) & 1));
            if (++count_ == 8) {
                emit();
            }
        }
    }
    // Pads the last byte with one bits, as the standard asks.
    void finish() {
        while (count_ != 0) {
            put(1, 1);
        }
    }

private:
    void emit() {
        out_.push_back(buffer_);
        if (buffer_ == 0xFF) {
            out_.push_back(0);
        }
        buffer_ = 0;
        count_ = 0;
    }
    std::vector<std::uint8_t>& out_;
    std::uint8_t buffer_ = 0;
    int count_ = 0;
};

int sizeOf(int v) {
    int s = 0;
    for (int a = std::abs(v); a != 0; a >>= 1) {
        ++s;
    }
    return s;
}

// The forward DCT of one block of samples less 128, as the standard defines it.
std::array<double, 64> forwardDct(const std::array<double, 64>& in) {
    static const auto cosines = [] {
        std::array<double, 64> c{};
        for (int x = 0; x < 8; ++x) {
            for (int u = 0; u < 8; ++u) {
                c[static_cast<std::size_t>(x * 8 + u)] = std::cos((2 * x + 1) * u * 3.14159265358979323846 / 16);
            }
        }
        return c;
    }();
    std::array<double, 64> rows{};
    for (int y = 0; y < 8; ++y) {
        for (int u = 0; u < 8; ++u) {
            double sum = 0;
            for (int x = 0; x < 8; ++x) {
                sum += in[static_cast<std::size_t>(y * 8 + x)] * cosines[static_cast<std::size_t>(x * 8 + u)];
            }
            rows[static_cast<std::size_t>(y * 8 + u)] = sum * (u == 0 ? std::sqrt(0.5) : 1.0) / 2;
        }
    }
    std::array<double, 64> out{};
    for (int u = 0; u < 8; ++u) {
        for (int v = 0; v < 8; ++v) {
            double sum = 0;
            for (int y = 0; y < 8; ++y) {
                sum += rows[static_cast<std::size_t>(y * 8 + u)] * cosines[static_cast<std::size_t>(y * 8 + v)];
            }
            out[static_cast<std::size_t>(v * 8 + u)] = sum * (v == 0 ? std::sqrt(0.5) : 1.0) / 2;
        }
    }
    return out;
}

struct Table {
    std::array<int, 64> quant;
    std::array<Code, 256> dc;
    std::array<Code, 256> ac;
};

void encodeBlock(BitWriter& bits, const std::array<double, 64>& samples, const Table& table, int& predictor) {
    const std::array<double, 64> coef = forwardDct(samples);
    std::array<int, 64> q{};
    for (std::size_t i = 0; i < 64; ++i) {
        q[i] = static_cast<int>(std::lround(coef[i] / table.quant[i]));
    }
    const int diff = q[0] - predictor;
    predictor = q[0];
    const int dcSize = sizeOf(diff);
    bits.put(table.dc[static_cast<std::size_t>(dcSize)].bits, table.dc[static_cast<std::size_t>(dcSize)].length);
    if (dcSize != 0) {
        bits.put(static_cast<std::uint32_t>(diff < 0 ? diff + (1 << dcSize) - 1 : diff), dcSize);
    }
    int run = 0;
    for (std::size_t k = 1; k < 64; ++k) {
        const int v = q[static_cast<std::size_t>(kZigzag[k])];
        if (v == 0) {
            ++run;
            continue;
        }
        for (; run > 15; run -= 16) {
            bits.put(table.ac[0xF0].bits, table.ac[0xF0].length);
        }
        const int s = sizeOf(v);
        const auto symbol = static_cast<std::size_t>(run << 4 | s);
        bits.put(table.ac[symbol].bits, table.ac[symbol].length);
        bits.put(static_cast<std::uint32_t>(v < 0 ? v + (1 << s) - 1 : v), s);
        run = 0;
    }
    if (run != 0) {
        bits.put(table.ac[0].bits, table.ac[0].length);
    }
}

void putSegment(std::vector<std::uint8_t>& out, std::uint8_t marker, std::span<const std::uint8_t> body) {
    const std::size_t length = body.size() + 2;
    out.insert(out.end(), {0xFF, marker, static_cast<std::uint8_t>(length >> 8), static_cast<std::uint8_t>(length)});
    out.insert(out.end(), body.begin(), body.end());
}

void putQuant(std::vector<std::uint8_t>& out, std::uint8_t id, const std::array<int, 64>& quant) {
    std::vector<std::uint8_t> body{id};
    for (std::size_t k = 0; k < 64; ++k) {
        body.push_back(static_cast<std::uint8_t>(quant[static_cast<std::size_t>(kZigzag[k])]));
    }
    putSegment(out, 0xDB, body);
}

void putHuffman(std::vector<std::uint8_t>& out, std::uint8_t classAndId, const HuffmanSpec& spec) {
    std::vector<std::uint8_t> body{classAndId};
    body.insert(body.end(), spec.counts.begin(), spec.counts.end());
    body.insert(body.end(), spec.values.begin(), spec.values.end());
    putSegment(out, 0xC4, body);
}

} // namespace

std::vector<std::uint8_t> encode(const tga::Pixels& image, int quality, std::span<const std::uint8_t> extraApp0) {
    const int width = static_cast<int>(image.width);
    const int height = static_cast<int>(image.height);
    if (width <= 0 || height <= 0 || width > 0xFFFF || height > 0xFFFF || image.rgba.size() < static_cast<std::size_t>(image.width) * image.height * 4 ||
        extraApp0.size() > 0xFFFD || quality < 1 || quality > 100) {
        return {};
    }
    const Table luma{scaled(kLuminance, quality), codesOf(kDcLuminance), codesOf(kAcLuminance)};
    const Table chroma{scaled(kChrominance, quality), codesOf(kDcChrominance), codesOf(kAcChrominance)};

    std::vector<std::uint8_t> out{0xFF, 0xD8};
    // JFIF 1.1, no density units, 1 by 1, no preview, as the game's pictures carry.
    const std::uint8_t jfif[] = {'J', 'F', 'I', 'F', 0, 1, 1, 0, 0, 1, 0, 1, 0, 0};
    putSegment(out, 0xE0, jfif);
    if (!extraApp0.empty()) {
        putSegment(out, 0xE0, extraApp0);
    }
    putQuant(out, 0, luma.quant);
    putQuant(out, 1, chroma.quant);
    // Luma sampled at full size, the two chroma planes at half size each way.
    const std::uint8_t frame[] = {8,    static_cast<std::uint8_t>(height >> 8), static_cast<std::uint8_t>(height), static_cast<std::uint8_t>(width >> 8),
                                  static_cast<std::uint8_t>(width), 3, 1, 0x22, 0, 2, 0x11, 1, 3, 0x11, 1};
    putSegment(out, 0xC0, frame);
    putHuffman(out, 0x00, kDcLuminance);
    putHuffman(out, 0x10, kAcLuminance);
    putHuffman(out, 0x01, kDcChrominance);
    putHuffman(out, 0x11, kAcChrominance);
    const std::uint8_t scan[] = {3, 1, 0x00, 2, 0x11, 3, 0x11, 0, 63, 0};
    putSegment(out, 0xDA, scan);

    // A pixel outside the picture repeats the nearest edge one.
    const auto ycc = [&](int x, int y, int channel) {
        x = std::min(x, width - 1);
        y = std::min(y, height - 1);
        const std::uint8_t* p = &image.rgba[(static_cast<std::size_t>(y) * image.width + static_cast<std::size_t>(x)) * 4];
        const double r = p[0], g = p[1], b = p[2];
        if (channel == 0) {
            return 0.299 * r + 0.587 * g + 0.114 * b;
        }
        if (channel == 1) {
            return -0.168736 * r - 0.331264 * g + 0.5 * b + 128;
        }
        return 0.5 * r - 0.418688 * g - 0.081312 * b + 128;
    };
    BitWriter bits(out);
    int predictors[3] = {0, 0, 0};
    std::array<double, 64> block{};
    for (int my = 0; my < height; my += 16) {
        for (int mx = 0; mx < width; mx += 16) {
            for (int by = 0; by < 2; ++by) {
                for (int bx = 0; bx < 2; ++bx) {
                    for (int i = 0; i < 64; ++i) {
                        block[static_cast<std::size_t>(i)] = ycc(mx + bx * 8 + i % 8, my + by * 8 + i / 8, 0) - 128;
                    }
                    encodeBlock(bits, block, luma, predictors[0]);
                }
            }
            // Each chroma sample is the mean of the 2 by 2 pixels it covers.
            for (int channel = 1; channel <= 2; ++channel) {
                for (int i = 0; i < 64; ++i) {
                    const int x = mx + (i % 8) * 2, y = my + (i / 8) * 2;
                    block[static_cast<std::size_t>(i)] =
                        (ycc(x, y, channel) + ycc(x + 1, y, channel) + ycc(x, y + 1, channel) + ycc(x + 1, y + 1, channel)) / 4 - 128;
                }
                encodeBlock(bits, block, chroma, predictors[channel]);
            }
        }
    }
    bits.finish();
    out.insert(out.end(), {0xFF, 0xD9});
    return out;
}

} // namespace ts2::jpeg
