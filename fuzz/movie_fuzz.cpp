#include "audio/movie_sound.hpp"
#include "formats/movie.hpp"

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <exception>
#include <span>
#include <vector>

namespace {

// Every frame decoded and turned to colour, twice: different pixels mean memory read before it
// was written (Windows has no sanitizer for that). Frames are hashed, not kept, and a movie with
// too many pixels in all is cut short, so a small input can't ask for gigabytes.
std::uint64_t pictures(std::span<const std::uint8_t> bytes, const ts2::movie::Movie& movie, ts2::movie::Quality quality) {
    constexpr std::size_t kMaxPixels = std::size_t{1} << 26;
    ts2::movie::VideoDecoder decoder(quality);
    std::vector<std::uint8_t> rgba;
    std::uint64_t hash = 0xCBF29CE484222325ull;
    std::size_t pixels = 0;
    for (const auto& frame : movie.frames) {
        pixels += std::size_t{movie.width} * movie.height;
        if (pixels > kMaxPixels) {
            break;
        }
        (void)decoder.decode(bytes, movie, frame);
        decoder.rgba(rgba);
        if (rgba.size() != std::size_t{movie.width} * movie.height * 4) {
            std::abort();
        }
        for (const std::uint8_t b : rgba) {
            hash = (hash ^ b) * 0x100000001B3ull;
        }
    }
    return hash;
}

} // namespace

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
    const std::span<const std::uint8_t> bytes(data, size);
    try {
        const ts2::movie::Movie movie = ts2::movie::read(bytes);
        if (movie.width % 16 != 0 || movie.height % 16 != 0 || movie.width > ts2::movie::kMaxSide || movie.height > ts2::movie::kMaxSide) {
            std::abort();
        }
        for (const auto quality : {ts2::movie::Quality::Game, ts2::movie::Quality::Fine}) {
            if (pictures(bytes, movie, quality) != pictures(bytes, movie, quality)) {
                std::abort();
            }
        }
        const auto pcm = ts2::audio::decodeMovieSound(bytes, movie.sound);
        if (pcm.channels != 0 && pcm.samples.size() % pcm.channels != 0) {
            std::abort();
        }
    } catch (const std::exception&) {
    }
    return 0;
}
