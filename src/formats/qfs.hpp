#pragma once

#include <cstdint>
#include <span>
#include <vector>

// QFS, the compression the game uses for resources in packages.
namespace ts2::qfs {

bool isCompressed(std::span<const std::uint8_t> data);

std::vector<std::uint8_t> decompress(std::span<const std::uint8_t> data);

// The largest input the stored form can describe: its size field is 3 bytes.
constexpr std::size_t kMaxInput = 0xFFFFFF;

// Compresses `data` byte for byte as the game does when it writes a package. The result is the
// stored form: its own total size (4 bytes, little-endian), 0x10FB, the unpacked size (3 bytes,
// big-endian) and the packed stream. Inputs over 250,000 bytes use the game's faster search,
// which gives a different (still valid) stream. Throws when `data` is larger than kMaxInput.
std::vector<std::uint8_t> compress(std::span<const std::uint8_t> data);

} // namespace ts2::qfs
