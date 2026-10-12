#include "formats/jpeg.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <vector>

namespace ts2::jpeg {

namespace {

// Pictures larger than this many pixels are refused, so broken sizes can't ask for huge memory.
constexpr std::uint64_t kMaxPixels = 1u << 26;

// Coefficient positions in the order they are stored, with spare entries so a broken block that
// runs past the last coefficient lands on it harmlessly.
constexpr std::array<int, 80> kNaturalOrder{0,  1,  8,  16, 9,  2,  3,  10, 17, 24, 32, 25, 18, 11, 4,  5,  12, 19, 26, 33,
                                            40, 48, 41, 34, 27, 20, 13, 6,  7,  14, 21, 28, 35, 42, 49, 56, 57, 50, 43, 36,
                                            29, 22, 15, 23, 30, 37, 44, 51, 58, 59, 52, 45, 38, 31, 39, 46, 53, 60, 61, 54,
                                            47, 55, 62, 63, 63, 63, 63, 63, 63, 63, 63, 63, 63, 63, 63, 63, 63, 63, 63, 63};

struct Huffman {
    bool defined = false;
    std::array<int, 18> maxCode{};
    std::array<int, 17> valueAt{};
    std::array<int, 17> minCode{};
    std::vector<std::uint8_t> values;
};

struct Component {
    int id = 0, h = 1, v = 1, quant = 0, dc = 0, ac = 0;
    int width = 0, height = 0;  // in samples, as the picture needs them
    int stride = 0, rows = 0;   // the sample plane, whole blocks
    std::vector<std::uint8_t> samples;
    int predictor = 0;
};

// Reads entropy-coded bits; a marker or the end of the data reads as zeros, as libjpeg does.
class Bits {
public:
    explicit Bits(std::span<const std::uint8_t> data, std::size_t at) : data_(data), at_(at) {}

    int bit() {
        if (count_ == 0) {
            fill();
        }
        --count_;
        return (buffer_ >> count_) & 1;
    }
    int bits(int n) {
        int v = 0;
        for (int i = 0; i < n; ++i) {
            v = (v << 1) | bit();
        }
        return v;
    }
    // At a restart: drops the bits left and steps over the RSTn marker.
    void restart() {
        count_ = 0;
        marker_ = false;
        while (at_ + 1 < data_.size() && !(data_[at_] == 0xFF && data_[at_ + 1] >= 0xD0 && data_[at_ + 1] <= 0xD7)) {
            ++at_;
        }
        if (at_ + 1 < data_.size()) {
            at_ += 2;
        }
    }

private:
    void fill() {
        buffer_ = 0;
        count_ = 8;
        if (marker_ || at_ >= data_.size()) {
            return;
        }
        const std::uint8_t b = data_[at_];
        if (b == 0xFF) {
            if (at_ + 1 < data_.size() && data_[at_ + 1] == 0x00) {
                at_ += 2;
                buffer_ = 0xFF;
            } else {
                marker_ = true;
            }
            return;
        }
        ++at_;
        buffer_ = b;
    }

