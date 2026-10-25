// PNG files without a library: SDL 1.2 can't save them. The picture is stored, not squeezed.
#include "platform/platform.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <fstream>
#include <vector>

namespace ts2::platform {

namespace {

std::uint32_t crc32(const std::uint8_t* data, std::size_t size, std::uint32_t crc = 0) {
    static const std::array<std::uint32_t, 256> table = [] {
        std::array<std::uint32_t, 256> t{};
        for (std::uint32_t n = 0; n < 256; ++n) {
            std::uint32_t c = n;
            for (int k = 0; k < 8; ++k) {
                c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
            }
            t[n] = c;
        }
        return t;
    }();
    crc = ~crc;
    for (std::size_t i = 0; i < size; ++i) {
        crc = table[(crc ^ data[i]) & 0xFF] ^ (crc >> 8);
    }
    return ~crc;
}

void put32(std::vector<std::uint8_t>& out, std::uint32_t v) {
    out.push_back(static_cast<std::uint8_t>(v >> 24));
    out.push_back(static_cast<std::uint8_t>(v >> 16));
    out.push_back(static_cast<std::uint8_t>(v >> 8));
    out.push_back(static_cast<std::uint8_t>(v));
}

// A chunk: its length, its name and data, and the CRC of those two.
void chunk(std::vector<std::uint8_t>& out, const char (&name)[5], const std::vector<std::uint8_t>& data) {
    put32(out, static_cast<std::uint32_t>(data.size()));
    const std::size_t from = out.size();
    out.insert(out.end(), name, name + 4);
    out.insert(out.end(), data.begin(), data.end());
    put32(out, crc32(out.data() + from, out.size() - from));
}

} // namespace

bool savePng(const std::filesystem::path& path, const std::uint8_t* rgba, unsigned width, unsigned height) {
    if (width == 0 || height == 0) {
        return false;
    }
    // Each row has a byte before it naming its filter (0, none).
    std::vector<std::uint8_t> raw;
    raw.reserve((std::size_t{width} * 4 + 1) * height);
    for (unsigned y = 0; y < height; ++y) {
        raw.push_back(0);
        raw.insert(raw.end(), rgba + std::size_t{y} * width * 4, rgba + std::size_t{y + 1} * width * 4);
    }
    // zlib's stream of stored blocks of up to 65535 bytes, and the Adler-32 of what they hold.
    std::vector<std::uint8_t> z{0x78, 0x01};
    std::uint32_t a = 1, b = 0;
    for (const std::uint8_t byte : raw) {
        a = (a + byte) % 65521;
        b = (b + a) % 65521;
    }
    for (std::size_t at = 0; at < raw.size();) {
        const std::size_t n = std::min<std::size_t>(65535, raw.size() - at);
        z.push_back(at + n == raw.size() ? 1 : 0);
        z.push_back(static_cast<std::uint8_t>(n));
        z.push_back(static_cast<std::uint8_t>(n >> 8));
        z.push_back(static_cast<std::uint8_t>(~n));
        z.push_back(static_cast<std::uint8_t>(~n >> 8));
        z.insert(z.end(), raw.begin() + static_cast<std::ptrdiff_t>(at), raw.begin() + static_cast<std::ptrdiff_t>(at + n));
        at += n;
    }
    put32(z, (b << 16) | a);

    std::vector<std::uint8_t> file{0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n'};
    std::vector<std::uint8_t> header;
    put32(header, width);
    put32(header, height);
    header.insert(header.end(), {8, 6, 0, 0, 0});  // 8 bits a channel, RGBA, the one method and filter, no interlacing
    chunk(file, "IHDR", header);
    chunk(file, "IDAT", z);
    chunk(file, "IEND", {});
    std::ofstream out(path, std::ios::binary);
    out.write(reinterpret_cast<const char*>(file.data()), static_cast<std::streamsize>(file.size()));
    return static_cast<bool>(out);
}

} // namespace ts2::platform
