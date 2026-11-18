#include "formats/movie.hpp"

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <stdexcept>
#include <string>

namespace ts2::movie {

namespace {

constexpr std::uint32_t tag(char a, char b, char c, char d) {
    return static_cast<std::uint32_t>(static_cast<std::uint8_t>(a)) | static_cast<std::uint32_t>(static_cast<std::uint8_t>(b)) << 8 |
           static_cast<std::uint32_t>(static_cast<std::uint8_t>(c)) << 16 | static_cast<std::uint32_t>(static_cast<std::uint8_t>(d)) << 24;
}

constexpr std::uint32_t kKeyFrame = tag('M', 'A', 'D', 'k');
constexpr std::uint32_t kPredictedFrame = tag('M', 'A', 'D', 'm');
constexpr std::uint32_t kPassingFrame = tag('M', 'A', 'D', 'e');
constexpr std::uint32_t kSoundHeader = tag('S', 'C', 'H', 'l');
constexpr std::uint32_t kSoundBlock = tag('S', 'C', 'D', 'l');

// A frame's data starts with a 16-byte header: frame time in ms at 6, width at 8, height at 10 and
// the quantiser scale at 13.
constexpr std::size_t kFrameHeader = 16;

std::uint16_t le16(const std::uint8_t* p) {
    return static_cast<std::uint16_t>(p[0] | p[1] << 8);
}

std::uint32_t le32(const std::uint8_t* p) {
    return static_cast<std::uint32_t>(p[0]) | static_cast<std::uint32_t>(p[1]) << 8 | static_cast<std::uint32_t>(p[2]) << 16 |
           static_cast<std::uint32_t>(p[3]) << 24;
}

[[noreturn]] void fail(const std::string& message) {
    throw std::runtime_error("movie: " + message);
}

// The sound header: "PT" and two bytes, then tagged values (tag, length, big-endian value). Tag 0xFD
// opens the list of the sound's own values, which 0x8A or 0xFF closes. 0xFF also ends the header.
// Revision 1 sound without a compression tag is EA ADPCM, the only kind the game's movies hold.
Sound readSoundHeader(std::span<const std::uint8_t> data) {
    Sound sound;
    if (data.size() < 4 || data[0] != 'P' || data[1] != 'T') {
        return sound;
    }
    std::size_t at = 4;
    int revision = -1;
    bool compressed = false;
    std::uint32_t channels = 1, rate = 22050, samples = 0;
    auto value = [&](std::uint32_t& out) {
        if (at >= data.size()) {
            return false;
        }
        const std::size_t length = data[at++];
        if (length > 4 || data.size() - at < length) {
            return false;
        }
        out = 0;
        for (std::size_t i = 0; i < length; ++i) {
            out = out << 8 | data[at++];
        }
        return true;
    };
    bool inHeader = true;
    while (inHeader && at < data.size()) {
        const std::uint8_t outer = data[at++];
        std::uint32_t v = 0;
        if (outer == 0xFF) {
            break;
        }
        if (outer != 0xFD) {
            if (!value(v)) {
                return {};
            }
            continue;
        }
        bool inList = true;
        while (inList && at < data.size()) {
            const std::uint8_t inner = data[at++];
            if (inner == 0xFF) {
                inList = inHeader = false;
                break;
            }
            if (!value(v)) {
                return {};
            }
            switch (inner) {
            case 0x80: revision = static_cast<int>(v); break;
            case 0x82: channels = v; break;
            case 0x83: compressed = true; break;
            case 0x84: rate = v; break;
            case 0x85: samples = v; break;
            case 0x8A: inList = false; break;
            default: break;
            }
        }
    }
    if (revision != 1 || compressed || channels < 1 || channels > 2 || rate < 4000 || rate > 48000) {
        return {};
    }
    sound.channels = static_cast<std::uint16_t>(channels);
    sound.sampleRate = rate;
    sound.samples = samples;
    return sound;
}

// An MPEG-1 run and level code (ISO 11172-2 table B.14) without its sign bit. Run 0 level 1 is
// coded "11" as it is after a block's first coefficient.
struct LevelCode {
    std::uint16_t bits;
    std::uint8_t length, run, level;
};

constexpr LevelCode kLevelCodes[] = {
    {0b11, 2, 0, 1}, {0b011, 3, 1, 1}, {0b0100, 4, 0, 2}, {0b0101, 4, 2, 1}, {0b00101, 5, 0, 3}, {0b00111, 5, 3, 1},
    {0b00110, 5, 4, 1}, {0b000110, 6, 1, 2}, {0b000111, 6, 5, 1}, {0b000101, 6, 6, 1}, {0b000100, 6, 7, 1},
    {0b0000110, 7, 0, 4}, {0b0000100, 7, 2, 2}, {0b0000111, 7, 8, 1}, {0b0000101, 7, 9, 1}, {0b00100110, 8, 0, 5},
    {0b00100001, 8, 0, 6}, {0b00100101, 8, 1, 3}, {0b00100100, 8, 3, 2}, {0b00100111, 8, 10, 1}, {0b00100011, 8, 11, 1},
    {0b00100010, 8, 12, 1}, {0b00100000, 8, 13, 1}, {0b0000001010, 10, 0, 7}, {0b0000001100, 10, 1, 4},
    {0b0000001011, 10, 2, 3}, {0b0000001111, 10, 4, 2}, {0b0000001001, 10, 5, 2}, {0b0000001110, 10, 14, 1},
    {0b0000001101, 10, 15, 1}, {0b0000001000, 10, 16, 1}, {0b000000011101, 12, 0, 8}, {0b000000011000, 12, 0, 9},
    {0b000000010011, 12, 0, 10}, {0b000000010000, 12, 0, 11}, {0b000000011011, 12, 1, 5}, {0b000000010100, 12, 2, 4},
    {0b000000011100, 12, 3, 3}, {0b000000010010, 12, 4, 3}, {0b000000011110, 12, 6, 2}, {0b000000010101, 12, 7, 2},
    {0b000000010001, 12, 8, 2}, {0b000000011111, 12, 17, 1}, {0b000000011010, 12, 18, 1}, {0b000000011001, 12, 19, 1},
    {0b000000010111, 12, 20, 1}, {0b000000010110, 12, 21, 1}, {0b0000000011010, 13, 0, 12}, {0b0000000011001, 13, 0, 13},
    {0b0000000011000, 13, 0, 14}, {0b0000000010111, 13, 0, 15}, {0b0000000010110, 13, 1, 6}, {0b0000000010101, 13, 1, 7},
    {0b0000000010100, 13, 2, 5}, {0b0000000010011, 13, 3, 4}, {0b0000000010010, 13, 5, 3}, {0b0000000010001, 13, 9, 2},
    {0b0000000010000, 13, 10, 2}, {0b0000000011111, 13, 22, 1}, {0b0000000011110, 13, 23, 1}, {0b0000000011101, 13, 24, 1},
    {0b0000000011100, 13, 25, 1}, {0b0000000011011, 13, 26, 1}, {0b00000000011111, 14, 0, 16}, {0b00000000011110, 14, 0, 17},
    {0b00000000011101, 14, 0, 18}, {0b00000000011100, 14, 0, 19}, {0b00000000011011, 14, 0, 20}, {0b00000000011010, 14, 0, 21},
    {0b00000000011001, 14, 0, 22}, {0b00000000011000, 14, 0, 23}, {0b00000000010111, 14, 0, 24}, {0b00000000010110, 14, 0, 25},
    {0b00000000010101, 14, 0, 26}, {0b00000000010100, 14, 0, 27}, {0b00000000010011, 14, 0, 28}, {0b00000000010010, 14, 0, 29},
    {0b00000000010001, 14, 0, 30}, {0b00000000010000, 14, 0, 31}, {0b000000000011000, 15, 0, 32}, {0b000000000010111, 15, 0, 33},
    {0b000000000010110, 15, 0, 34}, {0b000000000010101, 15, 0, 35}, {0b000000000010100, 15, 0, 36}, {0b000000000010011, 15, 0, 37},
    {0b000000000010010, 15, 0, 38}, {0b000000000010001, 15, 0, 39}, {0b000000000010000, 15, 0, 40}, {0b000000000011111, 15, 1, 8},
    {0b000000000011110, 15, 1, 9}, {0b000000000011101, 15, 1, 10}, {0b000000000011100, 15, 1, 11}, {0b000000000011011, 15, 1, 12},
    {0b000000000011010, 15, 1, 13}, {0b000000000011001, 15, 1, 14}, {0b0000000000010011, 16, 1, 15}, {0b0000000000010010, 16, 1, 16},
    {0b0000000000010001, 16, 1, 17}, {0b0000000000010000, 16, 1, 18}, {0b0000000000010100, 16, 6, 3}, {0b0000000000011010, 16, 11, 2},
    {0b0000000000011001, 16, 12, 2}, {0b0000000000011000, 16, 13, 2}, {0b0000000000010111, 16, 14, 2}, {0b0000000000010110, 16, 15, 2},
    {0b0000000000010101, 16, 16, 2}, {0b0000000000011111, 16, 27, 1}, {0b0000000000011110, 16, 28, 1}, {0b0000000000011101, 16, 29, 1},
    {0b0000000000011100, 16, 30, 1}, {0b0000000000011011, 16, 31, 1},
};
constexpr std::uint8_t kEndOfBlock = 0xFE, kEscape = 0xFD, kNoCode = 0xFF;

// For each 16 bits to come, the code they start with: an index into kLevelCodes, kEndOfBlock ("10"),
// kEscape ("000001") or kNoCode.
const std::array<std::uint8_t, 65536>& codeTable() {
    static const auto table = [] {
        std::array<std::uint8_t, 65536> t{};
        t.fill(kNoCode);
        auto put = [&](std::uint32_t bits, int length, std::uint8_t what) {
            const std::uint32_t first = bits << (16 - length), count = 1u << (16 - length);
            std::fill_n(t.begin() + first, count, what);
        };
        for (std::size_t i = 0; i < std::size(kLevelCodes); ++i) {
            put(kLevelCodes[i].bits, kLevelCodes[i].length, static_cast<std::uint8_t>(i));
        }
        put(0b10, 2, kEndOfBlock);
        put(0b000001, 6, kEscape);
        return t;
    }();
    return table;
}

// Coefficient order in the bitstream: scan position to natural index (row * 8 + column). It is the
// usual zigzag with rows and columns swapped.
constexpr std::uint8_t kScan[64] = {0,  8,  1,  2,  9,  16, 24, 17, 10, 3,  4,  11, 18, 25, 32, 40, 33, 26, 19, 12, 5,  6,
                                    13, 20, 27, 34, 41, 48, 56, 49, 42, 35, 28, 21, 14, 7,  15, 22, 29, 36, 43, 50, 57, 58,
                                    51, 44, 37, 30, 23, 31, 38, 45, 52, 59, 60, 53, 46, 39, 47, 54, 61, 62, 55, 63};

// The MPEG-1 default intra quantiser matrix, in natural order.
constexpr std::uint8_t kIntraMatrix[64] = {8,  16, 19, 22, 26, 27, 29, 34, 16, 16, 22, 24, 27, 29, 34, 37, 19, 22, 26, 27, 29, 34,
                                           34, 38, 22, 22, 26, 27, 29, 34, 37, 40, 22, 26, 27, 29, 32, 35, 40, 48, 26, 27, 29, 32,
                                           35, 40, 48, 58, 26, 27, 29, 34, 38, 46, 56, 69, 27, 29, 35, 38, 46, 56, 69, 83};

// The game's scale per coefficient, folded into the quantiser so its inverse DCT needs fewer
// multiplies: 8192 s(u) s(v) with s(0) = 1 and s(k) = sqrt(2) cos(k pi / 16), as the game rounds them.
constexpr std::int32_t kScale[64] = {
    0x2000, 0x1712, 0x187E, 0x1B37, 0x2000, 0x28BA, 0x3B21, 0x73FC, 0x1712, 0x10A2, 0x11A8, 0x139F, 0x1712, 0x1D5D, 0x2AA1, 0x539F,
    0x187E, 0x11A8, 0x12BF, 0x14D4, 0x187E, 0x1F2C, 0x2D41, 0x58C5, 0x1B37, 0x139F, 0x14D4, 0x1725, 0x1B37, 0x22A3, 0x3249, 0x62A3,
    0x2000, 0x1712, 0x187E, 0x1B37, 0x2000, 0x28BA, 0x3B21, 0x73FC, 0x28BA, 0x1D5D, 0x1F2C, 0x22A3, 0x28BA, 0x33D6, 0x4B42, 0x939F,
    0x3B21, 0x2AA1, 0x2D41, 0x3249, 0x3B21, 0x4B42, 0x6D41, 0xD650, 0x73FC, 0x539F, 0x58C5, 0x62A3, 0x73FC, 0x939F, 0xD650, 0x1A463,
};

// The inverse DCT's constants, each exact as a float.
constexpr double kK1 = 0.3826904296875, kK2 = -0.54119873046875, kK3 = 0.7071075439453125, kK4 = -1.3065643310546875;

// Sums wrap at 32 bits as the game's do.
std::int32_t add(std::int32_t a, std::int32_t b) {
    return static_cast<std::int32_t>(static_cast<std::uint32_t>(a) + static_cast<std::uint32_t>(b));
}

std::int32_t sub(std::int32_t a, std::int32_t b) {
    return static_cast<std::int32_t>(static_cast<std::uint32_t>(a) - static_cast<std::uint32_t>(b));
}

// A product truncated toward zero, low 32 bits kept. In double precision, which the game's thread
// keeps from its start, the product is exact before the truncation.
std::int32_t times(std::int32_t v, double k) {
    return static_cast<std::int32_t>(static_cast<std::uint32_t>(static_cast<std::int64_t>(static_cast<double>(v) * k)));
}

// One 1-D pass of the game's inverse DCT, in its order of operations.
void idctPass(const std::int32_t* x, std::int32_t* y) {
    const std::int32_t s17 = add(x[1], x[7]), d17 = sub(x[1], x[7]), d53 = sub(x[5], x[3]), s35 = add(x[3], x[5]);
    const std::int32_t a = times(add(d17, d53), kK1), b = times(d53, kK2), c = times(sub(s17, s35), kK3), d = times(d17, kK4);
    const std::int32_t negAD = sub(sub(0, a), d);
    const std::int32_t p = add(add(s35, s17), negAD), q = add(negAD, c), r = add(c, sub(a, b)), sv = sub(a, b);
    const std::int32_t e0 = add(x[0], x[4]), e1 = sub(x[0], x[4]), e = times(sub(x[2], x[6]), kK3), e3 = add(add(x[6], x[2]), e);
    const std::int32_t f0 = add(e0, e3), f3 = sub(e0, e3), f1 = add(e1, e), f2 = sub(e1, e);
    y[0] = add(f0, p);
    y[1] = add(f1, q);
    y[2] = add(f2, r);
    y[3] = add(f3, sv);
    y[4] = sub(f3, sv);
    y[5] = sub(f2, r);
    y[6] = sub(f1, q);
    y[7] = sub(f0, p);
}

// A work sample's 7-bit value: byte 2 of its 32 bits as a signed byte, clamped to -64..63, plus 64.
// The game's table never fills the entry for 127, so that byte gives 0.
std::uint8_t sample(std::int32_t v) {
    const auto b = static_cast<std::uint8_t>(static_cast<std::uint32_t>(v) >> 16);
    if (b == 127) {
        return 0;
    }
    return static_cast<std::uint8_t>(std::clamp<int>(static_cast<std::int8_t>(b), -64, 63) + 64);
}

// The colour tables of the game's 32-bit conversion. A pixel's three channels add up in one word,
// in fields at bits 23, 11 and 0 of half steps, and each field is clamped back to 0..255.
struct ColourTables {
    std::array<std::uint32_t, 128> y{}, cb{}, cr{};
};

const ColourTables& colourTables() {
    static const ColourTables tables = [] {
        // Full-range BT.601, as the game's float constants hold it.
        constexpr double kCbG = static_cast<double>(-0.3441f), kCbB = static_cast<double>(1.772f);
        constexpr double kCrR = static_cast<double>(-1.402f), kCrG = static_cast<double>(-0.7141f);
        ColourTables t;
        for (int k = 0; k < 128; ++k) {
            const double x = (k - 64) + 0.5;
            auto trunc = [](double v) { return static_cast<std::uint32_t>(static_cast<std::int32_t>(v)); };
            t.y[k] = 0x2C821871u + static_cast<std::uint32_t>(k) * 0x800801u;
            t.cb[k] = ((trunc(kCbG * x) & 0x1FF) << 11) + (trunc(kCbB * x) & 0x1FF);
            t.cr[k] = ((trunc(kCrG * x) & 0x1FF) - (trunc(kCrR * x) << 12)) << 11;
        }
        return t;
    }();
    return tables;
}

std::uint8_t channel(int field, int offset) {
    return static_cast<std::uint8_t>(std::clamp(2 * field - offset, 0, 255));
}
} // namespace

Movie read(std::span<const std::uint8_t> file) {
    if (file.size() > kMaxFileBytes) {
        fail("file is too large");
    }
    Movie movie;
    std::size_t at = 0, chunks = 0;
    bool sizeKnown = false;
    while (file.size() - at >= 8) {
        const std::uint32_t id = le32(&file[at]);
        const std::uint32_t size = le32(&file[at + 4]);
        if (size < 8 || size > file.size() - at) {
            break;  // the rest is not chunks
        }
        if (++chunks > kMaxChunks) {
            fail("too many chunks");
        }
        const Chunk data{static_cast<std::uint32_t>(at + 8), size - 8};
        if (id == kKeyFrame || id == kPredictedFrame || id == kPassingFrame) {
            if (data.size >= kFrameHeader) {
                const std::uint8_t* h = &file[data.offset];
                const std::uint16_t w = le16(h + 8), hgt = le16(h + 10);
                if (!sizeKnown) {
                    // The game writes whole 16 x 16 macroblocks, so other sides would run past its picture.
                    if (w < 16 || hgt < 16 || w > kMaxSide || hgt > kMaxSide || w % 16 != 0 || hgt % 16 != 0) {
                        fail("bad picture size " + std::to_string(w) + " x " + std::to_string(hgt));
                    }
                    movie.width = w;
                    movie.height = hgt;
                    movie.frameMs = le16(h + 6);
                    sizeKnown = true;
                }
                if (w == movie.width && hgt == movie.height) {
                    const FrameKind kind = id == kKeyFrame ? FrameKind::Key : id == kPredictedFrame ? FrameKind::Predicted : FrameKind::Passing;
                    movie.frames.push_back({kind, data});
                }
            }
        } else if (id == kSoundHeader && movie.sound.channels == 0 && movie.sound.blocks.empty()) {
            movie.sound = readSoundHeader(file.subspan(data.offset, data.size));
        } else if (id == kSoundBlock && movie.sound.channels != 0) {
            movie.sound.blocks.push_back(data);
        }
        at += size;
    }
    if (movie.frames.empty()) {
        fail("no video frames");
    }
    return movie;
}

// The bitstream is little-endian 16-bit words read high bit first. Past its end the reader gets
// zeros (the game reads on into memory after the frame).
std::uint32_t VideoDecoder::peek(int count) const {
    std::uint64_t window = 0;
    const std::size_t word = bitPos_ >> 4;
    for (std::size_t i = 0; i < 3; ++i) {
        const std::size_t at = (word + i) * 2;
        const std::uint32_t low = at < bits_.size() ? bits_[at] : 0u, high = at + 1 < bits_.size() ? bits_[at + 1] : 0u;
        window = window << 16 | (high << 8 | low);
    }
    return static_cast<std::uint32_t>(window >> (48 - (bitPos_ & 15) - static_cast<std::size_t>(count))) & ((1u << count) - 1);
}

void VideoDecoder::skip(int count) {
    bitPos_ += static_cast<std::size_t>(count);
}

// A motion value: "0" is 0, "1" and 5 bits b give b + 1 below 16, else b - 32.
int VideoDecoder::motion() {
    if (peek(1) == 0) {
        skip(1);
        return 0;
    }
    const int b = static_cast<int>(peek(6) & 0x1F);
    skip(6);
    return b < 16 ? b + 1 : b - 32;
}

// A byte of the reference picture. The game reads outside it too, from memory nearby, so those
// bytes are taken as the picture's starting black (0).
std::uint8_t VideoDecoder::refByte(std::ptrdiff_t at) const {
    return at >= 0 && static_cast<std::size_t>(at) < reference_.size() ? reference_[static_cast<std::size_t>(at)] : 0;
}

// A predicted block copies the reference picture at the macroblock's whole-pixel vector and adds
// one value. Addressing is by the packed rows, so a vector past the left or right edge reads the
// next row's pixels, as the game's does.
void VideoDecoder::predictLuma(std::size_t at, int bx, int by, std::uint8_t add) {
    const auto row = static_cast<std::ptrdiff_t>(width_) * 2;
    for (int py = 0; py < 8; ++py) {
        for (int px = 0; px < 8; ++px) {
            const int x = bx + mvx_ + px, y = by + mvy_ + py;
            const std::ptrdiff_t src = static_cast<std::ptrdiff_t>(at) + y * row + (x >> 1) * 4 + (3 - (x & 1));
            luma_[static_cast<std::size_t>((by + py) * 16 + bx + px)] = static_cast<std::uint8_t>(refByte(src) + add) << 16;
        }
    }
}

// Chroma moves by half the vector across (rounded down) and by the vector's rows of the packed
// picture, every second one.
void VideoDecoder::predictChroma(std::size_t at, bool cb, std::uint8_t add) {
    const auto row = static_cast<std::ptrdiff_t>(width_) * 2;
    const std::ptrdiff_t base = static_cast<std::ptrdiff_t>(at) + mvy_ * row + (mvx_ >> 1) * 4 + (cb ? 1 : 0);
    auto& work = cb ? cb_ : cr_;
    for (int py = 0; py < 8; ++py) {
        for (int px = 0; px < 8; ++px) {
            work[static_cast<std::size_t>(py * 8 + px)] = static_cast<std::uint8_t>(refByte(base + 2 * py * row + px * 4) + add) << 16;
        }
    }
}

// A block coded by its coefficients: a signed 8-bit DC, then MPEG-1 run and level codes until "10".
// False when the data is broken: a code the table lacks or a run past the 64th coefficient.
bool VideoDecoder::codedBlock(std::int32_t* work, std::size_t stride) {
    std::array<std::int32_t, 64> coef{};
    coef[0] = static_cast<std::int8_t>(peek(8)) * quant_[0];
    skip(8);
    const auto& table = codeTable();
    std::size_t pos = 1;
    for (;;) {
        const std::uint8_t what = table[peek(16)];
        int run = 0, level = 0;
        if (what == kEndOfBlock) {
            skip(2);
            break;
        }
        if (what == kNoCode) {
            return false;
        }
        if (what == kEscape) {
            skip(6);
            level = static_cast<int>(peek(10));
            level = level >= 512 ? level - 1024 : level;
            skip(10);
            run = static_cast<int>(peek(6));
            skip(6);
        } else {
            const LevelCode& c = kLevelCodes[what];
            skip(c.length);
            run = c.run;
            level = peek(1) ? -c.level : c.level;
            skip(1);
        }
        pos += static_cast<std::size_t>(run);
        if (pos > 63) {
            return false;
        }
        const std::size_t k = kScan[pos];
        coef[k] = static_cast<std::int32_t>(static_cast<std::uint32_t>(level) * static_cast<std::uint32_t>(quant_[k]));
        ++pos;
    }
    std::array<std::int32_t, 64> temp{};
    std::int32_t y[8];
    for (std::size_t r = 0; r < 8; ++r) {
        idctPass(&coef[r * 8], y);
        for (std::size_t k = 0; k < 8; ++k) {
            temp[k * 8 + r] = y[k];
        }
    }
    for (std::size_t k = 0; k < 8; ++k) {
        idctPass(&temp[k * 8], y);
        for (std::size_t j = 0; j < 8; ++j) {
            work[k * stride + j] = y[j];
        }
    }
    return true;
}

// One 16 x 16 macroblock. An inter frame's macroblock starts with which of its blocks are
// predicted: "00" none, "1" all, "01" and a 6-bit mask (Y0 Y1 Y2 Y3 Cb Cr from its low bit). A
// vector follows unless none are.
bool VideoDecoder::macroblock(std::size_t mbX, std::size_t mbY) {
    unsigned mask = 0;
    if (inter_) {
        const std::uint32_t head = peek(8);
        if ((head & 0xC0) == 0) {
            skip(2);
        } else if (head & 0x80) {
            mask = 0x3F;
            skip(1);
        } else {
            mask = head;  // "01" sets bit 6, which names no block, so the vector is read even for an empty mask
            skip(8);
        }
        if (mask != 0) {
            mvx_ = motion();
            mvy_ = motion();
        }
    }
    const std::size_t at = (mbY * 16 * width_ + mbX * 16) * 2;  // in the packed picture
    constexpr int kBlockX[4] = {0, 8, 0, 8}, kBlockY[4] = {0, 0, 8, 8};
    for (int b = 0; b < 6; ++b) {
        if (mask & (1u << b)) {
            const int delta = motion();
            if (quality_ == Quality::Fine) {
                predictFine(mbX, mbY, b, delta);
            } else if (b < 4) {
                predictLuma(at, kBlockX[b], kBlockY[b], static_cast<std::uint8_t>(delta - 64));
            } else {
                predictChroma(at, b == 4, static_cast<std::uint8_t>(delta - 64));
            }
        } else {
            std::int32_t* work = b < 4 ? &luma_[static_cast<std::size_t>(kBlockY[b] * 16 + kBlockX[b])] : b == 4 ? cb_.data() : cr_.data();
            if (!codedBlock(work, b < 4 ? 16 : 8)) {
                return false;
            }
        }
    }
    store(mbX, mbY);
    return true;
}

namespace {

// A work sample in full 8 bits. The inverse DCT gives the 7-bit sample in 16.16 fixed point, so the
// 8-bit sample is its value over 32768, rounded.
std::uint8_t fineSample(std::int32_t v) {
    return static_cast<std::uint8_t>(std::clamp<std::int64_t>((static_cast<std::int64_t>(v) + 16384) >> 15, -128, 127) + 128);
}

// The work value a predicted 8-bit sample stands for, so fineSample gives it back.
std::int32_t fineWork(int sample) {
    return (std::clamp(sample, 0, 255) - 128) * 32768;
}

} // namespace

void VideoDecoder::store(std::size_t mbX, std::size_t mbY) {
    if (quality_ == Quality::Fine) {
        const std::size_t w = width_, cw = w / 2, chroma = std::size_t{width_} * height_;
        for (std::size_t r = 0; r < 16; ++r) {
            std::uint8_t* row = out_.data() + (mbY * 16 + r) * w + mbX * 16;
            for (std::size_t c = 0; c < 16; ++c) {
                row[c] = fineSample(luma_[r * 16 + c]);
            }
        }
        for (std::size_t r = 0; r < 8; ++r) {
            const std::size_t at = (mbY * 8 + r) * cw + mbX * 8;
            for (std::size_t c = 0; c < 8; ++c) {
                out_[chroma + at + c] = fineSample(cb_[r * 8 + c]);
                out_[chroma + chroma / 4 + at + c] = fineSample(cr_[r * 8 + c]);
            }
        }
        return;
    }
    // Rows 2j and 2j + 1 share chroma row j.
    const std::size_t row = std::size_t{width_} * 2, at = (mbY * 16 * width_ + mbX * 16) * 2;
    for (std::size_t r = 0; r < 16; ++r) {
        std::uint8_t* dest = out_.data() + at + r * row;
        for (std::size_t k = 0; k < 8; ++k) {
            dest[k * 4 + 0] = sample(cr_[(r >> 1) * 8 + k]);
            dest[k * 4 + 1] = sample(cb_[(r >> 1) * 8 + k]);
            dest[k * 4 + 2] = sample(luma_[r * 16 + 2 * k + 1]);
            dest[k * 4 + 3] = sample(luma_[r * 16 + 2 * k]);
        }
    }
}

// A predicted block in full 8 bits: the reference at the whole-pixel vector (chroma at half of
// it, rounded down, as the game reads it) plus twice the 7-bit add. Reads past the picture's edge
// take the edge's samples.
void VideoDecoder::predictFine(std::size_t mbX, std::size_t mbY, int block, int delta) {
    const int w = width_, h = height_;
    if (block < 4) {
        const int bx = (block & 1) * 8, by = (block & 2) * 4;
        for (int py = 0; py < 8; ++py) {
            const int y = std::clamp(static_cast<int>(mbY) * 16 + by + py + mvy_, 0, h - 1);
            for (int px = 0; px < 8; ++px) {
                const int x = std::clamp(static_cast<int>(mbX) * 16 + bx + px + mvx_, 0, w - 1);
                luma_[static_cast<std::size_t>((by + py) * 16 + bx + px)] = fineWork(reference_[static_cast<std::size_t>(y * w + x)] + 2 * delta);
            }
        }
        return;
    }
    const int cw = w / 2, ch = h / 2;
    const std::size_t plane = std::size_t{width_} * height_ + (block == 5 ? std::size_t{width_} * height_ / 4 : 0);
    auto& work = block == 4 ? cb_ : cr_;
    for (int py = 0; py < 8; ++py) {
        const int y = std::clamp(static_cast<int>(mbY) * 8 + py + (mvy_ >> 1), 0, ch - 1);
        for (int px = 0; px < 8; ++px) {
            const int x = std::clamp(static_cast<int>(mbX) * 8 + px + (mvx_ >> 1), 0, cw - 1);
            work[static_cast<std::size_t>(py * 8 + px)] = fineWork(reference_[plane + static_cast<std::size_t>(y * cw + x)] + 2 * delta);
        }
    }
}

namespace {

// Smooths the edges of a plane's 8 x 8 blocks where the step across an edge is small enough to come
// from quantising rather than from the picture. Where both sides are flat the step is spread over
// three samples each side, elsewhere the two nearest move towards each other by at most `limit`.
void smoothBlockEdges(std::uint8_t* plane, int w, int h, int edge, int flat, int limit) {
    auto filter = [&](std::uint8_t* q0, std::ptrdiff_t step) {
        const int p3 = q0[-4 * step], p2 = q0[-3 * step], p1 = q0[-2 * step], p0 = q0[-step];
        const int q = q0[0], q1 = q0[step], q2 = q0[2 * step], q3 = q0[3 * step];
        if (std::abs(p0 - q) >= edge || std::abs(p1 - p0) >= flat || std::abs(q1 - q) >= flat) {
            return;
        }
        auto put = [](std::uint8_t* at, int v) { *at = static_cast<std::uint8_t>(std::clamp(v, 0, 255)); };
        if (std::abs(p2 - p0) < flat && std::abs(q2 - q) < flat) {
            put(q0 - 3 * step, (2 * p3 + 3 * p2 + p1 + p0 + q + 4) >> 3);
            put(q0 - 2 * step, (p2 + p1 + p0 + q + 2) >> 2);
            put(q0 - step, (p2 + 2 * p1 + 2 * p0 + 2 * q + q1 + 4) >> 3);
            put(q0, (p1 + 2 * p0 + 2 * q + 2 * q1 + q2 + 4) >> 3);
            put(q0 + step, (p0 + q + q1 + q2 + 2) >> 2);
            put(q0 + 2 * step, (2 * q3 + 3 * q2 + q1 + q + p0 + 4) >> 3);
            return;
        }
        const int d = std::clamp(((q - p0) * 4 + (p1 - q1) + 4) >> 3, -limit, limit);
        put(q0 - step, p0 + d);
        put(q0, q - d);
    };
    for (int y = 0; y < h; ++y) {
        for (int x = 8; x < w; x += 8) {
            filter(plane + y * w + x, 1);
        }
    }
    for (int y = 8; y < h; y += 8) {
        for (int x = 0; x < w; ++x) {
            filter(plane + y * w + x, w);
        }
    }
}

} // namespace

void VideoDecoder::rgbaFine(std::vector<std::uint8_t>& out) const {
    const std::vector<std::uint8_t>& planes = shownIsReference_ ? reference_ : out_;
    const int w = width_, h = height_, cw = w / 2, ch = h / 2;
    shown_ = planes;
    // A coarser quantiser leaves larger steps at block edges.
    const int q = std::clamp(quantiser_, 1, 64);
    const int edge = std::min(4 + 2 * q, 80), flat = std::min(2 + q / 2, 24), limit = std::min(1 + q / 3, 16);
    smoothBlockEdges(shown_.data(), w, h, edge, flat, limit);
    std::uint8_t* cb = shown_.data() + std::size_t{width_} * height_;
    std::uint8_t* cr = cb + std::size_t{width_} * height_ / 4;
    smoothBlockEdges(cb, cw, ch, edge, flat, limit);
    smoothBlockEdges(cr, cw, ch, edge, flat, limit);
    out.resize(std::size_t{width_} * height_ * 4);
    // Each chroma sample sits between 2 x 2 pixels, so a pixel takes 3/4 of its own sample and 1/4
    // of the next one towards it, across and down.
    auto chromaAt = [&](const std::uint8_t* c, int x, int y) {
        const int x0 = x >> 1, y0 = y >> 1;
        const int x1 = std::clamp(x & 1 ? x0 + 1 : x0 - 1, 0, cw - 1), y1 = std::clamp(y & 1 ? y0 + 1 : y0 - 1, 0, ch - 1);
        return (9 * c[y0 * cw + x0] + 3 * c[y0 * cw + x1] + 3 * c[y1 * cw + x0] + c[y1 * cw + x1] + 8) >> 4;
    };
    // Full-range BT.601 with the game's own factors, in 16-bit fixed point.
    constexpr int kCrR = 91881, kCbG = 22551, kCrG = 46799, kCbB = 116130;
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            const int luma = shown_[static_cast<std::size_t>(y * w + x)];
            const int u = chromaAt(cb, x, y) - 128, v = chromaAt(cr, x, y) - 128;
            std::uint8_t* px = out.data() + static_cast<std::size_t>(y * w + x) * 4;
            px[0] = static_cast<std::uint8_t>(std::clamp(luma + ((kCrR * v + 32768) >> 16), 0, 255));
            px[1] = static_cast<std::uint8_t>(std::clamp(luma + ((-kCbG * u - kCrG * v + 32768) >> 16), 0, 255));
            px[2] = static_cast<std::uint8_t>(std::clamp(luma + ((kCbB * u + 32768) >> 16), 0, 255));
            px[3] = 255;
        }
    }
}

