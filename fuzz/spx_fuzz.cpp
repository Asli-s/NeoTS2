#include "audio/spx.hpp"

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <exception>
#include <span>

// Decodes any bytes as an SPX1 voice. A voice that decodes is mono and never longer than its header says.
extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
    try {
        const ts2::audio::Pcm pcm = ts2::audio::decodeSpx(std::span<const std::uint8_t>(data, size));
        if (pcm.channels != 1) {
            std::abort();
        }
        const std::size_t declared = static_cast<std::size_t>(data[5] | data[6] << 8 | data[7] << 16) |
                                     static_cast<std::size_t>(data[8]) << 24;
        if (pcm.samples.size() * 2 > declared) {
            std::abort();
        }
    } catch (const std::exception&) {
    }
    return 0;
}
