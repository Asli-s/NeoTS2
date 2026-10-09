#include "formats/qfs.hpp"

#include <algorithm>
#include <stdexcept>
#include <string>

namespace ts2::qfs {

namespace {

constexpr std::size_t kHeaderSize = 9;

[[noreturn]] void fail(const std::string& message) {
    throw std::runtime_error("QFS: " + message);
}

} // namespace

bool isCompressed(std::span<const std::uint8_t> data) {
    return data.size() >= kHeaderSize && data[4] == 0x10 && data[5] == 0xFB;
}

std::vector<std::uint8_t> decompress(std::span<const std::uint8_t> in) {
    if (!isCompressed(in)) {
        fail("missing 0x10FB signature");
    }

    const std::size_t outSize = static_cast<std::size_t>(in[6]) << 16
                              | static_cast<std::size_t>(in[7]) << 8
                              | static_cast<std::size_t>(in[8]);
    std::vector<std::uint8_t> out(outSize);

    std::size_t src = kHeaderSize;
    std::size_t dst = 0;

    auto need = [&](std::size_t count) {
        if (count > in.size() - src) {
            fail("compressed data ends too early");
        }
    };

    auto copyLiteral = [&](std::size_t count) {
        need(count);
        if (count > outSize - dst) {
            fail("literal run overflows output");
        }
        std::copy_n(in.begin() + src, count, out.begin() + dst);
        src += count;
        dst += count;
    };

    auto copyBackReference = [&](std::size_t offset, std::size_t count) {
        if (offset > dst) {
            fail("back-reference points before start of output");
        }
        if (count > outSize - dst) {
            fail("back-reference overflows output");
        }
        for (std::size_t i = 0; i < count; ++i, ++dst) {
            out[dst] = out[dst - offset];
        }
    };

    while (dst < outSize) {
        need(1);
        const std::uint8_t b0 = in[src++];

        std::size_t literal = 0;
        std::size_t copy = 0;
        std::size_t offset = 0;

        if (b0 < 0x80) {
            need(1);
            const std::uint8_t b1 = in[src++];
            literal = b0 & 0x03;
            copy = ((b0 & 0x1C) >> 2) + 3;
            offset = ((b0 & 0x60) << 3) + b1 + 1;
        } else if (b0 < 0xC0) {
            need(2);
            const std::uint8_t b1 = in[src++];
            const std::uint8_t b2 = in[src++];
            literal = (b1 & 0xC0) >> 6;
            copy = (b0 & 0x3F) + 4;
            offset = ((b1 & 0x3F) << 8) + b2 + 1;
        } else if (b0 < 0xE0) {
            need(3);
            const std::uint8_t b1 = in[src++];
            const std::uint8_t b2 = in[src++];
            const std::uint8_t b3 = in[src++];
            literal = b0 & 0x03;
            copy = ((b0 & 0x0C) << 6) + b3 + 5;
            offset = ((b0 & 0x10) << 12) + (b1 << 8) + b2 + 1;
        } else if (b0 < 0xFC) {
            literal = ((b0 & 0x1F) << 2) + 4;
        } else {
            copyLiteral(b0 & 0x03);
            break;
        }

        copyLiteral(literal);
        if (copy > 0) {
            copyBackReference(offset, copy);
        }
    }

    if (dst != outSize) {
        fail("stream ended after " + std::to_string(dst) + " of " +
             std::to_string(outSize) + " bytes");
    }
    return out;
}

} // namespace ts2::qfs
