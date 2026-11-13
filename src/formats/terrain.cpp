#include "formats/terrain.hpp"

#include "formats/binary_reader.hpp"
#include "formats/binary_writer.hpp"

#include <cstring>
#include <stdexcept>

namespace ts2::terrain {

namespace {

constexpr std::uint32_t kMaxSide = 4096;
constexpr std::uint32_t kMaxPaints = 256;

std::string shortString(BinaryReader& r) {
    const auto text = r.bytes(r.u8());
    return std::string(text.begin(), text.end());
}

} // namespace

LotTexture parseLotTexture(std::span<const std::uint8_t> data) {
    BinaryReader r(data);
    r.skip(64);
    LotTexture t;
    r.u32();  // type id
    t.version = r.u32();
    t.width = r.u32();
    t.height = r.u32();
    t.canvas = r.varString();
    const std::uint32_t count = r.u32();
    if (count > kMaxPaints) {
        throw std::runtime_error("lot texture: too many paints");
    }
    for (std::uint32_t i = 0; i < count; ++i) {
        t.paints.push_back(r.varString());
    }
    return t;
}

std::vector<std::uint8_t> writeLotTexture(const LotTexture& texture) {
    BinaryWriter w;
    w.zeros(64);
    w.u32(0x4B58975B);
    w.u32(kLotTextureWrittenVersion);
    w.u32(texture.width);
    w.u32(texture.height);
    w.text(texture.canvas);
    w.count(texture.paints.size());
    for (const std::string& paint : texture.paints) {
        w.text(paint);
    }
    return w.take();
}

std::uint8_t Array2D::byte(std::int32_t x, std::int32_t y) const {
    if (x < 0 || y < 0 || static_cast<std::uint32_t>(x) >= width || static_cast<std::uint32_t>(y) >= height) {
        return 0;
    }
    return elements[(static_cast<std::size_t>(y) * width + static_cast<std::size_t>(x)) * elementSize];
}

std::optional<float> Array2D::f32(std::int32_t x, std::int32_t y) const {
    if (elementSize != 4 || x < 0 || y < 0 || static_cast<std::uint32_t>(x) >= width || static_cast<std::uint32_t>(y) >= height) {
        return std::nullopt;
    }
    const std::size_t at = (static_cast<std::size_t>(y) * width + static_cast<std::size_t>(x)) * 4;
    if (at + 4 > elements.size()) {
        return std::nullopt;
    }
    float f;
    std::memcpy(&f, &elements[at], 4);
    return f;
}

namespace {

Array2D readArray2DHeader(BinaryReader& r) {
    r.skip(64);
    Array2D a;
    r.u32();  // type id
    a.version = r.u32();
    if (shortString(r) != "c2DArray") {
        throw std::runtime_error("c2DArray: unexpected class name");
    }
    a.width = r.u32();
    a.height = r.u32();
    if (a.width == 0 || a.height == 0 || a.width > kMaxSide || a.height > kMaxSide) {
        throw std::runtime_error("c2DArray: bad size");
    }
    return a;
}

} // namespace

Array2D parseArray2D(std::span<const std::uint8_t> data) {
    BinaryReader r(data);
    Array2D a = readArray2DHeader(r);
    const std::size_t cells = static_cast<std::size_t>(a.width) * a.height;
    if (r.remaining() == 0 || r.remaining() % cells != 0) {
        throw std::runtime_error("c2DArray: data does not fill the grid evenly");
    }
    a.elementSize = r.remaining() / cells;
    const auto rest = r.bytes(r.remaining());
    a.elements.assign(rest.begin(), rest.end());
    return a;
}

Array2D parseByteLists(std::span<const std::uint8_t> data) {
    constexpr std::size_t kMaxBytes = 3;
    BinaryReader r(data);
    Array2D a = readArray2DHeader(r);
    const std::size_t cells = static_cast<std::size_t>(a.width) * a.height;
    if (cells > r.remaining()) {
        throw std::runtime_error("c2DArray: too short for its grid");
    }
    a.elementSize = kMaxBytes;
    a.elements.assign(cells * kMaxBytes, 0xFF);
    for (std::size_t i = 0; i < cells; ++i) {
        const std::uint8_t count = r.u8();
        if (count > kMaxBytes) {
            throw std::runtime_error("c2DArray: more than three bytes in a tile");
        }
        for (std::uint8_t k = 0; k < count; ++k) {
            a.elements[i * kMaxBytes + k] = r.u8();
        }
    }
    if (r.remaining() != 0) {
        throw std::runtime_error("c2DArray: data left after the grid");
    }
    return a;
}

} // namespace ts2::terrain
