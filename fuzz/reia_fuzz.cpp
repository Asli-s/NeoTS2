#include "formats/reia.hpp"

#include <cstddef>
#include <cstdint>
#include <span>

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
    const auto video = ts2::reia::parse(std::span<const std::uint8_t>(data, size));
    ts2::tga::Pixels picture;
    for (std::size_t frame = 0; frame < video.frames.size(); ++frame) {
        (void)ts2::reia::decodeFrame(video, frame, picture);
    }
    return 0;
}