    std::span<const std::uint8_t> data_;
    std::size_t at_;
    int buffer_ = 0, count_ = 0;
    bool marker_ = false;
};

bool build(Huffman& t, const std::array<std::uint8_t, 16>& counts, std::vector<std::uint8_t> values) {
    int code = 0, k = 0;
    for (int length = 1; length <= 16; ++length) {
        const int n = counts[static_cast<std::size_t>(length - 1)];
        t.valueAt[static_cast<std::size_t>(length)] = k;
        t.minCode[static_cast<std::size_t>(length)] = code;
        code += n;
        k += n;
        if (code > (1 << length)) {
            return false;  // more codes than the length can hold
        }
        t.maxCode[static_cast<std::size_t>(length)] = n ? code - 1 : -1;
        code <<= 1;
    }
    t.maxCode[17] = 0x7FFFFFFF;
    t.values = std::move(values);
    t.defined = true;
    return true;
}

int decodeSymbol(Bits& in, const Huffman& t) {
    int code = in.bit();
    int length = 1;
    while (length <= 16 && code > t.maxCode[static_cast<std::size_t>(length)]) {
        code = (code << 1) | in.bit();
        ++length;
    }
    if (length > 16) {
        return 0;  // a bad code reads as 0, as libjpeg does
    }
    const auto at = static_cast<std::size_t>(t.valueAt[static_cast<std::size_t>(length)] + code - t.minCode[static_cast<std::size_t>(length)]);
    return at < t.values.size() ? t.values[at] : 0;
}

int extend(int v, int s) {
    return s > 0 && v < (1 << (s - 1)) ? v - (1 << s) + 1 : v;
}

// 16-bit lane arithmetic as the decoder's MMX code does it: saturating adds and subtracts, the
// high half of a product, and arithmetic shifts.
std::int16_t sat(std::int32_t v) {
    return static_cast<std::int16_t>(std::clamp<std::int32_t>(v, -32768, 32767));
}
std::int16_t adds(std::int16_t a, std::int16_t b) {
    return sat(static_cast<std::int32_t>(a) + b);
}
std::int16_t subs(std::int16_t a, std::int16_t b) {
    return sat(static_cast<std::int32_t>(a) - b);
}
std::int16_t mulhi(std::int16_t a, std::int16_t b) {
    return static_cast<std::int16_t>((static_cast<std::int32_t>(a) * b) >> 16);
}
std::uint8_t toByte(std::int16_t v) {
    return static_cast<std::uint8_t>(std::clamp<int>(adds(static_cast<std::int16_t>(v >> 5), 128), 0, 255));
}

// The row pass's constants: four tables of eight pairs, rows 0/4, 1/7, 2/6 and 3/5.
constexpr std::int16_t kRowTables[4][8][4] = {
    {{16384, 21407, 16384, 8867}, {16384, 8867, -16384, -21407}, {16384, -8867, 16384, -21407}, {-16384, 21407, 16384, -8867},
     {22725, 19266, 19266, -4520}, {12873, 4520, -22725, -12873}, {12873, -22725, 4520, -12873}, {4520, 19266, 19266, -22725}},
    {{22725, 29692, 22725, 12299}, {22725, 12299, -22725, -29692}, {22725, -12299, 22725, -29692}, {-22725, 29692, 22725, -12299},
     {31521, 26722, 26722, -6270}, {17855, 6270, -31521, -17855}, {17855, -31521, 6270, -17855}, {6270, 26722, 26722, -31521}},
    {{21407, 27969, 21407, 11585}, {21407, 11585, -21407, -27969}, {21407, -11585, 21407, -27969}, {-21407, 27969, 21407, -11585},
     {29692, 25172, 25172, -5906}, {16819, 5906, -29692, -16819}, {16819, -29692, 5906, -16819}, {5906, 25172, 25172, -29692}},
    {{19266, 25172, 19266, 10426}, {19266, 10426, -19266, -25172}, {19266, -10426, 19266, -25172}, {-19266, 25172, 19266, -10426},
     {26722, 22654, 22654, -5315}, {15137, 5315, -26722, -15137}, {15137, -26722, 5315, -15137}, {5315, 22654, 22654, -26722}}};
constexpr int kRowTableOf[8] = {0, 1, 2, 3, 0, 3, 2, 1};

// Two products summed into 32 bits, wrapping as the processor's multiply-add does.
std::int32_t madd(std::int16_t a, std::int16_t b, std::int16_t c, std::int16_t d) {
    return static_cast<std::int32_t>(static_cast<std::uint32_t>(static_cast<std::int32_t>(a) * b) +
                                     static_cast<std::uint32_t>(static_cast<std::int32_t>(c) * d));
}
std::int32_t wrapAdd(std::int32_t a, std::int32_t b) {
    return static_cast<std::int32_t>(static_cast<std::uint32_t>(a) + static_cast<std::uint32_t>(b));
}
std::int32_t wrapSub(std::int32_t a, std::int32_t b) {
    return static_cast<std::int32_t>(static_cast<std::uint32_t>(a) - static_cast<std::uint32_t>(b));
}

// The game's JPEG decoder's inverse DCT (Intel's AP-922 integer transform): coefficients times
// their quantizers kept to 16 bits, a row pass in 32 bits rounded off 12 bits, a column pass in
// saturating 16-bit steps rounded off 5 bits, then +128 and clamped.
void inverseDct(const std::array<std::int16_t, 64>& coef, const std::array<std::uint16_t, 64>& quant, std::uint8_t* out, int stride) {
    std::array<std::int16_t, 64> in{}, rows{};
    for (std::size_t i = 0; i < 64; ++i) {
        in[i] = static_cast<std::int16_t>(static_cast<std::uint32_t>(static_cast<std::uint16_t>(coef[i])) * quant[i]);
    }
    constexpr std::int32_t kRound = 2048;
    for (int r = 0; r < 8; ++r) {
        const auto& t = kRowTables[kRowTableOf[r]];
        const std::int16_t* x = &in[static_cast<std::size_t>(r * 8)];
        const std::int32_t e0 = wrapAdd(wrapAdd(madd(t[0][0], x[0], t[0][1], x[2]), kRound), madd(t[1][0], x[4], t[1][1], x[6]));
        const std::int32_t e1 = wrapAdd(wrapAdd(madd(t[0][2], x[0], t[0][3], x[2]), kRound), madd(t[1][2], x[4], t[1][3], x[6]));
        const std::int32_t e2 = wrapAdd(wrapAdd(madd(t[2][0], x[0], t[2][1], x[2]), kRound), madd(t[3][0], x[4], t[3][1], x[6]));
        const std::int32_t e3 = wrapAdd(wrapAdd(madd(t[2][2], x[0], t[2][3], x[2]), kRound), madd(t[3][2], x[4], t[3][3], x[6]));
        const std::int32_t o0 = wrapAdd(madd(t[4][0], x[1], t[4][1], x[3]), madd(t[5][0], x[5], t[5][1], x[7]));
        const std::int32_t o1 = wrapAdd(madd(t[4][2], x[1], t[4][3], x[3]), madd(t[5][2], x[5], t[5][3], x[7]));
        const std::int32_t o2 = wrapAdd(madd(t[6][0], x[1], t[6][1], x[3]), madd(t[7][0], x[5], t[7][1], x[7]));
        const std::int32_t o3 = wrapAdd(madd(t[6][2], x[1], t[6][3], x[3]), madd(t[7][2], x[5], t[7][3], x[7]));
        std::int16_t* y = &rows[static_cast<std::size_t>(r * 8)];
        y[0] = sat(wrapAdd(e0, o0) >> 12);
        y[1] = sat(wrapAdd(e1, o1) >> 12);
        y[2] = sat(wrapAdd(e2, o2) >> 12);
        y[3] = sat(wrapAdd(e3, o3) >> 12);
        y[4] = sat(wrapSub(e3, o3) >> 12);
        y[5] = sat(wrapSub(e2, o2) >> 12);
        y[6] = sat(wrapSub(e1, o1) >> 12);
        y[7] = sat(wrapSub(e0, o0) >> 12);
    }
    constexpr std::int16_t tg1 = 13036, tg2 = 27146, tg3 = -21746, cos4 = -19195, one = 1, r16 = 16, r15 = 15;
    for (int c = 0; c < 8; ++c) {
        auto x = [&](int r) { return rows[static_cast<std::size_t>(r * 8 + c)]; };
        std::int16_t m0 = adds(adds(mulhi(x(5), tg3), x(5)), x(3));
        const std::int16_t m1 = adds(mulhi(tg3, x(3)), x(3));
        std::int16_t m4 = adds(mulhi(x(7), tg1), x(1));
        const std::int16_t m2 = subs(x(5), m1);
        std::int16_t m5 = subs(mulhi(tg1, x(1)), x(7));
        const std::int16_t m7 = adds(mulhi(x(6), tg2), x(2));
        const std::int16_t m3 = subs(mulhi(tg2, x(2)), x(6));
        const std::int16_t top = adds(adds(m0, m4), one);
        m4 = subs(m4, m0);
        const std::int16_t mid = adds(m5, m2);
        m5 = adds(subs(m5, m2), one);
        const std::int16_t sum = adds(m4, m5), diff = subs(m4, m5);
        const std::int16_t p = static_cast<std::int16_t>(adds(sum, mulhi(cos4, sum)) | one);
        const std::int16_t q = static_cast<std::int16_t>(adds(mulhi(cos4, diff), diff) | one);
        const std::int16_t a = adds(adds(adds(x(4), x(0)), m7), r16);
        const std::int16_t b = adds(subs(adds(x(4), x(0)), m7), r15);
        const std::int16_t d = adds(adds(subs(x(0), x(4)), m3), r16);
        const std::int16_t e = adds(subs(subs(x(0), x(4)), m3), r15);
        auto put = [&](int r, std::int16_t v) { out[static_cast<std::ptrdiff_t>(r) * stride + c] = toByte(v); };
        put(0, adds(top, a));
        put(1, adds(d, p));
        put(2, adds(e, q));
        put(3, adds(mid, b));
        put(4, subs(b, mid));
        put(5, subs(e, q));
        put(6, subs(d, p));
        put(7, subs(a, top));
    }
}

// A component's samples brought up to the picture's size the way the game's decoder does it:
// each sample repeated over the pixels it covers.
std::vector<std::uint8_t> upsample(const Component& c, int hMax, int vMax, int outWidth, int outHeight) {
    const int fx = hMax / c.h, fy = vMax / c.v;
    std::vector<std::uint8_t> out(static_cast<std::size_t>(outWidth) * static_cast<std::size_t>(outHeight));
    for (int y = 0; y < outHeight; ++y) {
        for (int x = 0; x < outWidth; ++x) {
            const int sx = std::min(x / fx, c.stride - 1), sy = std::min(y / fy, c.rows - 1);
            out[static_cast<std::size_t>(y) * static_cast<std::size_t>(outWidth) + static_cast<std::size_t>(x)] =
                c.samples[static_cast<std::size_t>(sy) * static_cast<std::size_t>(c.stride) + static_cast<std::size_t>(sx)];
        }
    }
    return out;
}

std::uint16_t be16(std::span<const std::uint8_t> d, std::size_t at) {
    return static_cast<std::uint16_t>(d[at] << 8 | d[at + 1]);
}

} // namespace

std::optional<tga::Pixels> decode(std::span<const std::uint8_t> data, std::uint32_t maxSide) {
    if (data.size() < 4 || data[0] != 0xFF || data[1] != 0xD8) {
        return std::nullopt;
    }
    std::array<std::array<std::uint16_t, 64>, 4> quant{};
    std::array<bool, 4> quantDefined{};
    std::array<Huffman, 4> dcTables, acTables;
    std::vector<Component> comps;
    int width = 0, height = 0, restartInterval = 0;
    std::size_t at = 2;
    std::optional<std::size_t> scanStart;
    while (at + 4 <= data.size() && !scanStart) {
        if (data[at] != 0xFF) {
            return std::nullopt;
        }
        const std::uint8_t marker = data[at + 1];
        if (marker == 0xFF) {
            ++at;  // fill byte
            continue;
        }
        if (marker == 0xD9) {
            return std::nullopt;  // ended before any scan
        }
        const std::size_t length = be16(data, at + 2);
        if (length < 2 || at + 2 + length > data.size()) {
            return std::nullopt;
        }
        const auto seg = data.subspan(at + 4, length - 2);
        switch (marker) {
        case 0xDB:  // quantization tables, 8-bit only
            for (std::size_t i = 0; i < seg.size();) {
                const int precision = seg[i] >> 4, id = seg[i] & 15;
                if (precision != 0 || id > 3 || i + 65 > seg.size()) {
                    return std::nullopt;
                }
                for (std::size_t k = 0; k < 64; ++k) {
                    quant[static_cast<std::size_t>(id)][static_cast<std::size_t>(kNaturalOrder[k])] = seg[i + 1 + k];
                }
                quantDefined[static_cast<std::size_t>(id)] = true;
                i += 65;
            }
            break;
        case 0xC4:  // Huffman tables
            for (std::size_t i = 0; i < seg.size();) {
                if (i + 17 > seg.size()) {
                    return std::nullopt;
                }
                const int kind = seg[i] >> 4, id = seg[i] & 15;
                std::array<std::uint8_t, 16> counts{};
                std::size_t total = 0;
                for (std::size_t k = 0; k < 16; ++k) {
                    counts[k] = seg[i + 1 + k];
                    total += counts[k];
                }
                if (kind > 1 || id > 3 || total > 256 || i + 17 + total > seg.size()) {
                    return std::nullopt;
                }
                std::vector<std::uint8_t> values(seg.begin() + static_cast<std::ptrdiff_t>(i + 17), seg.begin() + static_cast<std::ptrdiff_t>(i + 17 + total));
                if (!build((kind == 0 ? dcTables : acTables)[static_cast<std::size_t>(id)], counts, std::move(values))) {
                    return std::nullopt;
                }
                i += 17 + total;
            }
            break;
        case 0xC0:
        case 0xC1: {  // baseline / extended sequential, Huffman
            if (seg.size() < 6 || seg[0] != 8) {
                return std::nullopt;
            }
            height = be16(seg, 1);
            width = be16(seg, 3);
            const int count = seg[5];
            if (width <= 0 || height <= 0 || static_cast<std::uint32_t>(width) > maxSide || static_cast<std::uint32_t>(height) > maxSide ||
                static_cast<std::uint64_t>(width) * static_cast<std::uint64_t>(height) > kMaxPixels || (count != 1 && count != 3) ||
                seg.size() < 6 + 3 * static_cast<std::size_t>(count)) {
                return std::nullopt;
            }
            comps.clear();
            for (int i = 0; i < count; ++i) {
                Component c;
                c.id = seg[6 + 3 * static_cast<std::size_t>(i)];
                c.h = seg[7 + 3 * static_cast<std::size_t>(i)] >> 4;
                c.v = seg[7 + 3 * static_cast<std::size_t>(i)] & 15;
                c.quant = seg[8 + 3 * static_cast<std::size_t>(i)];
                if (c.h < 1 || c.h > 2 || c.v < 1 || c.v > 2 || c.quant > 3) {
                    return std::nullopt;
                }
                comps.push_back(c);
            }
            break;
        }
        case 0xC2: case 0xC3: case 0xC5: case 0xC6: case 0xC7: case 0xC9: case 0xCA: case 0xCB: case 0xCD: case 0xCE: case 0xCF:
            return std::nullopt;  // progressive, lossless, hierarchical or arithmetic coded
        case 0xDD:
            if (seg.size() < 2) {
                return std::nullopt;
            }
            restartInterval = be16(seg, 0);
            break;
        case 0xDA: {  // start of scan: all components, interleaved, whole spectrum
            if (comps.empty() || seg.empty() || seg[0] != comps.size() || seg.size() < 1 + 2 * comps.size() + 3) {
                return std::nullopt;
            }
            for (std::size_t i = 0; i < comps.size(); ++i) {
                const int id = seg[1 + 2 * i], tables = seg[2 + 2 * i];
                auto c = std::find_if(comps.begin(), comps.end(), [&](const Component& x) { return x.id == id; });
                if (c == comps.end() || (tables >> 4) > 3 || (tables & 15) > 3) {
                    return std::nullopt;
                }
                c->dc = tables >> 4;
                c->ac = tables & 15;
            }
            const std::size_t s = 1 + 2 * comps.size();
            if (seg[s] != 0 || seg[s + 1] != 63 || seg[s + 2] != 0) {
                return std::nullopt;
            }
            scanStart = at + 2 + length;
            break;
        }
        default:
            break;  // application data, comments: skipped
        }
        at += 2 + length;
    }
    if (!scanStart || comps.empty()) {
        return std::nullopt;
    }
    int hMax = 1, vMax = 1;
    for (const auto& c : comps) {
        hMax = std::max(hMax, c.h);
        vMax = std::max(vMax, c.v);
        if (!quantDefined[static_cast<std::size_t>(c.quant)] || !dcTables[static_cast<std::size_t>(c.dc)].defined ||
            !acTables[static_cast<std::size_t>(c.ac)].defined) {
            return std::nullopt;
        }
    }
    const bool single = comps.size() == 1;
    const int mcuWidth = single ? 8 : 8 * hMax, mcuHeight = single ? 8 : 8 * vMax;
    const int mcusX = (width + mcuWidth - 1) / mcuWidth, mcusY = (height + mcuHeight - 1) / mcuHeight;
    for (auto& c : comps) {
        c.width = (width * c.h + hMax - 1) / hMax;
        c.height = (height * c.v + vMax - 1) / vMax;
        c.stride = single ? mcusX * 8 : mcusX * c.h * 8;
        c.rows = single ? mcusY * 8 : mcusY * c.v * 8;
        c.samples.assign(static_cast<std::size_t>(c.stride) * static_cast<std::size_t>(c.rows), 0);
    }

    // The scan: MCUs left to right, top to bottom; each component's blocks in its h x v layout.
    Bits in(data, *scanStart);
    std::array<std::int16_t, 64> coef{};
    int mcusLeft = restartInterval;
    for (int my = 0; my < mcusY; ++my) {
        for (int mx = 0; mx < mcusX; ++mx) {
            if (restartInterval > 0) {
                if (mcusLeft == 0) {
                    in.restart();
                    for (auto& c : comps) c.predictor = 0;
                    mcusLeft = restartInterval;
                }
                --mcusLeft;
            }
            for (auto& c : comps) {
                const int bw = single ? 1 : c.h, bh = single ? 1 : c.v;
                for (int by = 0; by < bh; ++by) {
                    for (int bx = 0; bx < bw; ++bx) {
                        coef.fill(0);
                        const int s = decodeSymbol(in, dcTables[static_cast<std::size_t>(c.dc)]) & 15;
                        // Wraps like the 32-bit original rather than overflowing on broken data.
                        const int diff = s ? extend(in.bits(s), s) : 0;
                        c.predictor = static_cast<int>(static_cast<std::uint32_t>(c.predictor) + static_cast<std::uint32_t>(diff));
                        coef[0] = static_cast<std::int16_t>(c.predictor);
                        for (int k = 1; k < 64;) {
                            const int rs = decodeSymbol(in, acTables[static_cast<std::size_t>(c.ac)]);
                            const int run = rs >> 4, size = rs & 15;
                            if (size) {
                                k += run;
                                coef[static_cast<std::size_t>(kNaturalOrder[static_cast<std::size_t>(std::min(k, 79))])] =
                                    static_cast<std::int16_t>(extend(in.bits(size), size));
                                ++k;
                            } else if (run == 15) {
                                k += 16;
                            } else {
                                break;
                            }
                        }
                        const int x = (mx * bw + bx) * 8, y = (my * bh + by) * 8;
                        inverseDct(coef, quant[static_cast<std::size_t>(c.quant)],
                                   c.samples.data() + static_cast<std::ptrdiff_t>(y) * c.stride + x, c.stride);
                    }
                }
            }
        }
    }

    // Colour: the game's decoder's integer YCbCr to RGB, in its 16-bit steps.
    tga::Pixels out;
    out.width = static_cast<std::uint32_t>(width);
    out.height = static_cast<std::uint32_t>(height);
    out.rgba.assign(static_cast<std::size_t>(width) * static_cast<std::size_t>(height) * 4, 255);
    std::vector<std::vector<std::uint8_t>> planes;
    for (const auto& c : comps) {
        planes.push_back(upsample(c, hMax, vMax, width, height));
    }
    auto byte = [](std::int16_t v) { return static_cast<std::uint8_t>(std::clamp<int>(v, 0, 255)); };
    for (std::size_t i = 0; i < static_cast<std::size_t>(width) * static_cast<std::size_t>(height); ++i) {
        const auto y = static_cast<std::int16_t>(planes[0][i]);
        if (planes.size() == 1) {
            out.rgba[i * 4] = out.rgba[i * 4 + 1] = out.rgba[i * 4 + 2] = static_cast<std::uint8_t>(y);
            continue;
        }
        const auto cb = static_cast<std::int16_t>(planes[1][i] * 16), cr = static_cast<std::int16_t>(planes[2][i] * 16);
        const std::int16_t blue = subs(mulhi(cb, 7264), 227);
        const std::int16_t red = subs(mulhi(cr, 5744), 179);
        const std::int16_t green = sat((183 * static_cast<std::int32_t>(cr) + 88 * static_cast<std::int32_t>(cb) - 556896) >> 12);
        out.rgba[i * 4] = byte(adds(y, red));
        out.rgba[i * 4 + 1] = byte(subs(y, green));
        out.rgba[i * 4 + 2] = byte(adds(y, blue));
    }
    return out;
}

} // namespace ts2::jpeg
