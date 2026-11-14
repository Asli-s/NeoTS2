// A baseline JPEG encoder: what the game's snapshots are saved as (Storytelling/snapshot_*.jpg). JFIF,
// YCbCr with no subsampling, the standard (Annex K) quantization and Huffman tables, one scan.
#include "formats/jpeg.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <numbers>

namespace ts2::jpeg {

namespace {

// Where each coefficient of the zigzag order sits in the 8 x 8 block.
constexpr std::array<int, 64> kZigzag = {0,  1,  8,  16, 9,  2,  3,  10, 17, 24, 32, 25, 18, 11, 4,  5,
                                         12, 19, 26, 33, 40, 48, 41, 34, 27, 20, 13, 6,  7,  14, 21, 28,
                                         35, 42, 49, 56, 57, 50, 43, 36, 29, 22, 15, 23, 30, 37, 44, 51,
                                         58, 59, 52, 45, 38, 31, 39, 46, 53, 60, 61, 54, 47, 55, 62, 63};

// The standard quantization tables at quality 50, in the block's own order.
constexpr std::array<int, 64> kLumaQuant = {16, 11, 10, 16, 24,  40,  51,  61,  12, 12, 14, 19, 26,  58,  60,  55,
                                            14, 13, 16, 24, 40,  57,  69,  56,  14, 17, 22, 29, 51,  87,  80,  62,
                                            18, 22, 37, 56, 68,  109, 103, 77,  24, 35, 55, 64, 81,  104, 113, 92,
                                            49, 64, 78, 87, 103, 121, 120, 101, 72, 92, 95, 98, 112, 100, 103, 99};
constexpr std::array<int, 64> kChromaQuant = {17, 18, 24, 47, 99, 99, 99, 99, 18, 21, 26, 66, 99, 99, 99, 99,
                                              24, 26, 56, 99, 99, 99, 99, 99, 47, 66, 99, 99, 99, 99, 99, 99,
                                              99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99,
                                              99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99};

// The standard Huffman tables: how many codes of each length (1 to 16), then the symbols in order.
struct HuffmanSpec {
    std::array<std::uint8_t, 16> counts;
    std::vector<std::uint8_t> symbols;
};
const HuffmanSpec kLumaDc = {{0, 1, 5, 1, 1, 1, 1, 1, 1, 0, 0, 0, 0, 0, 0, 0}, {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11}};
const HuffmanSpec kChromaDc = {{0, 3, 1, 1, 1, 1, 1, 1, 1, 1, 1, 0, 0, 0, 0, 0}, {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11}};
const HuffmanSpec kLumaAc = {
    {0, 2, 1, 3, 3, 2, 4, 3, 5, 5, 4, 4, 0, 0, 1, 0x7d},
    {0x01, 0x02, 0x03, 0x00, 0x04, 0x11, 0x05, 0x12, 0x21, 0x31, 0x41, 0x06, 0x13, 0x51, 0x61, 0x07, 0x22, 0x71, 0x14, 0x32, 0x81,
     0x91, 0xa1, 0x08, 0x23, 0x42, 0xb1, 0xc1, 0x15, 0x52, 0xd1, 0xf0, 0x24, 0x33, 0x62, 0x72, 0x82, 0x09, 0x0a, 0x16, 0x17, 0x18,
     0x19, 0x1a, 0x25, 0x26, 0x27, 0x28, 0x29, 0x2a, 0x34, 0x35, 0x36, 0x37, 0x38, 0x39, 0x3a, 0x43, 0x44, 0x45, 0x46, 0x47, 0x48,
     0x49, 0x4a, 0x53, 0x54, 0x55, 0x56, 0x57, 0x58, 0x59, 0x5a, 0x63, 0x64, 0x65, 0x66, 0x67, 0x68, 0x69, 0x6a, 0x73, 0x74, 0x75,
     0x76, 0x77, 0x78, 0x79, 0x7a, 0x83, 0x84, 0x85, 0x86, 0x87, 0x88, 0x89, 0x8a, 0x92, 0x93, 0x94, 0x95, 0x96, 0x97, 0x98, 0x99,
     0x9a, 0xa2, 0xa3, 0xa4, 0xa5, 0xa6, 0xa7, 0xa8, 0xa9, 0xaa, 0xb2, 0xb3, 0xb4, 0xb5, 0xb6, 0xb7, 0xb8, 0xb9, 0xba, 0xc2, 0xc3,
     0xc4, 0xc5, 0xc6, 0xc7, 0xc8, 0xc9, 0xca, 0xd2, 0xd3, 0xd4, 0xd5, 0xd6, 0xd7, 0xd8, 0xd9, 0xda, 0xe1, 0xe2, 0xe3, 0xe4, 0xe5,
     0xe6, 0xe7, 0xe8, 0xe9, 0xea, 0xf1, 0xf2, 0xf3, 0xf4, 0xf5, 0xf6, 0xf7, 0xf8, 0xf9, 0xfa}};
const HuffmanSpec kChromaAc = {
    {0, 2, 1, 2, 4, 4, 3, 4, 7, 5, 4, 4, 0, 1, 2, 0x77},
    {0x00, 0x01, 0x02, 0x03, 0x11, 0x04, 0x05, 0x21, 0x31, 0x06, 0x12, 0x41, 0x51, 0x07, 0x61, 0x71, 0x13, 0x22, 0x32, 0x81, 0x08,
     0x14, 0x42, 0x91, 0xa1, 0xb1, 0xc1, 0x09, 0x23, 0x33, 0x52, 0xf0, 0x15, 0x62, 0x72, 0xd1, 0x0a, 0x16, 0x24, 0x34, 0xe1, 0x25,
     0xf1, 0x17, 0x18, 0x19, 0x1a, 0x26, 0x27, 0x28, 0x29, 0x2a, 0x35, 0x36, 0x37, 0x38, 0x39, 0x3a, 0x43, 0x44, 0x45, 0x46, 0x47,
     0x48, 0x49, 0x4a, 0x53, 0x54, 0x55, 0x56, 0x57, 0x58, 0x59, 0x5a, 0x63, 0x64, 0x65, 0x66, 0x67, 0x68, 0x69, 0x6a, 0x73, 0x74,
     0x75, 0x76, 0x77, 0x78, 0x79, 0x7a, 0x82, 0x83, 0x84, 0x85, 0x86, 0x87, 0x88, 0x89, 0x8a, 0x92, 0x93, 0x94, 0x95, 0x96, 0x97,
     0x98, 0x99, 0x9a, 0xa2, 0xa3, 0xa4, 0xa5, 0xa6, 0xa7, 0xa8, 0xa9, 0xaa, 0xb2, 0xb3, 0xb4, 0xb5, 0xb6, 0xb7, 0xb8, 0xb9, 0xba,
     0xc2, 0xc3, 0xc4, 0xc5, 0xc6, 0xc7, 0xc8, 0xc9, 0xca, 0xd2, 0xd3, 0xd4, 0xd5, 0xd6, 0xd7, 0xd8, 0xd9, 0xda, 0xe2, 0xe3, 0xe4,
     0xe5, 0xe6, 0xe7, 0xe8, 0xe9, 0xea, 0xf2, 0xf3, 0xf4, 0xf5, 0xf6, 0xf7, 0xf8, 0xf9, 0xfa}};

// Each symbol's code and its length in bits, from a table's spec (canonical codes, as the decoder
// builds them).
struct Codes {
    std::array<std::uint16_t, 256> code{};
    std::array<std::uint8_t, 256> length{};
};
Codes codesOf(const HuffmanSpec& spec) {
    Codes out;
    std::uint16_t code = 0;
    std::size_t k = 0;
    for (int len = 1; len <= 16; ++len) {
        for (int i = 0; i < spec.counts[static_cast<std::size_t>(len - 1)]; ++i) {
            const std::uint8_t symbol = spec.symbols[k++];
            out.code[symbol] = code++;
            out.length[symbol] = static_cast<std::uint8_t>(len);
        }
        code = static_cast<std::uint16_t>(code << 1);
    }
    return out;
}

// A quantization table at `quality` (1 to 100), as the IJG library scales the standard ones.
std::array<int, 64> scaledQuant(const std::array<int, 64>& base, int quality) {
    quality = std::clamp(quality, 1, 100);
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
    void put(std::uint32_t bits, int count) {
        for (int i = count - 1; i >= 0; --i) {
            acc_ = static_cast<std::uint8_t>(acc_ << 1 | ((bits >> i) & 1));
            if (++filled_ == 8) {
                emit();
            }
        }
    }
    // The last byte padded with ones, as the standard asks.
    void flush() {
        while (filled_ != 0) {
            put(1, 1);
        }
    }

private:
    void emit() {
        out_.push_back(acc_);
        if (acc_ == 0xFF) {
            out_.push_back(0);  // a data byte 0xFF is followed by 0, so it isn't read as a marker
        }
        acc_ = 0;
        filled_ = 0;
    }
    std::vector<std::uint8_t>& out_;
    std::uint8_t acc_ = 0;
    int filled_ = 0;
};

// How many bits a coefficient needs, and those bits (a negative one as one less than its value in
// that many bits, as the standard stores them).
int magnitude(int v) {
    int n = 0;
    for (int a = std::abs(v); a != 0; a >>= 1) {
        ++n;
    }
    return n;
}
std::uint32_t magnitudeBits(int v, int n) {
    return static_cast<std::uint32_t>(v >= 0 ? v : v + (1 << n) - 1);
}

void encodeBlock(BitWriter& bits, const std::array<float, 64>& block, const std::array<float, 64>& divisors, int& previousDc,
                 const Codes& dc, const Codes& ac) {
    // The forward DCT, row by row and then column by column.
    static const std::array<float, 64> kCos = [] {
        std::array<float, 64> c{};
        for (int u = 0; u < 8; ++u) {
            for (int x = 0; x < 8; ++x) {
                const double cu = u == 0 ? std::sqrt(0.125) : 0.5;
                c[static_cast<std::size_t>(u * 8 + x)] = static_cast<float>(cu * std::cos((2 * x + 1) * u * std::numbers::pi / 16));
            }
        }
        return c;
    }();
    std::array<float, 64> rows{}, coef{};
    for (int y = 0; y < 8; ++y) {
        for (int u = 0; u < 8; ++u) {
            float s = 0;
            for (int x = 0; x < 8; ++x) {
                s += block[static_cast<std::size_t>(y * 8 + x)] * kCos[static_cast<std::size_t>(u * 8 + x)];
            }
            rows[static_cast<std::size_t>(y * 8 + u)] = s;
        }
    }
    for (int u = 0; u < 8; ++u) {
        for (int v = 0; v < 8; ++v) {
            float s = 0;
            for (int y = 0; y < 8; ++y) {
                s += rows[static_cast<std::size_t>(y * 8 + u)] * kCos[static_cast<std::size_t>(v * 8 + y)];
            }
            coef[static_cast<std::size_t>(v * 8 + u)] = s;
        }
    }
    std::array<int, 64> q{};
    for (std::size_t k = 0; k < 64; ++k) {
        const auto at = static_cast<std::size_t>(kZigzag[k]);
        q[k] = static_cast<int>(std::lround(coef[at] / divisors[at]));
    }
    // The DC as the change from the last block's, then the AC as runs of zeros and values.
    const int diff = q[0] - previousDc;
    previousDc = q[0];
    const int dn = magnitude(diff);
    bits.put(dc.code[static_cast<std::size_t>(dn)], dc.length[static_cast<std::size_t>(dn)]);
    bits.put(magnitudeBits(diff, dn), dn);
    int zeros = 0;
    for (std::size_t k = 1; k < 64; ++k) {
        if (q[k] == 0) {
            ++zeros;
            continue;
        }
        while (zeros >= 16) {
            bits.put(ac.code[0xF0], ac.length[0xF0]);  // sixteen zeros
            zeros -= 16;
        }
        const int n = magnitude(q[k]);
        const auto symbol = static_cast<std::size_t>(zeros << 4 | n);
        bits.put(ac.code[symbol], ac.length[symbol]);
        bits.put(magnitudeBits(q[k], n), n);
        zeros = 0;
    }
    if (zeros > 0) {
        bits.put(ac.code[0x00], ac.length[0x00]);  // the rest are zeros
    }
}

void u16(std::vector<std::uint8_t>& out, int v) {
    out.push_back(static_cast<std::uint8_t>(v >> 8));
    out.push_back(static_cast<std::uint8_t>(v));
}
void marker(std::vector<std::uint8_t>& out, std::uint8_t m) {
    out.push_back(0xFF);
    out.push_back(m);
}
void huffmanTable(std::vector<std::uint8_t>& out, std::uint8_t id, const HuffmanSpec& spec) {
    marker(out, 0xC4);
    u16(out, static_cast<int>(2 + 1 + 16 + spec.symbols.size()));
    out.push_back(id);
    out.insert(out.end(), spec.counts.begin(), spec.counts.end());
    out.insert(out.end(), spec.symbols.begin(), spec.symbols.end());
}

} // namespace

std::vector<std::uint8_t> encode(const tga::Pixels& picture, int quality) {
    const std::uint32_t w = picture.width, h = picture.height;
    if (w == 0 || h == 0 || w > 65535 || h > 65535 || picture.rgba.size() != static_cast<std::size_t>(w) * h * 4) {
        return {};
    }
    const auto luma = scaledQuant(kLumaQuant, quality), chroma = scaledQuant(kChromaQuant, quality);
    std::vector<std::uint8_t> out;
    marker(out, 0xD8);  // start of image
    // JFIF 1.1, square pixels.
    marker(out, 0xE0);
    u16(out, 16);
    for (const char c : {'J', 'F', 'I', 'F', '\0'}) {
        out.push_back(static_cast<std::uint8_t>(c));
    }
    out.insert(out.end(), {1, 1, 0, 0, 1, 0, 1, 0, 0});
    // The quantization tables, in zigzag order.
    for (const auto& [id, table] : {std::pair{0, &luma}, std::pair{1, &chroma}}) {
        marker(out, 0xDB);
        u16(out, 2 + 1 + 64);
        out.push_back(static_cast<std::uint8_t>(id));
        for (std::size_t k = 0; k < 64; ++k) {
            out.push_back(static_cast<std::uint8_t>((*table)[static_cast<std::size_t>(kZigzag[k])]));
        }
    }
    // Baseline frame: three components, none subsampled; Y uses table 0, Cb and Cr table 1.
    marker(out, 0xC0);
    u16(out, 8 + 3 * 3);
    out.push_back(8);
    u16(out, static_cast<int>(h));
    u16(out, static_cast<int>(w));
    out.push_back(3);
    out.insert(out.end(), {1, 0x11, 0, 2, 0x11, 1, 3, 0x11, 1});
    huffmanTable(out, 0x00, kLumaDc);
    huffmanTable(out, 0x10, kLumaAc);
    huffmanTable(out, 0x01, kChromaDc);
    huffmanTable(out, 0x11, kChromaAc);
    marker(out, 0xDA);
    u16(out, 6 + 2 * 3);
    out.push_back(3);
    out.insert(out.end(), {1, 0x00, 2, 0x11, 3, 0x11, 0, 63, 0});

    // A coefficient is divided by its table's entry (the DCT above is scaled as the standard's).
    std::array<float, 64> lumaDiv{}, chromaDiv{};
    for (std::size_t i = 0; i < 64; ++i) {
        lumaDiv[i] = static_cast<float>(luma[i]);
        chromaDiv[i] = static_cast<float>(chroma[i]);
    }
    const Codes lumaDc = codesOf(kLumaDc), lumaAc = codesOf(kLumaAc), chromaDc = codesOf(kChromaDc), chromaAc = codesOf(kChromaAc);
    BitWriter bits(out);
    int dcY = 0, dcCb = 0, dcCr = 0;
    std::array<float, 64> y{}, cb{}, cr{};
    for (std::uint32_t by = 0; by < h; by += 8) {
        for (std::uint32_t bx = 0; bx < w; bx += 8) {
            // A block past the picture's edge repeats its last row and column.
            for (std::uint32_t j = 0; j < 8; ++j) {
                for (std::uint32_t i = 0; i < 8; ++i) {
                    const std::uint32_t px = std::min(bx + i, w - 1), py = std::min(by + j, h - 1);
                    const std::uint8_t* p = &picture.rgba[(static_cast<std::size_t>(py) * w + px) * 4];
                    const float r = p[0], g = p[1], b = p[2];
                    const std::size_t at = j * 8 + i;
                    y[at] = 0.299f * r + 0.587f * g + 0.114f * b - 128.0f;
                    cb[at] = -0.168736f * r - 0.331264f * g + 0.5f * b;
                    cr[at] = 0.5f * r - 0.418688f * g - 0.081312f * b;
                }
            }
            encodeBlock(bits, y, lumaDiv, dcY, lumaDc, lumaAc);
            encodeBlock(bits, cb, chromaDiv, dcCb, chromaDc, chromaAc);
            encodeBlock(bits, cr, chromaDiv, dcCr, chromaDc, chromaAc);
        }
    }
    bits.flush();
    marker(out, 0xD9);  // end of image
    return out;
}

} // namespace ts2::jpeg
