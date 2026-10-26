#include "formats/cursor.hpp"

#include "formats/binary_reader.hpp"

#include <algorithm>
#include <exception>
#include <stdexcept>
#include <vector>

namespace ts2::cursor {

namespace {

constexpr std::uint16_t kCursorType = 2;
constexpr std::uint32_t kInfoHeaderSize = 40;

// Bytes in one row of a bitmap, padded to 4 bytes.
std::size_t rowBytes(std::uint32_t width, std::uint32_t bits) {
    return (static_cast<std::size_t>(width) * bits + 31) / 32 * 4;
}

constexpr std::uint32_t fourCc(const char (&c)[5]) {
    return static_cast<std::uint32_t>(static_cast<std::uint8_t>(c[0])) | static_cast<std::uint32_t>(static_cast<std::uint8_t>(c[1])) << 8 |
           static_cast<std::uint32_t>(static_cast<std::uint8_t>(c[2])) << 16 | static_cast<std::uint32_t>(static_cast<std::uint8_t>(c[3])) << 24;
}

// The RIFF chunk ids an .ani file uses.
constexpr std::uint32_t kRiff = fourCc("RIFF"), kAcon = fourCc("ACON"), kAnih = fourCc("anih"), kRate = fourCc("rate");
constexpr std::uint32_t kSequence = fourCc("seq "), kList = fourCc("LIST"), kFrames = fourCc("fram"), kIcon = fourCc("icon");

// The anih header's flag for pictures stored as .cur or .ico files (not raw bitmaps).
constexpr std::uint32_t kFramesAreIcons = 1;
constexpr std::size_t kAnihSize = 36;

// Calls `chunk(id, data)` for each chunk in a RIFF chunk list. Each chunk is padded to an even size.
template <typename Chunk>
void forEachChunk(std::span<const std::uint8_t> list, Chunk&& chunk) {
    BinaryReader r(list);
    while (r.remaining() >= 8) {
        const std::uint32_t id = r.u32(), size = r.u32();
        chunk(id, r.bytes(size));
        if (size % 2 != 0 && r.remaining() > 0) {
            r.skip(1);
        }
    }
}

// A chunk of u32 values, one per step.
std::vector<std::uint32_t> perStep(std::span<const std::uint8_t> chunk, std::uint32_t steps) {
    if (chunk.size() < static_cast<std::size_t>(steps) * 4) {
        throw std::runtime_error("ani: a per-step chunk is too short");
    }
    BinaryReader r(chunk);
    std::vector<std::uint32_t> out(steps);
    for (auto& v : out) {
        v = r.u32();
    }
    return out;
}

} // namespace

std::optional<AnimatedCursor> decodeAnimated(std::span<const std::uint8_t> data) {
    try {
        BinaryReader r(data);
        if (r.u32() != kRiff) {
            return std::nullopt;
        }
        // Some files count the 8 bytes before the size in it, the game's hourglass among them, so the
        // size is kept within the file.
        const std::uint32_t stated = r.u32();
        const std::size_t size = std::min<std::size_t>(stated, r.remaining());
        if (size < 4 || r.u32() != kAcon) {
            return std::nullopt;
        }
        std::optional<std::span<const std::uint8_t>> header, rates, sequence;
        AnimatedCursor out;
        bool broken = false;
        forEachChunk(r.bytes(size - 4), [&](std::uint32_t id, std::span<const std::uint8_t> chunk) {
            if (id == kAnih) {
                header = chunk;
            } else if (id == kRate) {
                rates = chunk;
            } else if (id == kSequence) {
                sequence = chunk;
            } else if (id == kList && chunk.size() >= 4 && BinaryReader(chunk).u32() == kFrames) {
                forEachChunk(chunk.subspan(4), [&](std::uint32_t frameId, std::span<const std::uint8_t> frame) {
                    if (frameId != kIcon || broken) {
                        return;
                    }
                    auto cursor = out.frames.size() < kMaxFrames ? decode(frame) : std::nullopt;
                    if (!cursor) {
                        broken = true;
                        return;
                    }
                    out.frames.push_back(std::move(*cursor));
                });
            }
        });
        if (broken || !header || header->size() < kAnihSize || out.frames.empty()) {
            return std::nullopt;
        }
        BinaryReader h(*header);
        h.skip(4);
        const std::uint32_t frames = h.u32(), steps = h.u32();
        h.skip(16);
        const std::uint32_t rate = h.u32(), flags = h.u32();
        if ((flags & kFramesAreIcons) == 0 || frames != out.frames.size() || steps == 0 || steps > kMaxSteps) {
            return std::nullopt;
        }
        // Without a sequence there is one step per picture.
        const std::vector<std::uint32_t> order = sequence ? perStep(*sequence, steps) : std::vector<std::uint32_t>();
        if (!sequence && steps != frames) {
            return std::nullopt;
        }
        const std::vector<std::uint32_t> times = rates ? perStep(*rates, steps) : std::vector<std::uint32_t>(steps, rate);
        out.steps.resize(steps);
        for (std::uint32_t i = 0; i < steps; ++i) {
            const std::uint32_t frame = sequence ? order[i] : i;
            if (frame >= frames) {
                return std::nullopt;
            }
            // A step of no time would never let the animation move on.
            out.steps[i] = {frame, std::max<std::uint32_t>(times[i], 1)};
        }
        return out;
    } catch (const std::exception&) {
        return std::nullopt;
    }
}

void CursorPlayback::restart() {
    step_ = 0;
    stepEnds_.reset();
}

std::size_t CursorPlayback::step(std::span<const AnimatedCursor::Step> steps, double seconds) {
    constexpr double kJiffySeconds = 1.0 / 60;
    if (steps.empty()) {
        return 0;
    }
    if (step_ >= steps.size()) {
        restart();
    }
    // A step of no time (never from decodeAnimated) counts as one jiffy, so the loop below ends.
    const auto length = [&](std::size_t i) { return std::max<std::uint32_t>(steps[i].jiffies, 1) * kJiffySeconds; };
    if (!stepEnds_ || seconds - *stepEnds_ > 1) {
        stepEnds_ = seconds + length(step_);
    }
    while (seconds >= *stepEnds_) {
        step_ = (step_ + 1) % steps.size();
        *stepEnds_ += length(step_);
    }
    return step_;
}

std::optional<Cursor> decode(std::span<const std::uint8_t> data) {
    try {
        BinaryReader r(data);
        if (r.u16() != 0 || r.u16() != kCursorType || r.u16() == 0) {
            return std::nullopt;
        }
        // The first entry: its hotspot and where its picture is.
        r.skip(4);
        Cursor out;
        out.hotX = r.u16();
        out.hotY = r.u16();
        const std::uint32_t size = r.u32(), offset = r.u32();
        if (offset > data.size() || size > data.size() - offset || size < kInfoHeaderSize) {
            return std::nullopt;
        }
        const auto image = data.subspan(offset, size);
        BinaryReader b(image);
        if (b.u32() != kInfoHeaderSize) {
            return std::nullopt;
        }
        const std::int32_t width = b.i32(), doubled = b.i32();
        b.skip(2);
        const std::uint16_t bits = b.u16();
        const std::uint32_t compression = b.u32();
        b.skip(12);
        std::uint32_t colors = b.u32();
        b.skip(4);
        // The height counts the color rows and the mask rows.
        if (width <= 0 || doubled <= 0 || doubled % 2 != 0 || compression != 0 || static_cast<std::uint32_t>(width) > kMaxSide ||
            static_cast<std::uint32_t>(doubled / 2) > kMaxSide) {
            return std::nullopt;
        }
        if (bits != 1 && bits != 4 && bits != 8 && bits != 24 && bits != 32) {
            return std::nullopt;
        }
        const std::uint32_t w = static_cast<std::uint32_t>(width), h = static_cast<std::uint32_t>(doubled / 2);
        if (bits <= 8 && (colors == 0 || colors > (1u << bits))) {
            colors = 1u << bits;
        } else if (bits > 8) {
            colors = 0;
        }
        const auto palette = b.bytes(static_cast<std::size_t>(colors) * 4);
        const std::size_t colorRow = rowBytes(w, bits), maskRow = rowBytes(w, 1);
        const auto colorRows = b.bytes(colorRow * h);
        const auto maskRows = b.bytes(maskRow * h);
        // A hotspot outside the picture is kept on its edge.
        out.hotX = std::min(out.hotX, w - 1);
        out.hotY = std::min(out.hotY, h - 1);
        out.pixels.width = w;
        out.pixels.height = h;
        out.pixels.rgba.assign(static_cast<std::size_t>(w) * h * 4, 0);
        bool anyAlpha = false;
        for (std::uint32_t y = 0; y < h; ++y) {
            // Rows are stored from the bottom up.
            const auto row = colorRows.subspan(static_cast<std::size_t>(h - 1 - y) * colorRow, colorRow);
            const auto mask = maskRows.subspan(static_cast<std::size_t>(h - 1 - y) * maskRow, maskRow);
            for (std::uint32_t x = 0; x < w; ++x) {
                std::uint8_t bgra[4] = {0, 0, 0, 255};
                if (bits <= 8) {
                    const std::size_t bit = static_cast<std::size_t>(x) * bits;
                    const std::uint32_t index = (row[bit / 8] >> (8 - bits - bit % 8)) & ((1u << bits) - 1);
                    if (index >= colors) {
                        return std::nullopt;
                    }
                    std::copy_n(&palette[static_cast<std::size_t>(index) * 4], 3, bgra);
                } else {
                    std::copy_n(&row[static_cast<std::size_t>(x) * (bits / 8)], bits / 8, bgra);
                    anyAlpha = anyAlpha || (bits == 32 && bgra[3] != 0);
                }
                const bool masked = (mask[x / 8] >> (7 - x % 8)) & 1;
                auto* to = &out.pixels.rgba[(static_cast<std::size_t>(y) * w + x) * 4];
                to[0] = bgra[2];
                to[1] = bgra[1];
                to[2] = bgra[0];
                to[3] = masked ? 0 : bits == 32 ? bgra[3] : 255;
            }
        }
        // A 32-bit picture without any alpha is drawn by its mask alone.
        if (bits == 32 && !anyAlpha) {
            for (std::uint32_t y = 0; y < h; ++y) {
                const auto mask = maskRows.subspan(static_cast<std::size_t>(h - 1 - y) * maskRow, maskRow);
                for (std::uint32_t x = 0; x < w; ++x) {
                    out.pixels.rgba[(static_cast<std::size_t>(y) * w + x) * 4 + 3] = ((mask[x / 8] >> (7 - x % 8)) & 1) ? 0 : 255;
                }
            }
        }
        return out;
    } catch (const std::exception&) {
        return std::nullopt;
    }
}

} // namespace ts2::cursor
