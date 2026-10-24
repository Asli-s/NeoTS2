#include "formats/cursor.hpp"

#include "formats/binary_reader.hpp"

#include <algorithm>
#include <exception>

namespace ts2::cursor {

namespace {

constexpr std::uint16_t kCursorType = 2;
constexpr std::uint32_t kInfoHeaderSize = 40;

// Bytes in one row of a bitmap, padded to 4 bytes.
std::size_t rowBytes(std::uint32_t width, std::uint32_t bits) {
    return (static_cast<std::size_t>(width) * bits + 31) / 32 * 4;
}

} // namespace

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