bool VideoDecoder::decode(std::span<const std::uint8_t> file, const Movie& movie, const VideoFrame& frame) {
    if (frame.chunk.offset > file.size() || frame.chunk.size > file.size() - frame.chunk.offset || frame.chunk.size < kFrameHeader ||
        movie.width % 16 != 0 || movie.height % 16 != 0 || movie.width > kMaxSide || movie.height > kMaxSide) {
        return false;
    }
    if (width_ != movie.width || height_ != movie.height) {
        width_ = movie.width;
        height_ = movie.height;
        // Fine keeps a Y plane and two quarter-size chroma planes, starting black.
        const std::size_t pixels = std::size_t{width_} * height_;
        out_.assign(quality_ == Quality::Fine ? pixels * 3 / 2 : pixels * 2, 0);
        if (quality_ == Quality::Fine) {
            std::fill(out_.begin() + static_cast<std::ptrdiff_t>(pixels), out_.end(), std::uint8_t{128});
        }
        reference_ = out_;
    }
    const std::uint8_t* header = file.data() + frame.chunk.offset;
    // The quantiser: Q[i] = M[i] q S[i] / 16 for the AC coefficients, and 32768 for the DC.
    const std::int32_t q = header[13];
    quantiser_ = q;
    quant_[0] = 32768;
    for (std::size_t i = 1; i < 64; ++i) {
        quant_[i] = static_cast<std::int32_t>(static_cast<std::int64_t>(kIntraMatrix[i]) * q * kScale[i] / 16);
    }
    inter_ = frame.kind != FrameKind::Key;
    bits_ = file.subspan(frame.chunk.offset + kFrameHeader, frame.chunk.size - kFrameHeader);
    bitPos_ = 0;
    mvx_ = mvy_ = 0;
    // Every macroblock reads at least 8 bits per block, so a stream this far past its end is broken.
    const std::size_t limit = bits_.size() * 8 + 4096;
    bool ok = true;
    for (std::size_t y = 0; y < height_ && ok; y += 16) {
        for (std::size_t x = 0; x < width_ && ok; x += 16) {
            ok = macroblock(x / 16, y / 16) && bitPos_ <= limit;
        }
    }
    // A Passing frame is shown but the next one builds on the frame before it.
    if (frame.kind != FrameKind::Passing) {
        std::swap(out_, reference_);
    }
    shownIsReference_ = frame.kind != FrameKind::Passing;
    return ok;
}

void VideoDecoder::rgba(std::vector<std::uint8_t>& out) const {
    if (quality_ == Quality::Fine) {
        rgbaFine(out);
        return;
    }
    const std::vector<std::uint8_t>& packed = shownIsReference_ ? reference_ : out_;
    out.resize(std::size_t{width_} * height_ * 4);
    const ColourTables& t = colourTables();
    for (std::size_t w = 0; w + 4 <= packed.size() && w * 2 + 8 <= out.size(); w += 4) {
        const std::uint32_t chroma = t.cb[packed[w + 1] & 0x7F] + t.cr[packed[w] & 0x7F];
        for (int i = 0; i < 2; ++i) {
            const std::uint32_t u = t.y[packed[w + 3 - i] & 0x7F] + chroma;
            std::uint8_t* px = out.data() + (w / 2 + static_cast<std::size_t>(i)) * 4;
            px[0] = channel(static_cast<int>(u >> 23), 177);
            px[1] = channel(static_cast<int>((u >> 11) & 0x1FF), 133);
            px[2] = channel(static_cast<int>(u & 0x1FF), 225);
            px[3] = 255;
        }
    }
}

} // namespace ts2::movie
