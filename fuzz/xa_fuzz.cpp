#include "audio/xa.hpp"

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <exception>
#include <span>

// Decodes any bytes as an XA sample. A sample that decodes must hold whole frames and never more
// than its header says.
extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
    try {
        const ts2::audio::Pcm pcm = ts2::audio::decodeXa(std::span<const std::uint8_t>(data, size));
        if (pcm.channels == 0 || pcm.samples.size() % pcm.channels != 0) {
            std::abort();
        }
        const std::size_t declared = static_cast<std::size_t>(data[4] | data[5] << 8 | data[6] << 16) |
                                     static_cast<std::size_t>(data[7]) << 24;
        if (pcm.samples.size() * 2 > declared) {
            std::abort();
        }
    } catch (const std::exception&) {
    }
    return 0;
}
